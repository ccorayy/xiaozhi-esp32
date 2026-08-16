#include "wifi_board.h"
#include "display/lcd_display.h"
#include "imu_qmi8658.h"
#include "rtc_pcf85063.h"
#include "sd_web_server.h"
#include "settings_panel_display.h"
#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "settings.h"
#include "assets/lang_config.h"

#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <dirent.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <sys/time.h>
#include <vector>

#define TAG "WaveshareEsp32s3TouchLCD1inch83"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

class WaveshareEsp32s3TouchLCD1inch83 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    Button pwr_button_;
    Display* display_;
    SettingsPanelDisplay* panel_display_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    sdmmc_card_t* sd_card_ = nullptr;
    std::string sd_status_ = "kapali";

    // microSD - SDMMC 1-bit. Stok firmware karta hic dokunmuyordu; pinler
    // Waveshare BSP bileseninden alindi (bkz. config.h). Kart yoksa veya
    // baglanamazsa acilis normal devam eder, sadece sd_status_ yazisi degisir.
    void InitializeSdCard() {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();

        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.clk = SD_CLK_PIN;
        slot_config.cmd = SD_CMD_PIN;
        slot_config.d0 = SD_D0_PIN;
        slot_config.d1 = GPIO_NUM_NC;
        slot_config.d2 = GPIO_NUM_NC;
        slot_config.d3 = GPIO_NUM_NC;
        slot_config.cd = SDMMC_SLOT_NO_CD;  // kartta algilama pini yok
        slot_config.wp = SDMMC_SLOT_NO_WP;
        slot_config.width = 1;

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
        mount_config.format_if_mount_failed = false;  // karti ASLA formatlama
        mount_config.max_files = 5;
        mount_config.allocation_unit_size = 16 * 1024;

        esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                                &mount_config, &sd_card_);
        if (err != ESP_OK) {
            sd_card_ = nullptr;
            sd_status_ = (err == ESP_ERR_TIMEOUT) ? "kart yok" : esp_err_to_name(err);
            ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(err));
            return;
        }

        sdmmc_card_print_info(stdout, sd_card_);

        uint64_t mb = (static_cast<uint64_t>(sd_card_->csd.capacity) * sd_card_->csd.sector_size)
                      / (1024 * 1024);
        char size_text[24];
        if (mb >= 1024) {
            snprintf(size_text, sizeof(size_text), "%.1f GB", mb / 1024.0);
        } else {
            snprintf(size_text, sizeof(size_text), "%llu MB", mb);
        }
        sd_status_ = std::string(size_text) + (TestSdReadWrite() ? " OK" : " R/O");
        ESP_LOGI(TAG, "SD card mounted at %s (%s)", SD_MOUNT_POINT, sd_status_.c_str());
    }

    // Kartin gercekten yazilip okunabildigini kanitlar; test dosyasini siler.
    bool TestSdReadWrite() {
        const char* path = SD_MOUNT_POINT "/xiaozhi-test.txt";
        FILE* f = fopen(path, "w");
        if (f == nullptr) {
            ESP_LOGW(TAG, "SD write test: fopen(w) failed");
            return false;
        }
        bool ok = fprintf(f, "xiaozhi sd test\n") > 0;
        fclose(f);
        if (!ok) {
            return false;
        }

        f = fopen(path, "r");
        if (f == nullptr) {
            ESP_LOGW(TAG, "SD read test: fopen(r) failed");
            return false;
        }
        char buffer[32] = {};
        ok = fgets(buffer, sizeof(buffer), f) != nullptr;
        fclose(f);
        remove(path);
        return ok;
    }

    // ------------------------------------------------------------------
    // SD dosya sunucusu - kart okuyucusu olmadan PC'den dosya atmak icin
    // ------------------------------------------------------------------
    // Varsayilan kapali; kimlik dogrulamasi yok, surekli acik durmasin.
    // Tercih NVS'te saklaniyor ki kullanici isterse acik biraksin.
    SdWebServer sd_server_{SD_MOUNT_POINT};
    esp_timer_handle_t sd_server_timer_ = nullptr;
    bool sd_server_wanted_ = false;

    // ⚠️ httpd_start lwIP soketi aciyor. Board kurucusu calisirken ag yigini
    // HENUZ ayakta degil; orada baslatmak
    //   assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)
    // ile paniklestiriyor ve cihaz acilis dongusune giriyor. Bir kez yasandi:
    // kullanici sunucuyu acik biraktigi icin tercih NVS'ten okunup kurucuda
    // baslatilmisti. Artik WiFi baglanana kadar bekliyoruz.
    static constexpr int kSdServerRetryMs = 10 * 1000;

    void InitializeSdServer() {
        if (sd_card_ == nullptr) {
            return;
        }
        Settings settings("sdweb", false);
        if (!settings.GetBool("on", false)) {
            return;
        }
        sd_server_wanted_ = true;

        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<WaveshareEsp32s3TouchLCD1inch83*>(arg)->TryStartSdServer();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "sdweb_start";
        if (esp_timer_create(&args, &sd_server_timer_) == ESP_OK) {
            esp_timer_start_periodic(sd_server_timer_, kSdServerRetryMs * 1000LL);
        }
    }

    void TryStartSdServer() {
        if (sd_server_.running() || !sd_server_wanted_) {
            esp_timer_stop(sd_server_timer_);  // isimiz bitti
            return;
        }
        if (!WifiManager::GetInstance().IsConnected()) {
            return;  // sonraki turda tekrar bak
        }
        // Soket ve gorev acmak esp_timer gorevinin kucuk yiginina gore agir.
        Application::GetInstance().Schedule([this]() { sd_server_.Start(); });
    }

    void SetSdServerEnabled(bool enabled) {
        if (sd_card_ == nullptr) {
            return;
        }
        sd_server_wanted_ = enabled;
        if (enabled) {
            sd_server_.Start();
        } else {
            sd_server_.Stop();
        }
        Settings settings("sdweb", true);
        settings.SetBool("on", sd_server_.running());
    }

    std::string SdFreeSpace() {
        if (sd_card_ == nullptr) {
            return "-";
        }
        uint64_t total = 0, free_bytes = 0;
        if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
            return "-";
        }
        char text[24];
        double gb = free_bytes / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1.0) {
            snprintf(text, sizeof(text), "%.1f GB", gb);
        } else {
            snprintf(text, sizeof(text), "%llu MB", free_bytes / (1024 * 1024));
        }
        return text;
    }

    // WiFi ayar portali da port 80'i kullaniyor; dosya sunucusu acikken
    // portal baslayamaz. Portala gecmeden once yerimizi bosaltiyoruz.
    void EnterWifiConfigModeReleasingPort() {
        sd_server_.Stop();
        EnterWifiConfigMode();
    }

    // Galeri sayfasi icin: kartin kok dizinindeki gorseller, ada gore sirali.
    std::vector<std::string> SdImageList() {
        std::vector<std::string> files;
        if (sd_card_ == nullptr) {
            return files;
        }
        DIR* dir = opendir(SD_MOUNT_POINT);
        if (dir == nullptr) {
            return files;
        }
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr && files.size() < kMaxGalleryFiles) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            std::string name = entry->d_name;
            auto dot = name.rfind('.');
            if (dot == std::string::npos) {
                continue;
            }
            std::string ext = name.substr(dot + 1);
            for (auto& c : ext) {
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            }
            // Derlenmis cozucular: LODEPNG ve TJPGD (bkz. config.json).
            if (ext == "png" || ext == "jpg" || ext == "jpeg") {
                files.push_back(name);
            }
        }
        closedir(dir);
        std::sort(files.begin(), files.end());
        return files;
    }

    static constexpr size_t kMaxGalleryFiles = 40;

    int SdFileCount() {
        if (sd_card_ == nullptr) {
            return 0;
        }
        DIR* dir = opendir(SD_MOUNT_POINT);
        if (dir == nullptr) {
            return 0;
        }
        int count = 0;
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] != '.') {
                count++;
            }
        }
        closedir(dir);
        return count;
    }

    // ------------------------------------------------------------------
    // Hareket algilama (QMI8658)
    //   eline alinca / masaya vurunca  -> ekrani uyandir
    //   ters cevirince (yuzustu)       -> sesi kapat, duzelince geri ac
    // ------------------------------------------------------------------
    Qmi8658* imu_ = nullptr;
    esp_timer_handle_t imu_timer_ = nullptr;
    float imu_last_x_ = 0, imu_last_y_ = 0, imu_last_z_ = 0;
    bool imu_have_sample_ = false;
    int imu_face_down_count_ = 0;
    bool imu_muted_ = false;
    int imu_volume_before_mute_ = 60;

    // Cihazda olculen gercek degerler:
    //   dik duruyor (ekran kullaniciya bakiyor) -> x=+1.0, z= 0.0
    //   ekran masaya kapali (yuzustu)           -> x= 0.0, z=-1.0
    //   sirtustu duz yatiyor                    -> z=+1.0
    // Yani "ekrani kapatmak" Z ekseninde goruluyor. Ilk surum bunu dogru
    // yakaliyordu ama sesi geri acma kosulu z > +0.30 idi; o da ancak SIRTUSTU
    // yatarken saglaniyor. Cihaz tekrar DIK konursa z sifira yakin kaliyor,
    // kosul saglanmiyor ve ses kapali kaliyordu. Artik "yuzustu degilse" acilir.
    static constexpr float kMotionThreshold = 0.35f;   // normal kullanimda 0.2-0.3 geliyordu
    static constexpr float kFaceDownZ = -0.60f;        // ekran asagi bakiyor
    static constexpr float kNotFaceDownZ = -0.30f;     // histerezis: bu esigin ustu "yuzustu degil"
    static constexpr int kFaceDownSamples = 16;        // ~0.8 sn (50 ms x 16)
    // Bakis: 1 g egim ~14 piksel bebek kaymasi. Goz 56 px, bebek 24 px,
    // yani en fazla 16 px oynayabiliyor - katsayi onu dolduracak kadar.
    static constexpr float kGazeGain = 26.0f;
    // Tek darbe (masaya vurma, cihazi birakma) sersemletmesin: esik yukseldi
    // ve kisa pencerede birden fazla darbe isteniyor. Gercek sallamada
    // saniyede 5-10 tepe geliyor, tek vurusta bir tane.
    static constexpr float kShakeThreshold = 1.60f;
    static constexpr int kShakeHitsNeeded = 3;
    static constexpr int kShakeWindow = 12;  // 12 x 50 ms = 600 ms
    int shake_hits_ = 0;
    int shake_window_ = 0;

    void InitializeImu() {
        // Once yoklama: I2cDevice::ReadReg icindeki ESP_ERROR_CHECK yanlis
        // adreste cihazi komple cokertirdi.
        uint8_t addr = 0;
        if (i2c_master_probe(i2c_bus_, Qmi8658::kAddrHigh, 100) == ESP_OK) {
            addr = Qmi8658::kAddrHigh;
        } else if (i2c_master_probe(i2c_bus_, Qmi8658::kAddrLow, 100) == ESP_OK) {
            addr = Qmi8658::kAddrLow;
        } else {
            ESP_LOGW(TAG, "IMU I2C'de bulunamadi, hareket algilama kapali");
            return;
        }

        imu_ = new Qmi8658(i2c_bus_, addr);
        if (!imu_->Initialize()) {
            delete imu_;
            imu_ = nullptr;
            return;
        }

        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<WaveshareEsp32s3TouchLCD1inch83*>(arg)->OnImuTick();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "imu_tick";
        if (esp_timer_create(&args, &imu_timer_) == ESP_OK) {
            esp_timer_start_periodic(imu_timer_, 50000);  // 50 ms - bakis takibi icin
        }
    }

    void OnImuTick() {
        float x = 0, y = 0, z = 0;
        if (imu_ == nullptr || !imu_->ReadAccel(x, y, z)) {
            return;
        }

        float delta = 0;
        if (imu_have_sample_) {
            delta = fabsf(x - imu_last_x_) + fabsf(y - imu_last_y_) + fabsf(z - imu_last_z_);
            if (delta > kMotionThreshold && power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
        }

        // --- Bakis takibi ve sarsinti (NIMO'daki davranisin karsiligi)
        // Eksen duzeni cihazda olculdu (bkz. CLAUDE.md): dik dururken x=+1
        // yani yercekimi X'te; Z ekran normali (yuzustu -1). Dolayisiyla
        // saga/sola egme Y'de, one/arkaya egme Z'de goruluyor.
        if (panel_display_ != nullptr) {
            panel_display_->SetGaze(y * kGazeGain, z * kGazeGain);
            if (delta > kShakeThreshold) {
                shake_window_ = kShakeWindow;
                if (++shake_hits_ >= kShakeHitsNeeded) {
                    shake_hits_ = 0;
                    shake_window_ = 0;
                    panel_display_->TriggerDizzy();
                }
            } else if (shake_window_ > 0 && --shake_window_ == 0) {
                shake_hits_ = 0;  // pencere kapandi, sayac sifirlansin
            }
        }

        imu_last_x_ = x;
        imu_last_y_ = y;
        imu_last_z_ = z;
        imu_have_sample_ = true;

        // Yuzustu birakinca sessize al. Anlik sarsintiyla tetiklenmesin diye
        // ust uste birkac ornek bekliyoruz.
        if (z < kFaceDownZ) {
            if (imu_face_down_count_ < kFaceDownSamples) {
                imu_face_down_count_++;
                if (imu_face_down_count_ == kFaceDownSamples && !imu_muted_) {
                    SetMutedByGesture(true);
                }
            }
        } else if (z > kNotFaceDownZ) {
            imu_face_down_count_ = 0;
            if (imu_muted_) {
                SetMutedByGesture(false);
            }
        }
    }

    void SetMutedByGesture(bool mute) {
        auto codec = GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        imu_muted_ = mute;
        if (mute) {
            imu_volume_before_mute_ = codec->output_volume();
            codec->SetOutputVolume(0);
            GetDisplay()->ShowNotification("Sessize alindi");
            ESP_LOGI(TAG, "Yuzustu: ses kapatildi (onceki %d)", imu_volume_before_mute_);
        } else {
            codec->SetOutputVolume(imu_volume_before_mute_);
            GetDisplay()->ShowNotification("Ses geri acildi");
            ESP_LOGI(TAG, "Duzeldi: ses %d", imu_volume_before_mute_);
        }
    }

    // ------------------------------------------------------------------
    // RTC (PCF85063) - pil yedekli saat
    // ------------------------------------------------------------------
    // Sistem saati normalde sunucudan geliyor (ota.cc, "Server-Time"). Aga
    // baglanana kadar cihaz 1970'te oluyor ve saat ekrani/alarm calismiyor.
    // Kartta pil yedekli RTC var; acilista ondan okuyup sistemi kuruyoruz,
    // sunucu saati geldikten sonra da RTC'yi guncelliyoruz.
    // Dakikada bir: yedi register yazmak bedava sayilir, karsiliginda fisi
    // cekilen cihaz saati en fazla bir dakika geride uyanir.
    static constexpr int kRtcSyncIntervalMs = 60 * 1000;

    Pcf85063* rtc_ = nullptr;
    esp_timer_handle_t rtc_timer_ = nullptr;
    bool rtc_written_ = false;

    static bool SystemTimeValid() {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        return t != nullptr && t->tm_year + 1900 >= 2024;
    }

    void InitializeRtc() {
        // IMU'daki ile ayni sebep: I2cDevice::ReadReg icindeki ESP_ERROR_CHECK
        // yanlis adreste cihazi cokertir, once yokluyoruz.
        if (i2c_master_probe(i2c_bus_, Pcf85063::kAddr, 100) != ESP_OK) {
            ESP_LOGW(TAG, "RTC I2C'de bulunamadi");
            return;
        }
        rtc_ = new Pcf85063(i2c_bus_, Pcf85063::kAddr);
        if (!rtc_->Initialize()) {
            delete rtc_;
            rtc_ = nullptr;
            ESP_LOGW(TAG, "RTC baslatilamadi");
            return;
        }

        struct tm t = {};
        if (SystemTimeValid()) {
            ESP_LOGI(TAG, "RTC bulundu, sistem saati zaten gecerli");
        } else if (!rtc_->ReadTime(t)) {
            // Ilk acilista beklenen durum: osilator hic calismamis, icindeki
            // zaman anlamsiz. Ilk gecerli sistem saatinde doldurulacak.
            ESP_LOGI(TAG, "RTC bulundu ama saati gecersiz");
        } else {
            // ota.cc sistem saatini zaten yerel saate ayarliyor (timezone_offset
            // ekleyerek), RTC'ye de oyle yazdik; donusum gerekmiyor.
            struct timeval tv = {};
            tv.tv_sec = mktime(&t);
            settimeofday(&tv, nullptr);
            ESP_LOGI(TAG, "Saat RTC'den kuruldu: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        }

        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<WaveshareEsp32s3TouchLCD1inch83*>(arg)->SyncRtcFromSystem();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "rtc_sync";
        if (esp_timer_create(&args, &rtc_timer_) == ESP_OK) {
            esp_timer_start_periodic(rtc_timer_, kRtcSyncIntervalMs * 1000LL);
        }
    }

    void SyncRtcFromSystem() {
        if (rtc_ == nullptr || !SystemTimeValid()) {
            return;
        }
        time_t now = time(nullptr);
        struct tm t = {};
        localtime_r(&now, &t);
        rtc_->WriteTime(t);
        if (!rtc_written_) {
            rtc_written_ = true;
            ESP_LOGI(TAG, "RTC kuruldu: %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900,
                     t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        }
    }

    // ------------------------------------------------------------------
    // Alarm - panelde kurulur, ses burada calinir
    // ------------------------------------------------------------------
    void OnAlarmRing() {
        if (power_save_timer_ != nullptr) {
            power_save_timer_->WakeUp();
        }
        // PlaySound ses hattini kullaniyor; LVGL gorevinden degil ana gorevden
        // cagirmak daha guvenli.
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
        });
    }

    // ------------------------------------------------------------------
    // Hava durumu - arama servisinin /weather ucundan
    // ------------------------------------------------------------------
    // Servis Raspberry Pi'den Hetzner'e tasindi (bkz. CLAUDE.md §10). Artik
    // tek adres yetiyor: sunucu acik internette, cihaz evde de disarida da
    // ayni yere gidiyor. Eskiden ev agindaki IP'ye dusen yedek yol vardi,
    // sunucu evde olmadigi icin anlamini yitirdi.
    // Servis yaniti onbellekli oldugu icin bu istekler Gemini aramasi harcamaz.
    static constexpr const char* kWeatherUrl = "https://hava.shoptimize.com.tr/weather";
    static constexpr int kWeatherFirstDelayMs = 30 * 1000;      // ag otursun
    static constexpr int kWeatherIntervalMs = 20 * 60 * 1000;   // 20 dakika

    esp_timer_handle_t weather_timer_ = nullptr;

    // GECICI TESHIS: galeri siyah ekran veriyor. Acilistan 20 sn sonra
    // karttaki ilk gorseli cozmeyi deneyip sonucu seri porta yaziyoruz.
    // Kullanicinin ekrana dokunmasini beklemeden nerede koptugunu gorelim.
    esp_timer_handle_t gallery_test_timer_ = nullptr;

    void InitializeGallerySelfTest() {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<WaveshareEsp32s3TouchLCD1inch83*>(arg);
            // Cozme islemi esp_timer gorevinin kucuk yiginina gore agir.
            Application::GetInstance().Schedule([self]() {
                if (self->panel_display_ != nullptr) {
                    self->panel_display_->LogImageDecodeSelfTest();
                }
            });
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "galeri_test";
        if (esp_timer_create(&args, &gallery_test_timer_) == ESP_OK) {
            esp_timer_start_once(gallery_test_timer_, 20 * 1000 * 1000LL);
        }
    }

    void InitializeWeather() {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<WaveshareEsp32s3TouchLCD1inch83*>(arg);
            // Ag islemi zamanlayici gorevinde yapilmaz; kisa omurlu bir gorev acip
            // orada cekiyoruz.
            xTaskCreate([](void* p) {
                static_cast<WaveshareEsp32s3TouchLCD1inch83*>(p)->FetchWeather();
                vTaskDelete(nullptr);
            }, "weather", 4096, self, 2, nullptr);
            esp_timer_start_once(self->weather_timer_, kWeatherIntervalMs * 1000LL);
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "weather";
        if (esp_timer_create(&args, &weather_timer_) == ESP_OK) {
            esp_timer_start_once(weather_timer_, kWeatherFirstDelayMs * 1000LL);
        }
    }

    // GECICI TESHIS: galeri raporunu sunucuya gonderir. Statik cunku kisa
    // omurlu bir gorevden cagriliyor. Galeri sorunu cozulunce kaldirilacak.
    static void PostDiagnostic(const std::string& body) {
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            return;
        }
        auto http = network->CreateHttp(0);
        if (http == nullptr) {
            return;
        }
        http->SetContent(std::string(body));
        if (http->Open("POST", "https://hava.shoptimize.com.tr/log")) {
            ESP_LOGI(TAG, "Teshis raporu gonderildi (%d)", http->GetStatusCode());
            http->Close();
        } else {
            ESP_LOGW(TAG, "Teshis raporu gonderilemedi");
        }
    }

    bool TryFetchWeather(const char* url, std::string& body) {
        auto network = GetNetwork();
        if (network == nullptr) {
            return false;
        }
        auto http = network->CreateHttp(0);
        if (http == nullptr || !http->Open("GET", url)) {
            return false;
        }
        bool ok = http->GetStatusCode() == 200;
        if (ok) {
            body = http->ReadAll();
        }
        http->Close();
        return ok;
    }

    void FetchWeather() {
        std::string body;
        if (!TryFetchWeather(kWeatherUrl, body)) {
            ESP_LOGW(TAG, "Hava durumu alinamadi");
            return;
        }

        // {"city":"Istanbul","temp":"23","desc":"Acik"}
        std::string temp = JsonField(body, "temp");
        std::string desc = JsonField(body, "desc");
        if (temp.empty()) {
            return;
        }
        std::string text = temp + "\xC2\xB0  " + desc;  // derece isareti (UTF-8)
        ESP_LOGI(TAG, "Hava durumu: %s", text.c_str());
        if (panel_display_ != nullptr) {
            panel_display_->SetWeatherText(text);
        }
    }

    // Kucuk yanit icin cJSON kurmaya degmez; alani elle cikariyoruz.
    static std::string JsonField(const std::string& json, const char* key) {
        std::string pattern = std::string("\"") + key + "\":\"";
        auto start = json.find(pattern);
        if (start == std::string::npos) {
            return "";
        }
        start += pattern.size();
        auto end = json.find('"', start);
        return end == std::string::npos ? "" : json.substr(start, end - start);
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigModeReleasingPort();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        // ------------------------------------------------------------------
        // PWR button (GPIO41) - sesle ugrasmadan ses ve parlaklik kontrolu.
        // Stok firmware'de bu buton hic kullanilmiyordu.
        //   tek tik   -> ses      : 20 > 40 > 60 > 80 > 100 > sessiz > 20 ...
        //   cift tik  -> parlaklik: 25 > 50 > 75 > 100 > 25 ...
        // Uzun basma bilerek bos birakildi: 4 sn basili tutmak AXP2101
        // tarafindan donanimsal kapatmaya ayrilmis durumda.
        // ------------------------------------------------------------------
        pwr_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "PWR button pressed");
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
        });

        pwr_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            int volume = ((codec->output_volume() / 20) + 1) * 20;
            if (volume > 100) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            if (volume == 0) {
                GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            } else if (volume >= 100) {
                GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
            } else {
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            }
            ESP_LOGI(TAG, "Volume set to %d", volume);
        });

        pwr_button_.OnDoubleClick([this]() {
            auto backlight = GetBacklight();
            int brightness = ((backlight->brightness() / 25) + 1) * 25;
            if (brightness > 100) {
                brightness = 25;
            }
            backlight->SetBrightness(brightness, true);  // true -> NVS'e kalici yaz
            GetDisplay()->ShowNotification("Parlaklik " + std::to_string(brightness));
            ESP_LOGI(TAG, "Brightness set to %d", brightness);
        });
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 24 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        // esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        auto settings_display = new SettingsPanelDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        settings_display->SetOnWifiConfigRequest([this]() { EnterWifiConfigModeReleasingPort(); });
        settings_display->SetOnUserActivity([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
        });
        settings_display->SetSdInfoProvider([this]() { return sd_status_; });
        settings_display->SetOnAlarmRing([this]() { OnAlarmRing(); });
        settings_display->SetOnDizzy([]() {
            // LVGL gorevinden geliyor; ses hattini ana gorevde acalim.
            Application::GetInstance().Schedule(
                []() { Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION); });
        });

        SettingsPanelDisplay::SdHooks sd_hooks;
        sd_hooks.status = [this]() { return sd_status_; };
        sd_hooks.free_space = [this]() { return SdFreeSpace(); };
        sd_hooks.file_count = [this]() { return SdFileCount(); };
        sd_hooks.server_running = [this]() { return sd_server_.running(); };
        sd_hooks.set_server = [this](bool on) { SetSdServerEnabled(on); };
        sd_hooks.server_url = [this]() { return WifiManager::GetInstance().GetIpAddress(); };
        sd_hooks.list_images = [this]() { return SdImageList(); };
        settings_display->SetDiagnosticReporter([this](const std::string& text) {
            // Ag islemi LVGL/ana gorevde bloklamasin diye kisa omurlu gorev.
            auto* copy = new std::string(text);
            xTaskCreate([](void* p) {
                std::unique_ptr<std::string> body(static_cast<std::string*>(p));
                PostDiagnostic(*body);
                vTaskDelete(nullptr);
            }, "teshis", 4096, copy, 2, nullptr);
        });
        settings_display->SetSdHooks(std::move(sd_hooks));

        panel_display_ = settings_display;
        display_ = settings_display;
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = GPIO_NUM_39,
            .int_gpio_num = GPIO_NUM_13,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        tp_io_config.scl_speed_hz = 400 * 1000;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 8;
        tp_io_config.lcd_param_bits = 0;
        tp_io_config.flags.disable_control_phase = 1;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        // Sesle arayuz kontrolu: "menuyu ac", "saati goster", "ayarlara gec".
        // Cihaz kendi ekranini kontrol edebildigini bilsin diye acikca tanitiyoruz.
        mcp_server.AddTool("self.ui.open_app",
            "Open a screen on the device display. "
            "Valid values for `app`: menu, chat, clock, settings, wifi, info, alarm. "
            "Use this when the user asks to show or open something on the screen.",
            PropertyList({
                Property("app", kPropertyTypeString)
            }), [this](const PropertyList& properties) -> ReturnValue {
                auto app = properties["app"].value<std::string>();
                if (panel_display_ == nullptr) {
                    return false;
                }
                return panel_display_->OpenApp(app);
            });

        mcp_server.AddTool("self.ui.get_current_app",
            "Returns which screen the device is currently showing.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                return panel_display_ != nullptr ? panel_display_->CurrentApp() : std::string("unknown");
            });

        // Sesle alarm: "beni yarin 7'de kaldir".
        mcp_server.AddTool("self.alarm.set",
            "Set the device alarm clock. It rings every day at the given time until turned off. "
            "`hour` is 0-23, `minute` is 0-59, `enabled` turns the alarm on or off.",
            PropertyList({
                Property("hour", kPropertyTypeInteger, 0, 23),
                Property("minute", kPropertyTypeInteger, 0, 0, 59),
                Property("enabled", kPropertyTypeBoolean, true)
            }), [this](const PropertyList& properties) -> ReturnValue {
                if (panel_display_ == nullptr) {
                    return false;
                }
                return panel_display_->SetAlarm(properties["hour"].value<int>(),
                                                properties["minute"].value<int>(),
                                                properties["enabled"].value<bool>());
            });

        mcp_server.AddTool("self.alarm.get",
            "Returns the current alarm time and whether it is on.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                return panel_display_ != nullptr ? panel_display_->GetAlarmText()
                                                 : std::string("yok");
            });

        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigModeReleasingPort();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchLCD1inch83() : boot_button_(BOOT_BUTTON_GPIO), pwr_button_(PWR_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeAxp2101();
        InitializeRtc();
        InitializeImu();
        InitializeSdCard();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeWeather();
        InitializeGallerySelfTest();
        InitializeSdServer();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchLCD1inch83);
