#pragma once

// ---------------------------------------------------------------------------
// Web radyo.
//
// Cihazda hazir bir akis oynatma yolu zaten var: sunucudan gelen TTS sesi
// Opus paketleri halinde AudioService'e itiliyor. Radyo da ayni yolu
// kullaniyor, tek fark kaynagin HTTP olmasi:
//
//   http->Read(...) -> OggDemuxer::Process(...) -> PushPacketToDecodeQueue()
//
// Upstream'in notify oynaticisi (main/notify/) ayni makineyi kullaniyor;
// demuxer oradan geliyor ve paket suresini Opus TOC baytindan okuyor.
//
// PushPacketToDecodeQueue(paket, wait=true) kuyruk dolunca BLOKLUYOR; bu da
// HTTP okumasini gercek zamana kilitliyor. Akis kontrolu bedavaya geliyor,
// ayrica arabellek yonetmemiz gerekmiyor.
//
// ⚠️ Cozucumuz yalnizca Opus, demuxer yalnizca Ogg. Istasyonlarin cogu
// MP3/AAC/HLS yayinliyor, o yuzden cevrimi SUNUCU yapiyor (ffmpeg, bkz.
// xiaozhi-mcp-search/server.py). Sunucu 24 kHz mono Opus veriyor - cihazin
// cikis hizi da 24 kHz, yeniden ornekleyici devreye girmiyor.
//
// Ses cikisini acmak bizim isimiz degil: oynatma gorevi veri gelince kendi
// aciyor, sessizlikte guc zamanlayicisi kapatiyor.
// ---------------------------------------------------------------------------

#include "application.h"
#include "board.h"
#include "demuxer/ogg_demuxer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

class RadioPlayer {
public:
    struct Station {
        std::string slug;
        std::string name;
        int freq_tenths = 0;  // 890 = 89.0 MHz, 0 = yalnizca internet
    };

    ~RadioPlayer() { Stop(); }

    bool playing() const { return playing_.load(); }
    const std::string& station_name() const { return station_name_; }

    // Calan varsa durdurup yenisini baslatir.
    void Play(const std::string& base_url, const Station& station) {
        PlayUrl(base_url + "/radio?s=" + station.slug, station.name);
    }

    // Herhangi bir Ogg/Opus adresi. Radyo sonsuz akis, sesli bildirim ise
    // kisa bir dosya - ikisi de ayni yol: akis bitince dongu kendi cikiyor.
    void PlayUrl(const std::string& url, const std::string& label) {
        Stop();
        url_ = url;
        station_name_ = label;
        stop_requested_ = false;
        playing_ = true;
        // Demuxer obekte ama TLS el sikismasi hala birkac KB istiyor.
        xTaskCreate(TaskEntry, "akis", 12288, this, 3, &task_);
    }

    void Stop() {
        if (!playing_.load()) {
            return;
        }
        stop_requested_ = true;
        // Gorev okuma/bekleme icinde olabilir; kendi cikip bayragi dusurmesini
        // bekliyoruz. Kuyruk en fazla birkac yuz ms icinde bosaliyor.
        for (int i = 0; i < 60 && playing_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        station_name_.clear();
    }

    // Sunucudaki istasyon listesi. Firmware'e gomulu degil ki istasyon
    // degistirmek icin guncelleme gerekmesin.
    //
    // ⚠️ TLS istiyor: LVGL gorevinden CAGIRMA, onun yigini bu is icin dar.
    // Board bunu acilista kisa omurlu bir gorevde cagirip onbellekliyor.
    static std::vector<Station> FetchList(const std::string& base_url) {
        std::vector<Station> list;
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            return list;
        }
        auto http = network->CreateHttp(0);
        if (http == nullptr) {
            return list;
        }
        if (auto opened = http->Open("GET", base_url + "/radio/list"); !opened) {
            ESP_LOGW("Radyo", "Istasyon listesi alinamadi: %s",
                     opened.error().ToString().c_str());
            return list;
        }
        std::string body;
        if (auto status = http->GetStatusCode(); status && *status == 200) {
            body = http->ReadAll();
        }
        http->Close();

        // [{"slug":"joyturk","ad":"Joy Turk"}, ...] - kucuk yanit, cJSON
        // kurmaya degmez (hava durumunda da ayni yaklasim).
        size_t pos = 0;
        while (list.size() < kMaxStations) {
            std::string slug = Field(body, "\"slug\":\"", pos);
            std::string name = Field(body, "\"ad\":\"", pos);
            if (slug.empty() || name.empty()) {
                break;
            }
            // "f":89.0 -> 890 (ondalikli sayiyi tam sayida tutuyoruz)
            list.push_back({slug, name, FreqTenths(body, pos)});
        }
        return list;
    }

private:
    static constexpr size_t kMaxStations = 12;
    static constexpr size_t kChunk = 2048;

    // Listedeki "f":89.0 alanini onda birlik tam sayiya cevirir.
    static int FreqTenths(const std::string& json, size_t& pos) {
        auto start = json.find("\"f\":", pos);
        if (start == std::string::npos) {
            return 0;
        }
        start += 4;
        double value = atof(json.c_str() + start);
        pos = start;
        return static_cast<int>(value * 10.0 + 0.5);
    }

    static std::string Field(const std::string& json, const char* key, size_t& pos) {
        auto start = json.find(key, pos);
        if (start == std::string::npos) {
            return "";
        }
        start += strlen(key);
        auto end = json.find('"', start);
        if (end == std::string::npos) {
            return "";
        }
        pos = end;
        return json.substr(start, end - start);
    }

    static void TaskEntry(void* arg) {
        static_cast<RadioPlayer*>(arg)->Run();
        vTaskDelete(nullptr);
    }

    void Run() {
        static const char* kTag = "Radyo";
        auto network = Board::GetInstance().GetNetwork();
        auto http = network != nullptr ? network->CreateHttp(0) : nullptr;

        if (http == nullptr) {
            ESP_LOGW(kTag, "Ag hazir degil");
            playing_ = false;
            return;
        }
        // Http artik std::expected donuyor; hata metni DNS mi, TLS mi, zaman
        // asimi mi oldugunu soyluyor - eskiden sadece "baglanilamadi" vardi.
        if (auto opened = http->Open("GET", url_); !opened) {
            ESP_LOGW(kTag, "Baglanilamadi (%s): %s", url_.c_str(),
                     opened.error().ToString().c_str());
            playing_ = false;
            return;
        }
        auto status = http->GetStatusCode();
        if (!status) {
            ESP_LOGW(kTag, "Durum okunamadi: %s", status.error().ToString().c_str());
            http->Close();
            playing_ = false;
            return;
        }
        if (*status != 200) {
            ESP_LOGW(kTag, "Sunucu %d dondu", *status);
            http->Close();
            playing_ = false;
            return;
        }
        ESP_LOGI(kTag, "Caliyor: %s", station_name_.c_str());

        auto& audio = Application::GetInstance().GetAudioService();
        // ⚠️ OggDemuxer icinde 8192 baytlik paket tamponu var. Yiginda
        // olusturmak gorevin butun yigi­nini tek basina yiyor ve cihaz
        // aninda cokuyor - bir surum tam olarak boyle patladi. PlaySound
        // da bu yuzden make_unique kullaniyor.
        auto demuxer = std::make_unique<OggDemuxer>();
        demuxer->OnPacket([&audio](const uint8_t* data, int sample_rate, int frame_duration_ms,
                                   size_t size) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            // Paket suresi artik Opus TOC baytindan okunuyor; eskiden 60 ms
            // varsayiyorduk ve sunucudaki ffmpeg'i ona zorluyorduk.
            packet->frame_duration = frame_duration_ms;
            packet->payload.resize(size);
            std::memcpy(packet->payload.data(), data, size);
            // wait=true: kuyruk dolunca burada bekliyoruz, HTTP okumasi da
            // dolayisiyla yavasliyor. Akis gercek zamana kendiliginden oturuyor.
            audio.PushPacketToDecodeQueue(std::move(packet), true);
        });
        demuxer->Reset();

        std::vector<char> buffer(kChunk);
        while (!stop_requested_.load()) {
            auto n = http->Read(buffer.data(), buffer.size());
            if (!n) {
                ESP_LOGW(kTag, "Akis kesildi: %s", n.error().ToString().c_str());
                break;
            }
            if (*n == 0) {
                // Radyo sonsuz akis; burasi yalnizca sesli bildirim gibi
                // sonlu dosyalarda normal cikis.
                break;
            }
            demuxer->Process(reinterpret_cast<const uint8_t*>(buffer.data()),
                            static_cast<size_t>(*n));
        }

        http->Close();
        ESP_LOGI(kTag, "Durdu: %s", station_name_.c_str());
        playing_ = false;
    }

    std::string url_;
    std::string station_name_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_requested_{false};
    TaskHandle_t task_ = nullptr;
};
