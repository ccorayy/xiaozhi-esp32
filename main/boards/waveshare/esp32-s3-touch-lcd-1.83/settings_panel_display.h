#pragma once

// ---------------------------------------------------------------------------
// Bu board'a ozel dokunmatik arayuz.
//
// Upstream'in kendi kalibi: SpiLcdDisplay'i alt siniflayip SetupUI()'yi
// override etmek (bkz. diger waveshare board'lari). Tum kod bu header'da
// duruyor ki upstream rebase'lerinde board .cc dosyasindaki diff kucuk kalsin.
//
// Panel lv_layer_top() uzerine kuruluyor; upstream'in SetupUI()/SetTheme()
// fonksiyonlari sadece lv_screen_active() cocuklariyla ugrastigi icin
// birbirimizin ayagina basmiyoruz.
//
// Telefon benzeri yapi: acilista MENU var, uygulamaya girilir, geri donulur.
//
//   MENU                -> 6 uygulama ikonu (Sohbet, Saat, Ayarlar, WiFi,
//                          Bilgi, Kisayollar)
//   Sohbet              -> gozler; ekrana dokunmak konusmayi baslatir/bitirir
//   Saat                -> halkali kadran (saniye + pil)
//   uygulama icinde     -> ust soldaki geri oku menuye dondurur
//   yukari kaydirma     -> her yerden Sohbet ekranina
//   asagi kaydirma      -> Sohbet/Saat ekranindan menuye
//
// Asistan konusmaya baslarsa cihaz kendiliginden Sohbet ekranina gecer.
//
// WiFi TARAMASI - neden bu kadar dolambacli:
// esp-wifi-connect bileseni her WIFI_EVENT_SCAN_DONE olayinda
// esp_wifi_scan_get_ap_records() cagiriyor; bu fonksiyon sonuclari kopyaladiktan
// sonra listeyi SERBEST BIRAKIYOR. Ilk denemede kendi taramamizi baslatmistik ve
// sonuclar biz okumadan siliniyordu (cihazda: hep "Ag bulunamadi").
// Cozum: olay isleyicileri kayit SIRASINA gore calisiyor ve bizim SetupUI()
// (application.cc:64) bilesenin baslatildigi StartNetwork()'ten (satir 162) once
// caliyor. Olay dongusunu kendimiz kurup isleyicimizi ONCE kaydediyoruz.
// Bilesenin otomatik baglanmasini bozmamak icin sonuclari yalnizca kullanici
// "Aglari Tara" dedigi turda tuketiyoruz; diger turlarda dokunmuyoruz.
// ---------------------------------------------------------------------------

#include "application.h"
#include "eyes_face.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "lcd_display.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <material_symbols.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

class SettingsPanelDisplay : public SpiLcdDisplay {
public:
    // Hangi ekrandayiz. kChat ve kClock kabugu gizler (altta gozler/saat kalir),
    // digerleri kabuk uzerinde tam ekran acilir.
    // kPhoto menude yok; galeriden bir dosyaya dokununca aciliyor.
    enum class View { kChat, kClock, kLauncher, kSettings, kWifi, kInfo, kActions, kAlarm, kSd,
                      kGallery, kPhoto };

    // SD kart islerini board yapar; bu sinif ne SDMMC'yi ne de httpd'yi tanir.
    struct SdHooks {
        std::function<std::string()> status;          // "59.5 GB OK"
        std::function<std::string()> free_space;      // "47.1 GB bos"
        std::function<int()> file_count;
        std::function<bool()> server_running;
        std::function<void(bool)> set_server;
        std::function<std::string()> server_url;      // "192.168.1.42"
        std::function<std::vector<std::string>()> list_images;
    };

    SettingsPanelDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y, bool mirror_x,
                         bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    // WiFi ayar moduna gecis board tarafindan saglanir; bu sinif WifiBoard'i tanimaz.
    void SetOnWifiConfigRequest(std::function<void()> callback) {
        on_wifi_config_ = std::move(callback);
    }

    // Panelle etkilesim uyku sayacini sifirlasin diye.
    void SetOnUserActivity(std::function<void()> callback) { on_activity_ = std::move(callback); }

    // Hava durumu board tarafindan ceklip veriliyor.
    void SetWeatherText(const std::string& text) {
        DisplayLockGuard lock(this);
        eyes_.SetWeather(text);
    }

    // SD kart durumunu board saglar; bu sinif SDMMC'yi tanimaz.
    void SetSdInfoProvider(std::function<std::string()> provider) {
        sd_info_provider_ = std::move(provider);
    }

    void SetSdHooks(SdHooks hooks) { sd_ = std::move(hooks); }

    // GECICI TESHIS: galeri siyah ekran veriyor. Baslik okunabiliyor (en/boy
    // dogru yaziliyor) ama cizim bos. Hangi asamada koptugunu cihazin kendisi
    // soylesin diye acilista karttaki ilk gorseli cozmeyi deniyoruz.
    //
    // Sonuc hem seri porta hem EKRANA yaziliyor: kablo takili olmayabilir,
    // o zaman kullanici okuyup soyluyor. LVGL'in kendi uyarisi da
    // lv_log_register_print_cb ile yakalanip ekrana ekleniyor - asil sebep
    // genelde orada.
    // Sorun bulununca bu metot, kancasi ve LV_USE_LOG kaldirilacak.
    void LogImageDecodeSelfTest() {
        static const char* kTag = "GaleriTest";
        char summary[512];
        int used = 0;

        if (!sd_.list_images) {
            ESP_LOGW(kTag, "SD kancasi yok");
            return;
        }
        auto files = sd_.list_images();
        if (files.empty()) {
            ESP_LOGW(kTag, "Kartta gorsel yok");
            return;
        }

        DisplayLockGuard lock(this);

        // DIKKAT: lv_result_t'de LV_RESULT_INVALID=0, LV_RESULT_OK=1.
        // Ilk surumde "0=OK" yazmisim, tam tersi. lv_fs_res_t ise 0=OK.
        int drawable = -1;
        size_t count = files.size() < 4 ? files.size() : 4;
        for (size_t i = 0; i < count; i++) {
            std::string path = "S:/" + files[i];
            lvgl_message_[0] = '\0';

            uint8_t head[4] = {};
            uint32_t read = 0;
            lv_fs_file_t file;
            if (lv_fs_open(&file, path.c_str(), LV_FS_MODE_RD) == LV_FS_RES_OK) {
                lv_fs_read(&file, head, sizeof(head), &read);
                lv_fs_close(&file);
            }

            lv_image_header_t header = {};
            bool info_ok = lv_image_decoder_get_info(path.c_str(), &header) == LV_RESULT_OK;
            if (info_ok && drawable < 0) {
                drawable = static_cast<int>(i);
            }

            ESP_LOGI(kTag, "%s: %02X%02X info=%s %dx%d | LVGL: %s", files[i].c_str(), head[0],
                     head[1], info_ok ? "OK" : "HATA", static_cast<int>(header.w),
                     static_cast<int>(header.h),
                     lvgl_message_[0] != '\0' ? lvgl_message_ : "-");
            used += snprintf(summary + used, sizeof(summary) - used, "%.11s %02X%02X %s %dx%d\n%s\n",
                             files[i].c_str(), head[0], head[1], info_ok ? "OK" : "HATA",
                             static_cast<int>(header.w), static_cast<int>(header.h),
                             lvgl_message_[0] != '\0' ? lvgl_message_ : "-");
            if (used >= static_cast<int>(sizeof(summary)) - 80) {
                break;
            }
        }

        // Basligi okunabilen ilk dosyayi gercekten cizdir: cozucu tam cozmede
        // patlarsa uyarisini burada birakir.
        if (drawable >= 0) {
            lvgl_message_[0] = '\0';
            ShowPhoto(drawable);
            lv_refr_now(nullptr);
            ESP_LOGI(kTag, "cizim sonrasi LVGL: %s",
                     lvgl_message_[0] != '\0' ? lvgl_message_ : "(uyari yok)");
            snprintf(summary + used, sizeof(summary) - used, "CIZIM: %s",
                     lvgl_message_[0] != '\0' ? lvgl_message_ : "(uyari yok)");
        } else {
            snprintf(summary + used, sizeof(summary) - used, "CIZIM: denenmedi");
            ShowView(View::kPhoto);
        }
        lv_label_set_text(photo_note_, summary);
    }

    // Alarm caldiginda board ses calsin ve ekrani uyandirsin diye.
    void SetOnAlarmRing(std::function<void()> callback) { on_alarm_ring_ = std::move(callback); }

    // Sesle alarm kurma (board'daki MCP araci cagirir, baska gorevden gelir).
    bool SetAlarm(int hour, int minute, bool enabled) {
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            return false;
        }
        DisplayLockGuard lock(this);
        alarm_hour_ = hour;
        alarm_minute_ = minute;
        alarm_enabled_ = enabled;
        alarm_ring_ticks_ = 0;
        SuppressImmediateRing();
        SaveAlarm();
        RefreshAlarmView();
        return true;
    }

    std::string GetAlarmText() {
        char buffer[40];
        snprintf(buffer, sizeof(buffer), "%02d:%02d %s", alarm_hour_, alarm_minute_,
                 alarm_enabled_ ? "acik" : "kapali");
        return buffer;
    }

    // Sesle arayuz kontrolu icin disari acilan kapi (board'daki MCP araci cagirir).
    // Baska bir gorevden gelir, o yuzden LVGL kilidini burada aliyoruz.
    bool OpenApp(const std::string& name) {
        View target;
        if (name == "menu" || name == "menü") {
            target = View::kLauncher;
        } else if (name == "chat" || name == "sohbet") {
            target = View::kChat;
        } else if (name == "clock" || name == "saat") {
            target = View::kClock;
        } else if (name == "settings" || name == "ayarlar") {
            target = View::kSettings;
        } else if (name == "wifi") {
            target = View::kWifi;
        } else if (name == "info" || name == "bilgi") {
            target = View::kInfo;
        } else if (name == "alarm") {
            target = View::kAlarm;
        } else if (name == "sd" || name == "files" || name == "dosyalar") {
            target = View::kSd;
        } else if (name == "gallery" || name == "galeri" || name == "photos") {
            target = View::kGallery;
        } else {
            return false;
        }
        DisplayLockGuard lock(this);
        ShowView(target);
        return true;
    }

    std::string CurrentApp() const {
        switch (current_view_) {
            case View::kChat: return "chat";
            case View::kClock: return "clock";
            case View::kLauncher: return "menu";
            case View::kSettings: return "settings";
            case View::kWifi: return "wifi";
            case View::kInfo: return "info";
            case View::kActions: return "shortcuts";
            case View::kAlarm: return "alarm";
            case View::kSd: return "sd";
            case View::kGallery: return "gallery";
            case View::kPhoto: return "photo";
        }
        return "unknown";
    }

    virtual void SetupUI() override {
        SpiLcdDisplay::SetupUI();

        // GECICI: LVGL'in uyarilarini yakala ki teshis ekrana da yazilabilsin
        // (kablo takili olmayabilir). Teshis bitince kaldirilacak.
        lv_log_register_print_cb(LvglLogCb);

        DisplayLockGuard lock(this);
        ApplySafeAreaInsets();
        CreateFace();
        CreatePanel();
    }

    virtual void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);

        DisplayLockGuard lock(this);
        ApplySafeAreaInsets();
        eyes_.ApplyTheme(static_cast<LvglTheme*>(current_theme_));
        StylePanel();
    }

    // Emoji yerine gozler: ustteki SetEmotion emoji resmi ariyor, biz onu hic
    // cagirmiyoruz. Diger iki metot uste devrediliyor, sadece bosta sayacini
    // sifirlamak icin araya giriyoruz.
    virtual void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        eyes_.SetExpression(emotion);
        eyes_.NotifyActivity();
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        SpiLcdDisplay::SetChatMessage(role, content);
        // ⚠️ eyes_.NotifyActivity() saat kadranini gizleyebiliyor, yani LVGL
        // nesnelerine dokunuyor. Bu metot LVGL gorevinden degil, uyku
        // zamanlayicisinin esp_timer gorevinden de cagriliyor
        // (PowerSaveTimer -> SetPowerSaveMode -> SetChatMessage). Kilitsiz
        // birakilinca LVGL'in gecersiz alan listesi bozuluyor ve cihaz
        // lv_inv_area icinde sonsuz donguye giriyordu.
        DisplayLockGuard lock(this);
        eyes_.NotifyActivity();
        // Menudeyken cevap gelirse kullanici kacirmasin diye sohbete geciyoruz.
        if (content != nullptr && content[0] != '\0' && current_view_ != View::kChat) {
            ShowView(View::kChat);
        }
    }

    virtual void SetStatus(const char* status) override {
        SpiLcdDisplay::SetStatus(status);
        // DIKKAT: LvglDisplay::UpdateStatusBar her 10 saniyede bir ust cubuktaki
        // saati yazmak icin SetStatus("HH:MM") cagiriyor. Bunu kullanici
        // etkilesimi sayarsak bosta sayaci 15 saniyeye hic ulasamiyor ve saat
        // ekrani hic acilmiyor (cihazda olculdu: sayac 0-5-10-0-5-10...).
        // Saat bicimindeki cagrilari yok sayiyoruz.
        bool is_clock_tick = status != nullptr && strlen(status) == 5 && status[2] == ':';
        if (!is_clock_tick) {
            DisplayLockGuard lock(this);  // LVGL'e dokunuyor, bkz. SetChatMessage
            eyes_.NotifyActivity();
        }
    }

private:
    // Ekranin fiziksel yuvarlak koseleri ust bardaki wifi/pil ikonlarini kirpiyordu.
    // Upstream degeri spacing(4)=8 px yaniydi; kose yaricapini asacak kadar bosluk.
    static constexpr int kSafeInsetX = 20;
    static constexpr int kSafeInsetTop = 10;

    static constexpr int kOpenStripHeight = 28;
    static constexpr int kConfirmTimeoutMs = 5000;
    static constexpr int kInfoRefreshMs = 2000;
    static constexpr int kFaceTickMs = 1000;  // saat ve goz kirpma icin
    static constexpr int kMinBrightness = 5;  // 0 = ekran tamamen kapanir, kilitlenmeyelim

    // Iki asamali onay isteyen butonlar (WiFi ayari, yeniden baslatma).
    struct ConfirmButton {
        SettingsPanelDisplay* owner = nullptr;
        lv_obj_t* button = nullptr;
        lv_obj_t* label = nullptr;
        const char* idle_text = nullptr;
        const char* confirm_text = nullptr;
        std::function<void()> action;
        bool pending = false;
        lv_timer_t* timer = nullptr;
    };

    View current_view_ = View::kLauncher;
    EyesFace eyes_;
    lv_timer_t* face_timer_ = nullptr;
    lv_obj_t* launcher_ = nullptr;
    lv_obj_t* view_settings_ = nullptr;
    lv_obj_t* view_wifi_ = nullptr;
    lv_obj_t* view_info_ = nullptr;
    lv_obj_t* view_actions_ = nullptr;
    lv_obj_t* view_alarm_ = nullptr;
    lv_obj_t* view_sd_ = nullptr;
    lv_obj_t* view_gallery_ = nullptr;
    lv_obj_t* view_photo_ = nullptr;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* open_strip_ = nullptr;

    // Sayfa 1 - Ayarlar
    lv_obj_t* volume_value_label_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* brightness_value_label_ = nullptr;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* theme_switch_ = nullptr;
    lv_obj_t* close_button_ = nullptr;
    lv_obj_t* close_button_label_ = nullptr;

    // Sayfa 2 - Bilgi
    lv_obj_t* info_battery_ = nullptr;
    lv_obj_t* info_wifi_ = nullptr;
    lv_obj_t* info_ip_ = nullptr;
    lv_obj_t* info_sd_ = nullptr;
    lv_obj_t* info_version_ = nullptr;
    lv_obj_t* info_uptime_ = nullptr;
    lv_timer_t* info_timer_ = nullptr;

    // Sayfa 3 - Kisayollar
    lv_obj_t* chat_button_ = nullptr;
    lv_obj_t* chat_button_label_ = nullptr;
    ConfirmButton wifi_confirm_;
    ConfirmButton restart_confirm_;

    // Sayfa 5 - Alarm. Tek alarm yetiyor, her gun ayni saatte calar.
    lv_obj_t* alarm_time_label_ = nullptr;
    lv_obj_t* alarm_switch_ = nullptr;
    int alarm_hour_ = 7;
    int alarm_minute_ = 0;
    bool alarm_enabled_ = false;
    int alarm_last_fired_ = -1;   // gunun dakikasi; ayni dakikada iki kez calmasin
    int alarm_ring_ticks_ = 0;    // caliyorsa kalan saniye
    std::function<void()> on_alarm_ring_;

    // Sadece +/- adim butonlari; temada yeniden renklendirmek icin listeliyoruz.
    struct LabeledButton {
        lv_obj_t* button;
        lv_obj_t* label;
    };
    std::vector<LabeledButton> step_buttons_;

    // Sayfa 6 - SD kart
    SdHooks sd_;
    lv_obj_t* sd_size_ = nullptr;
    lv_obj_t* sd_free_ = nullptr;
    lv_obj_t* sd_files_ = nullptr;
    lv_obj_t* sd_server_switch_ = nullptr;
    lv_obj_t* sd_url_ = nullptr;

    // Sayfa 7 - Galeri
    lv_obj_t* gallery_list_ = nullptr;
    lv_obj_t* gallery_hint_ = nullptr;
    lv_obj_t* photo_image_ = nullptr;
    lv_obj_t* photo_note_ = nullptr;
    std::vector<std::string> gallery_files_;
    std::string photo_path_;  // "S:/foo.jpg" - lv_image kaynagi isaretciyi tutuyor

    // GECICI TESHIS: LVGL'in son uyarisi. Statik cunku geri cagirma islevi
    // nesneyi tanimiyor; zaten tek ekran var.
    static inline char lvgl_message_[160] = {};

    static void LvglLogCb(lv_log_level_t level, const char* text) {
        if (level < LV_LOG_LEVEL_WARN || text == nullptr) {
            return;
        }
        snprintf(lvgl_message_, sizeof(lvgl_message_), "%s", text);
    }

    // Tema degisiminde yeniden renklendirilecek duz yazi etiketleri.
    std::vector<lv_obj_t*> plain_labels_;

    // Ikon yazi tipiyle cizilecek etiketler (menu simgeleri, geri oku).
    std::vector<lv_obj_t*> icon_labels_;

    // Kaydirma hareketi de LV_EVENT_CLICKED uretebiliyor; paneli acan kaydirmanin
    // ayni zamanda sohbeti baslatmasini engellemek icin.
    bool gesture_handled_ = false;

    std::function<void()> on_wifi_config_;
    std::function<void()> on_activity_;
    std::function<std::string()> sd_info_provider_;

    // Sayfa 4 - WiFi
    lv_obj_t* wifi_status_label_ = nullptr;
    lv_obj_t* wifi_add_button_ = nullptr;
    lv_obj_t* wifi_add_button_label_ = nullptr;
    lv_obj_t* wifi_list_ = nullptr;
    lv_obj_t* wifi_scan_button_ = nullptr;
    lv_obj_t* wifi_scan_button_label_ = nullptr;
    bool scan_pending_ = false;   // yalnizca true iken sonuclari tuketiyoruz

    struct ScanResult {
        std::string ssid;
        int rssi = 0;
        bool encrypted = true;
    };
    std::vector<ScanResult> scan_results_;
    static constexpr int kMaxScanResults = 15;

    // Sifre klavyesi (LV_USE_KEYBOARD=n oldugu icin lv_buttonmatrix ile elde yapildi)
    lv_obj_t* kb_overlay_ = nullptr;
    lv_obj_t* kb_title_ = nullptr;
    lv_obj_t* kb_field_ = nullptr;
    lv_obj_t* kb_matrix_ = nullptr;
    std::string kb_ssid_;
    std::string kb_password_;
    bool kb_upper_ = false;
    bool kb_symbols_ = false;
    bool kb_entering_ssid_ = false;  // once ag adi, sonra sifre

    // ------------------------------------------------------------------
    // Ust bar guvenli alan
    // ------------------------------------------------------------------
    void ApplySafeAreaInsets() {
        if (top_bar_ != nullptr) {
            lv_obj_set_style_pad_left(top_bar_, kSafeInsetX, 0);
            lv_obj_set_style_pad_right(top_bar_, kSafeInsetX, 0);
            lv_obj_set_style_pad_top(top_bar_, kSafeInsetTop, 0);
        }
        // status_bar_ ust bar ile ust uste biniyor; ayni kadar asagi itilmezse
        // ortadaki durum yazisi ikonlarla hizasini kaybeder.
        if (status_bar_ != nullptr) {
            lv_obj_set_style_pad_top(status_bar_, kSafeInsetTop, 0);
        }
    }

    // ------------------------------------------------------------------
    // Yuz (gozler + bosta saat)
    // ------------------------------------------------------------------
    void CreateFace() {
        // Upstream'in emoji kutusu gizleniyor; yerine gozleri koyuyoruz.
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        eyes_.Create(lv_screen_active(), static_cast<LvglTheme*>(current_theme_));
        face_timer_ = lv_timer_create(FaceTimerCb, kFaceTickMs, this);
    }

    // ------------------------------------------------------------------
    // Kurulum
    // ------------------------------------------------------------------
    void CreatePanel() {
        if (panel_ != nullptr) {
            return;  // SetupUI iki kez cagrilirsa paneli tekrar kurmayalim
        }

        HookScreenEvents();
        HookWifiScanEvent();
        CreateOpenStrip();

        panel_ = lv_obj_create(lv_layer_top());
        lv_obj_set_size(panel_, width_, height_);
        lv_obj_set_pos(panel_, 0, 0);
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_style_border_width(panel_, 0, 0);
        lv_obj_set_style_pad_all(panel_, 0, 0);
        lv_obj_remove_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(panel_, GestureEventCb, LV_EVENT_GESTURE, this);

        BuildLauncher();
        view_settings_ = CreateAppView("Ayarlar");
        BuildSettingsTile(view_settings_);
        view_wifi_ = CreateAppView("WiFi");
        BuildWifiTile(view_wifi_);
        view_info_ = CreateAppView("Bilgi");
        BuildInfoTile(view_info_);
        view_actions_ = CreateAppView("Kisayollar");
        BuildActionsTile(view_actions_);
        view_alarm_ = CreateAppView("Alarm");
        BuildAlarmTile(view_alarm_);
        LoadAlarm();
        view_sd_ = CreateAppView("SD Kart");
        BuildSdTile(view_sd_);
        view_gallery_ = CreateAppView("Galeri");
        BuildGalleryTile(view_gallery_);
        view_photo_ = CreateAppView("Gorsel");
        BuildPhotoTile(view_photo_);

        BuildKeyboard();
        info_timer_ = lv_timer_create(InfoTimerCb, kInfoRefreshMs, this);

        StylePanel();
        ShowView(View::kLauncher);
    }

    void HookScreenEvents() {
        // Hareket ve tiklama parmagin altindaki nesneye gider; ekranin tamamini
        // kaplayan container_ ve emoji_box_ tiklanabilir oldugu icin olayi onlar
        // yakalar. EVENT_BUBBLE ile ekrana kadar cikarip tek yerde ele aliyoruz.
        lv_obj_t* screen = lv_screen_active();
        lv_obj_add_event_cb(screen, GestureEventCb, LV_EVENT_GESTURE, this);
        lv_obj_add_event_cb(screen, PressedEventCb, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(screen, ScreenClickedEventCb, LV_EVENT_CLICKED, this);

        for (lv_obj_t* obj : {container_, emoji_box_, top_bar_, status_bar_, bottom_bar_}) {
            if (obj != nullptr) {
                lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
            }
        }
        // Icerik tasmadigi halde scroll denemesi hareketi yutabiliyor.
        for (lv_obj_t* obj : {container_, emoji_box_}) {
            if (obj != nullptr) {
                lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            }
        }
    }

    void CreateOpenStrip() {
        // Yedek acma yolu: ust kenarda gorunmez dokunma seridi.
        open_strip_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(open_strip_);
        lv_obj_set_size(open_strip_, width_, kOpenStripHeight);
        lv_obj_set_pos(open_strip_, 0, 0);
        lv_obj_add_flag(open_strip_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(open_strip_, OpenEventCb, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(open_strip_, GestureEventCb, LV_EVENT_GESTURE, this);
    }

    // Uygulama ekrani: tam ekran, ustte geri oku ve baslik.
    lv_obj_t* CreateAppView(const char* title) {
        lv_obj_t* view = lv_obj_create(panel_);
        lv_obj_set_size(view, width_, height_);
        lv_obj_set_pos(view, 0, 0);
        lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(view, 0, 0);
        lv_obj_set_style_border_width(view, 0, 0);
        lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(view, kSafeInsetX, 0);
        lv_obj_set_style_pad_right(view, kSafeInsetX, 0);
        lv_obj_set_style_pad_top(view, kSafeInsetTop, 0);
        lv_obj_set_style_pad_bottom(view, 14, 0);
        lv_obj_set_style_pad_row(view, 8, 0);
        lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(view, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t* header = lv_obj_create(view);
        lv_obj_remove_style_all(header);
        lv_obj_set_width(header, lv_pct(100));
        lv_obj_set_height(header, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(header, 8, 0);
        lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(header, BackEventCb, LV_EVENT_CLICKED, this);

        lv_obj_t* arrow = CreateLabel(header, MATERIAL_SYMBOLS_ARROW_BACK);
        icon_labels_.push_back(arrow);
        CreateLabel(header, title);
        return view;
    }

    // ------------------------------------------------------------------
    // Menu (ana ekran)
    // ------------------------------------------------------------------
    struct AppEntry {
        const char* icon;
        const char* name;
        uint32_t color;
        View target;
    };

    void BuildLauncher() {
        launcher_ = lv_obj_create(panel_);
        lv_obj_set_size(launcher_, width_, height_);
        lv_obj_set_pos(launcher_, 0, 0);
        lv_obj_set_style_radius(launcher_, 0, 0);
        lv_obj_set_style_border_width(launcher_, 0, 0);
        lv_obj_set_style_bg_opa(launcher_, LV_OPA_TRANSP, 0);
        // 7 uygulama iki sutuna sigmiyordu; 240 px'e uc sutun ancak bu olculerle
        // giriyor: 3*66 + 2*4 = 206 <= 240 - 2*8.
        lv_obj_set_style_pad_left(launcher_, 8, 0);
        lv_obj_set_style_pad_right(launcher_, 8, 0);
        lv_obj_set_style_pad_top(launcher_, kSafeInsetTop + 12, 0);
        lv_obj_set_style_pad_row(launcher_, 8, 0);
        lv_obj_set_style_pad_column(launcher_, 4, 0);
        lv_obj_set_flex_flow(launcher_, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(launcher_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);
        lv_obj_remove_flag(launcher_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(launcher_, LV_OBJ_FLAG_EVENT_BUBBLE);

        static const AppEntry apps[] = {
            {MATERIAL_SYMBOLS_CHAT_BUBBLE, "Sohbet", 0x0A84FF, View::kChat},
            {MATERIAL_SYMBOLS_SCHEDULE, "Saat", 0xFF9F0A, View::kClock},
            {MATERIAL_SYMBOLS_SETTINGS, "Ayarlar", 0x8E8E93, View::kSettings},
            {MATERIAL_SYMBOLS_WIFI, "WiFi", 0x30D158, View::kWifi},
            {MATERIAL_SYMBOLS_INFO, "Bilgi", 0xBF5AF2, View::kInfo},
            {MATERIAL_SYMBOLS_ALARM, "Alarm", 0xFFD60A, View::kAlarm},
            {MATERIAL_SYMBOLS_SD_CARD, "SD Kart", 0x64D2FF, View::kSd},
            {MATERIAL_SYMBOLS_IMAGE, "Galeri", 0x5E5CE6, View::kGallery},
            {MATERIAL_SYMBOLS_POWER_SETTINGS_NEW, "Kisayol", 0xFF453A, View::kActions},
        };
        for (const auto& app : apps) {
            AddAppTile(app);
        }
    }

    void AddAppTile(const AppEntry& app) {
        lv_obj_t* cell = lv_obj_create(launcher_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 66, 74);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 4, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(app.target)));
        lv_obj_add_event_cb(cell, AppTileEventCb, LV_EVENT_CLICKED, this);

        lv_obj_t* box = lv_obj_create(cell);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 46, 46);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(app.color), 0);
        lv_obj_set_style_radius(box, 12, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t* icon = lv_label_create(box);
        lv_label_set_text(icon, app.icon);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(icon);
        icon_labels_.push_back(icon);

        CreateLabel(cell, app.name);
    }

    // ------------------------------------------------------------------
    // Sayfa 1 - Ayarlar
    // ------------------------------------------------------------------
    void BuildSettingsTile(lv_obj_t* tile) {
        lv_obj_t* volume_row = CreateRow(tile);
        CreateLabel(volume_row, "Ses");
        volume_value_label_ = CreateLabel(volume_row, "0");

        volume_slider_ = lv_slider_create(tile);
        lv_obj_set_width(volume_slider_, lv_pct(100));
        lv_obj_set_height(volume_slider_, 10);
        lv_slider_set_range(volume_slider_, 0, 100);
        lv_obj_add_flag(volume_slider_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(volume_slider_, VolumeEventCb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(volume_slider_, VolumeEventCb, LV_EVENT_RELEASED, this);

        lv_obj_t* brightness_row = CreateRow(tile);
        CreateLabel(brightness_row, "Parlaklik");
        brightness_value_label_ = CreateLabel(brightness_row, "0");

        brightness_slider_ = lv_slider_create(tile);
        lv_obj_set_width(brightness_slider_, lv_pct(100));
        lv_obj_set_height(brightness_slider_, 10);
        lv_slider_set_range(brightness_slider_, kMinBrightness, 100);
        lv_obj_add_flag(brightness_slider_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(brightness_slider_, BrightnessEventCb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(brightness_slider_, BrightnessEventCb, LV_EVENT_RELEASED, this);

        lv_obj_t* theme_row = CreateRow(tile);
        CreateLabel(theme_row, "Koyu tema");
        theme_switch_ = lv_switch_create(theme_row);
        lv_obj_add_flag(theme_switch_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(theme_switch_, ThemeEventCb, LV_EVENT_VALUE_CHANGED, this);

        close_button_ = CreateButton(tile, "Menu", &close_button_label_);
        lv_obj_add_event_cb(close_button_, CloseEventCb, LV_EVENT_CLICKED, this);
    }

    // ------------------------------------------------------------------
    // Sayfa 2 - Bilgi
    // ------------------------------------------------------------------
    void BuildInfoTile(lv_obj_t* tile) {
        info_battery_ = CreateInfoRow(tile, "Pil");
        info_wifi_ = CreateInfoRow(tile, "WiFi");
        info_ip_ = CreateInfoRow(tile, "IP");
        info_sd_ = CreateInfoRow(tile, "SD");
        info_version_ = CreateInfoRow(tile, "Surum");
        info_uptime_ = CreateInfoRow(tile, "Calisma");
    }

    lv_obj_t* CreateInfoRow(lv_obj_t* tile, const char* caption) {
        lv_obj_t* row = CreateRow(tile);
        CreateLabel(row, caption);
        return CreateLabel(row, "-");
    }

    // ------------------------------------------------------------------
    // Sayfa 3 - Kisayollar
    // ------------------------------------------------------------------
    void BuildActionsTile(lv_obj_t* tile) {
        chat_button_ = CreateButton(tile, "Sohbeti Baslat / Bitir", &chat_button_label_);
        lv_obj_add_event_cb(chat_button_, ChatEventCb, LV_EVENT_CLICKED, this);

        SetupConfirmButton(tile, wifi_confirm_, "WiFi Ayari", "Emin misin? Tekrar dokun",
                           [this]() {
                               if (on_wifi_config_) {
                                   auto callback = on_wifi_config_;
                                   Application::GetInstance().Schedule(
                                       [callback]() { callback(); });
                               }
                           });

        SetupConfirmButton(tile, restart_confirm_, "Yeniden Baslat", "Emin misin? Tekrar dokun",
                           []() { esp_restart(); });
    }

    void SetupConfirmButton(lv_obj_t* tile, ConfirmButton& confirm, const char* idle_text,
                            const char* confirm_text, std::function<void()> action) {
        confirm.owner = this;
        confirm.idle_text = idle_text;
        confirm.confirm_text = confirm_text;
        confirm.action = std::move(action);
        confirm.button = CreateButton(tile, idle_text, &confirm.label);
        lv_obj_add_event_cb(confirm.button, ConfirmEventCb, LV_EVENT_CLICKED, &confirm);
    }

    // ------------------------------------------------------------------
    // Sayfa 7 - Galeri (SD karttaki gorseller)
    // ------------------------------------------------------------------
    // LVGL dosyayi "S:" surucusu uzerinden aciyor; surucu LV_USE_FS_STDIO ile
    // /sdcard'a bagli (bkz. config.json sdkconfig_append). Cozucu olarak
    // LODEPNG (PNG) ve TJPGD (baseline JPEG) derlenmis durumda.
    //
    // ⚠️ LV_CACHE_DEF_SIZE varsayilani 0, yani goruntu onbellegi KAPALI.
    // LVGL 9'da PNG/JPEG cozucusu cozdugu tamponu onbellege veriyor; yer
    // yoksa acma basarisiz oluyor ve widget hicbir sey cizmiyor. Belirti
    // sinsi: baslik okunabildigi icin en/boy dogru gorunuyor ama ekran
    // siyah kaliyor. Onbellek de config.json'dan aciliyor (4 MB, PSRAM'den).
    //
    // Cozulmus PNG piksel basina 4 bayt tutuyor; sinir onbellege sigsin diye.
    static constexpr int kMaxPixels = 800 * 1000;

    void BuildGalleryTile(lv_obj_t* tile) {
        gallery_hint_ = CreateLabel(tile, "");
        lv_obj_set_width(gallery_hint_, lv_pct(100));
        lv_label_set_long_mode(gallery_hint_, LV_LABEL_LONG_WRAP);

        // Icerik her aciliista yeniden uretiliyor; etiketleri plain_labels_ e
        // EKLEMIYORUZ, yoksa lv_obj_clean sonrasi StylePanel olu isaretciye
        // dokunur (wifi listesinde ayni tuzak var).
        gallery_list_ = lv_obj_create(tile);
        lv_obj_set_width(gallery_list_, lv_pct(100));
        lv_obj_set_flex_grow(gallery_list_, 1);
        lv_obj_set_style_bg_opa(gallery_list_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(gallery_list_, 0, 0);
        lv_obj_set_style_pad_all(gallery_list_, 0, 0);
        lv_obj_set_style_pad_row(gallery_list_, 4, 0);
        lv_obj_set_flex_flow(gallery_list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(gallery_list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(gallery_list_, LV_SCROLLBAR_MODE_OFF);
    }

    void BuildPhotoTile(lv_obj_t* tile) {
        photo_image_ = lv_image_create(tile);
        lv_obj_set_width(photo_image_, lv_pct(100));
        lv_obj_set_flex_grow(photo_image_, 1);
        lv_obj_add_flag(photo_image_, LV_OBJ_FLAG_EVENT_BUBBLE);
        // CONTAIN: en-boy oranini koruyarak widget'a sigacak kadar olcekliyor.
        // Elle olcek hesaplamaktan iyi; olcek ile hizalama LVGL'de birbirine
        // karisiyor.
        lv_image_set_inner_align(photo_image_, LV_IMAGE_ALIGN_CONTAIN);

        photo_note_ = CreateLabel(tile, "");
        lv_obj_set_width(photo_note_, lv_pct(100));
        lv_label_set_long_mode(photo_note_, LV_LABEL_LONG_WRAP);
    }

    void RefreshGallery() {
        lv_obj_clean(gallery_list_);
        gallery_files_.clear();
        if (sd_.list_images) {
            gallery_files_ = sd_.list_images();
        }
        if (gallery_files_.empty()) {
            lv_label_set_text(gallery_hint_,
                              "Kartta gorsel yok. SD Kart sayfasindan dosya sunucusunu acip "
                              "bilgisayardan PNG veya JPEG yukle.");
            return;
        }
        lv_label_set_text_fmt(gallery_hint_, "%d gorsel", static_cast<int>(gallery_files_.size()));
        for (size_t i = 0; i < gallery_files_.size(); i++) {
            AddGalleryRow(gallery_files_[i].c_str(), static_cast<int>(i));
        }
    }

    void AddGalleryRow(const char* text, int index) {
        lv_obj_t* button = lv_button_create(gallery_list_);
        lv_obj_set_width(button, lv_pct(100));
        lv_obj_set_height(button, 30);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
        lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_center(label);
        lv_obj_add_event_cb(button, GalleryRowClickedCb, LV_EVENT_CLICKED, this);
        if (current_theme_ != nullptr) {
            auto* theme = static_cast<LvglTheme*>(current_theme_);
            StyleButton(button, label, theme->chat_background_color(), theme->text_color());
        }
    }

    void ShowPhoto(int index) {
        if (index < 0 || index >= static_cast<int>(gallery_files_.size())) {
            return;
        }
        // Kaynak dizeyi uyede tutuyoruz: lv_image_set_src yolu kopyalamiyor,
        // isaretciyi sakliyor.
        photo_path_ = "S:/" + gallery_files_[index];

        // Once basligi oku: bozuk ya da desteklenmeyen dosyada tam cozumu hic
        // denemeyelim, buyuk dosyada da RAM'i yakmayalim.
        lv_image_header_t header = {};
        if (lv_image_decoder_get_info(photo_path_.c_str(), &header) != LV_RESULT_OK) {
            lv_image_set_src(photo_image_, nullptr);
            lv_label_set_text(photo_note_,
                              "Acilamadi. Yalnizca PNG ve basit (progressive olmayan) JPEG "
                              "destekleniyor.");
            ShowView(View::kPhoto);
            return;
        }
        if (static_cast<int>(header.w) * static_cast<int>(header.h) > kMaxPixels) {
            lv_image_set_src(photo_image_, nullptr);
            lv_label_set_text_fmt(photo_note_,
                                  "Cok buyuk (%dx%d). 240x284 civarina kucultup tekrar yukle.",
                                  static_cast<int>(header.w), static_cast<int>(header.h));
            ShowView(View::kPhoto);
            return;
        }

        lv_image_set_src(photo_image_, photo_path_.c_str());
        lv_label_set_text_fmt(photo_note_, "%s  %dx%d", gallery_files_[index].c_str(),
                              static_cast<int>(header.w), static_cast<int>(header.h));
        ShowView(View::kPhoto);
    }

    // ------------------------------------------------------------------
    // Sayfa 6 - SD kart
    // ------------------------------------------------------------------
    void BuildSdTile(lv_obj_t* tile) {
        sd_size_ = CreateInfoRow(tile, "Kart");
        sd_free_ = CreateInfoRow(tile, "Bos");
        sd_files_ = CreateInfoRow(tile, "Dosya");

        lv_obj_t* server_row = CreateRow(tile);
        CreateLabel(server_row, "Dosya sunucusu");
        sd_server_switch_ = lv_switch_create(server_row);
        lv_obj_add_flag(sd_server_switch_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(sd_server_switch_, SdServerEventCb, LV_EVENT_VALUE_CHANGED, this);

        sd_url_ = CreateLabel(tile, "");
        lv_obj_set_width(sd_url_, lv_pct(100));
        lv_label_set_long_mode(sd_url_, LV_LABEL_LONG_WRAP);
    }

    void RefreshSd() {
        if (sd_size_ == nullptr) {
            return;
        }
        lv_label_set_text(sd_size_, sd_.status ? sd_.status().c_str() : "-");
        lv_label_set_text(sd_free_, sd_.free_space ? sd_.free_space().c_str() : "-");
        if (sd_.file_count) {
            lv_label_set_text_fmt(sd_files_, "%d", sd_.file_count());
        } else {
            lv_label_set_text(sd_files_, "-");
        }

        bool running = sd_.server_running && sd_.server_running();
        if (running) {
            lv_obj_add_state(sd_server_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sd_server_switch_, LV_STATE_CHECKED);
        }
        if (!running) {
            lv_label_set_text(sd_url_, "Bilgisayardan dosya atmak icin sunucuyu ac.");
        } else if (sd_.server_url) {
            std::string url = sd_.server_url();
            lv_label_set_text(sd_url_,
                              url.empty() ? "Ag yok" : ("Tarayicida ac:  http://" + url).c_str());
        }
    }

    // ------------------------------------------------------------------
    // Sayfa 5 - Alarm
    // ------------------------------------------------------------------
    void BuildAlarmTile(lv_obj_t* tile) {
        alarm_time_label_ = CreateLabel(tile, "07:00");
        lv_obj_set_style_text_font(alarm_time_label_, &font_noto_sans_basic_30_4, 0);
        lv_obj_set_width(alarm_time_label_, lv_pct(100));
        lv_obj_set_style_text_align(alarm_time_label_, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t* hour_row = CreateRow(tile);
        CreateLabel(hour_row, "Saat");
        AddStepButton(hour_row, "-", -60);
        AddStepButton(hour_row, "+", 60);

        lv_obj_t* minute_row = CreateRow(tile);
        CreateLabel(minute_row, "Dakika");
        AddStepButton(minute_row, "-", -1);
        AddStepButton(minute_row, "+", 1);

        lv_obj_t* enable_row = CreateRow(tile);
        CreateLabel(enable_row, "Alarm acik");
        alarm_switch_ = lv_switch_create(enable_row);
        lv_obj_add_flag(alarm_switch_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(alarm_switch_, AlarmSwitchEventCb, LV_EVENT_VALUE_CHANGED, this);

        lv_obj_t* hint = CreateLabel(tile, "Her gun calar. Susturmak icin ekrana dokun.");
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    }

    // step dakika cinsinden: +-1 dakika, +-60 saat. Tek isleyici yetsin diye
    // deger butonun user_data'sinda tasiniyor.
    void AddStepButton(lv_obj_t* parent, const char* text, int step) {
        lv_obj_t* label = nullptr;
        lv_obj_t* button = CreateButton(parent, text, &label);
        lv_obj_set_width(button, 54);
        lv_obj_set_height(button, 34);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(step)));
        lv_obj_add_event_cb(button, AlarmStepEventCb, LV_EVENT_CLICKED, this);
        step_buttons_.push_back({button, label});
    }

    void RefreshAlarmView() {
        if (alarm_time_label_ == nullptr) {
            return;
        }
        lv_label_set_text_fmt(alarm_time_label_, "%02d:%02d", alarm_hour_, alarm_minute_);
        if (alarm_enabled_) {
            lv_obj_add_state(alarm_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(alarm_switch_, LV_STATE_CHECKED);
        }
    }

    void LoadAlarm() {
        Settings settings("alarm", false);
        alarm_hour_ = settings.GetInt("hour", 7);
        alarm_minute_ = settings.GetInt("minute", 0);
        alarm_enabled_ = settings.GetBool("on", false);
        RefreshAlarmView();
    }

    void SaveAlarm() {
        Settings settings("alarm", true);
        settings.SetInt("hour", alarm_hour_);
        settings.SetInt("minute", alarm_minute_);
        settings.SetBool("on", alarm_enabled_);
    }

    // Saniyede bir, yuz zamanlayicisindan cagriliyor.
    void AlarmTick() {
        if (alarm_ring_ticks_ > 0) {
            alarm_ring_ticks_--;
            if (alarm_ring_ticks_ % 3 == 0 && on_alarm_ring_) {
                on_alarm_ring_();  // ses kisa, 3 saniyede bir tekrarliyoruz
            }
            return;
        }
        if (!alarm_enabled_) {
            return;
        }
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        // Saat sunucudan (OTA yaniti) veya RTC'den gelene kadar 1970'teyiz.
        if (t == nullptr || t->tm_year + 1900 < 2024) {
            return;
        }
        int minute_of_day = t->tm_hour * 60 + t->tm_min;
        if (t->tm_hour == alarm_hour_ && t->tm_min == alarm_minute_ &&
            alarm_last_fired_ != minute_of_day) {
            alarm_last_fired_ = minute_of_day;
            StartRinging();
        }
    }

    void StartRinging() {
        alarm_ring_ticks_ = 60;  // en fazla bir dakika
        // Saat ekranina geciyoruz: hem saat gorunur, hem panel kapandigi icin
        // bildirim yazisi (ust bardaki etiket) ortaya cikar.
        ShowView(View::kClock);
        ShowNotification("Alarm", 10000);
        if (on_alarm_ring_) {
            on_alarm_ring_();
        }
    }

    void StopRinging() {
        if (alarm_ring_ticks_ > 0) {
            alarm_ring_ticks_ = 0;
            ShowNotification("Alarm kapatildi", 2000);
        }
    }

    // Kullanici saati ayarlarken denk gelen dakika hemen calmasin.
    void SuppressImmediateRing() {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        alarm_last_fired_ = (t != nullptr) ? t->tm_hour * 60 + t->tm_min : -1;
    }

    // ------------------------------------------------------------------
    // Sayfa 4 - WiFi
    // ------------------------------------------------------------------
    void BuildWifiTile(lv_obj_t* tile) {
        wifi_status_label_ = CreateLabel(tile, "-");
        lv_obj_set_width(wifi_status_label_, lv_pct(100));
        lv_label_set_long_mode(wifi_status_label_, LV_LABEL_LONG_DOT);

        lv_obj_t* buttons = CreateRow(tile);
        wifi_scan_button_ = CreateButton(buttons, "Tara", &wifi_scan_button_label_);
        lv_obj_set_width(wifi_scan_button_, lv_pct(48));
        lv_obj_set_height(wifi_scan_button_, 32);
        lv_obj_add_event_cb(wifi_scan_button_, WifiScanEventCb, LV_EVENT_CLICKED, this);

        wifi_add_button_ = CreateButton(buttons, "Elle Ekle", &wifi_add_button_label_);
        lv_obj_set_width(wifi_add_button_, lv_pct(48));
        lv_obj_set_height(wifi_add_button_, 32);
        lv_obj_add_event_cb(wifi_add_button_, AddNetworkEventCb, LV_EVENT_CLICKED, this);

        // Kayitli aglar ve tarama sonuclari buraya diziliyor. Icerik surekli
        // yeniden uretildigi icin buradaki etiketler plain_labels_ e EKLENMIYOR;
        // eklenseydi lv_obj_clean sonrasi StylePanel serbest birakilmis
        // isaretcilere dokunurdu.
        wifi_list_ = lv_obj_create(tile);
        lv_obj_set_width(wifi_list_, lv_pct(100));
        lv_obj_set_flex_grow(wifi_list_, 1);
        lv_obj_set_style_bg_opa(wifi_list_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wifi_list_, 0, 0);
        lv_obj_set_style_pad_all(wifi_list_, 0, 0);
        lv_obj_set_style_pad_row(wifi_list_, 4, 0);
        lv_obj_set_flex_flow(wifi_list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(wifi_list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(wifi_list_, LV_SCROLLBAR_MODE_OFF);
    }

    void RefreshWifiStatus() {
        if (wifi_status_label_ == nullptr) {
            return;
        }
        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConnected()) {
            lv_label_set_text_fmt(wifi_status_label_, "%s  %d dBm", wifi.GetSsid().c_str(),
                                  wifi.GetRssi());
        } else {
            lv_label_set_text(wifi_status_label_, "Bagli degil");
        }
    }

    lv_obj_t* AddListRow(const char* text, int index, lv_event_cb_t cb, bool long_press_deletes) {
        lv_obj_t* button = lv_button_create(wifi_list_);
        lv_obj_set_width(button, lv_pct(100));
        lv_obj_set_height(button, 30);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
        lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_center(label);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
        if (long_press_deletes) {
            lv_obj_add_event_cb(button, SavedNetworkDeleteCb, LV_EVENT_LONG_PRESSED, this);
        }
        if (current_theme_ != nullptr) {
            auto* theme = static_cast<LvglTheme*>(current_theme_);
            StyleButton(button, label, theme->chat_background_color(), theme->text_color());
        }
        return button;
    }

    // Olay dongusunu erken kurup isleyicimizi bilesenden ONCE kaydediyoruz.
    // Bilesen de ayni dongude ESP_ERR_INVALID_STATE'i zarifce karsiliyor
    // (wifi_manager.cc: sadece baska hata olursa sikayet ediyor).
    void HookWifiScanEvent() {
        esp_event_loop_create_default();
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, ScanDoneHandler,
                                            this, nullptr);
    }

    void StartScan() {
        if (scan_pending_) {
            return;
        }
        scan_pending_ = true;
        lv_label_set_text(wifi_scan_button_label_, "...");

        wifi_scan_config_t cfg = {};
        cfg.show_hidden = false;
        cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        cfg.scan_time.active.min = 100;
        cfg.scan_time.active.max = 300;
        esp_err_t err = esp_wifi_scan_start(&cfg, false);  // bloklamayan
        // Bilesen zaten tariyorsa ESP_ERR_WIFI_STATE gelir; sorun degil, o
        // taramanin sonucunu biz tuketecegiz.
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGW("SettingsPanel", "scan_start: %s", esp_err_to_name(err));
            scan_pending_ = false;
            lv_label_set_text(wifi_scan_button_label_, "Tara");
        }
    }

    // WIFI_EVENT_SCAN_DONE - olay gorevinde calisir, LVGL gorevinde degil.
    static void ScanDoneHandler(void* arg, esp_event_base_t, int32_t, void*) {
        auto* self = static_cast<SettingsPanelDisplay*>(arg);
        if (!self->scan_pending_) {
            return;  // bilesenin kendi taramasi - dokunma
        }
        self->scan_pending_ = false;

        std::vector<ScanResult> found;
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        if (count > kMaxScanResults) {
            count = kMaxScanResults;
        }
        if (count > 0) {
            std::vector<wifi_ap_record_t> records(count);
            if (esp_wifi_scan_get_ap_records(&count, records.data()) == ESP_OK) {
                for (uint16_t i = 0; i < count; i++) {
                    std::string ssid(reinterpret_cast<const char*>(records[i].ssid));
                    if (ssid.empty()) {
                        continue;
                    }
                    ScanResult item;
                    item.ssid = ssid;
                    item.rssi = records[i].rssi;
                    item.encrypted = records[i].authmode != WIFI_AUTH_OPEN;
                    found.push_back(item);
                }
            }
        }

        DisplayLockGuard lock(self);
        self->scan_results_ = std::move(found);
        lv_label_set_text(self->wifi_scan_button_label_, "Tara");
        if (self->current_view_ == View::kWifi) {
            self->ShowScanResults();
        }
    }

    void ShowScanResults() {
        lv_obj_clean(wifi_list_);
        if (scan_results_.empty()) {
            AddListRow("Ag bulunamadi", -1, NoopEventCb, false);
            return;
        }
        for (size_t i = 0; i < scan_results_.size(); i++) {
            const auto& r = scan_results_[i];
            char row[96];
            snprintf(row, sizeof(row), "%s  %d dBm%s", r.ssid.c_str(), r.rssi,
                     r.encrypted ? "" : "  (acik)");
            AddListRow(row, static_cast<int>(i), ScanResultClickedCb, false);
        }
    }

    void ShowSavedNetworks() {
        lv_obj_clean(wifi_list_);
        const auto& list = SsidManager::GetInstance().GetSsidList();
        if (list.empty()) {
            AddListRow("Kayitli ag yok", -1, NoopEventCb, false);
            return;
        }
        int index = 0;
        for (const auto& item : list) {
            AddListRow(item.ssid.c_str(), index, SavedNetworkClickedCb, true);
            index++;
        }
    }

    // ------------------------------------------------------------------
    // Sifre klavyesi - LV_USE_KEYBOARD=n oldugu icin lv_buttonmatrix ile elde
    // ------------------------------------------------------------------
    static const char* const* KeyboardMap(bool upper, bool symbols) {
        static const char* lower_map[] = {
            "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
            "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
            "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
            "^", "z", "x", "c", "v", "b", "n", "m", "DEL", "\n",
            "#+=", "SP", "Geri", "OK", ""};
        static const char* upper_map[] = {
            "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
            "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
            "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
            "^", "Z", "X", "C", "V", "B", "N", "M", "DEL", "\n",
            "#+=", "SP", "Geri", "OK", ""};
        static const char* symbol_map[] = {
            "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
            "!", "@", "#", "$", "%", "&", "*", "(", ")", "\n",
            "-", "_", "+", "=", "/", ":", ";", ",", ".", "\n",
            "?", "'", "[", "]", "{", "}", "<", ">", "DEL", "\n",
            "abc", "SP", "Geri", "OK", ""};
        if (symbols) {
            return symbol_map;
        }
        return upper ? upper_map : lower_map;
    }

    void BuildKeyboard() {
        kb_overlay_ = lv_obj_create(panel_);
        lv_obj_set_size(kb_overlay_, width_, height_);
        lv_obj_set_pos(kb_overlay_, 0, 0);
        lv_obj_add_flag(kb_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(kb_overlay_, 0, 0);
        lv_obj_set_style_border_width(kb_overlay_, 0, 0);
        lv_obj_set_style_pad_left(kb_overlay_, 6, 0);
        lv_obj_set_style_pad_right(kb_overlay_, 6, 0);
        lv_obj_set_style_pad_top(kb_overlay_, kSafeInsetTop, 0);
        lv_obj_set_style_pad_bottom(kb_overlay_, 6, 0);
        lv_obj_set_style_pad_row(kb_overlay_, 4, 0);
        lv_obj_set_flex_flow(kb_overlay_, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(kb_overlay_, LV_OBJ_FLAG_SCROLLABLE);

        kb_title_ = lv_label_create(kb_overlay_);
        lv_label_set_text(kb_title_, "");
        lv_label_set_long_mode(kb_title_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(kb_title_, lv_pct(100));

        kb_field_ = lv_label_create(kb_overlay_);
        lv_label_set_text(kb_field_, "");
        lv_label_set_long_mode(kb_field_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(kb_field_, lv_pct(100));

        kb_matrix_ = lv_buttonmatrix_create(kb_overlay_);
        lv_obj_set_width(kb_matrix_, lv_pct(100));
        lv_obj_set_flex_grow(kb_matrix_, 1);
        lv_buttonmatrix_set_map(kb_matrix_, KeyboardMap(false, false));
        lv_obj_add_event_cb(kb_matrix_, KeyboardEventCb, LV_EVENT_VALUE_CHANGED, this);
    }

    void OpenKeyboard(const char* title, bool entering_ssid) {
        kb_entering_ssid_ = entering_ssid;
        kb_upper_ = false;
        kb_symbols_ = false;
        lv_label_set_text(kb_title_, title);
        lv_label_set_text(kb_field_, entering_ssid ? kb_ssid_.c_str() : kb_password_.c_str());
        lv_buttonmatrix_set_map(kb_matrix_, KeyboardMap(false, false));
        lv_obj_remove_flag(kb_overlay_, LV_OBJ_FLAG_HIDDEN);
    }

    // Ag adi girisi (ilk asama)
    void ShowSsidKeyboard() {
        NotifyActivity();
        kb_ssid_.clear();
        kb_password_.clear();
        OpenKeyboard("Ag adi", true);
    }

    // Sifre girisi (ikinci asama)
    void ShowPasswordKeyboard() {
        kb_password_.clear();
        char title[80];
        snprintf(title, sizeof(title), "%s sifresi", kb_ssid_.c_str());
        OpenKeyboard(title, false);
    }

    void HideKeyboard() {
        if (kb_overlay_ != nullptr) {
            lv_obj_add_flag(kb_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void OnKeyPressed() {
        uint32_t id = lv_buttonmatrix_get_selected_button(kb_matrix_);
        const char* text = lv_buttonmatrix_get_button_text(kb_matrix_, id);
        if (text == nullptr) {
            return;
        }
        NotifyActivity();
        std::string key(text);

        std::string& field = kb_entering_ssid_ ? kb_ssid_ : kb_password_;

        if (key == "DEL") {
            if (!field.empty()) {
                field.pop_back();
            }
        } else if (key == "^") {
            kb_upper_ = !kb_upper_;
            lv_buttonmatrix_set_map(kb_matrix_, KeyboardMap(kb_upper_, kb_symbols_));
        } else if (key == "#+=" || key == "abc") {
            kb_symbols_ = !kb_symbols_;
            lv_buttonmatrix_set_map(kb_matrix_, KeyboardMap(kb_upper_, kb_symbols_));
        } else if (key == "SP") {
            field += ' ';
        } else if (key == "Geri") {
            if (kb_entering_ssid_) {
                HideKeyboard();          // ilk asama: klavyeden tamamen cik
            } else {
                OpenKeyboard("Ag adi", true);  // sifre asamasi: ag adina don
            }
            return;
        } else if (key == "OK") {
            if (kb_entering_ssid_) {
                if (kb_ssid_.empty()) {
                    return;  // bos ag adiyla ilerleme
                }
                ShowPasswordKeyboard();
            } else {
                SaveNetwork();
            }
            return;
        } else {
            field += key;
        }
        lv_label_set_text(kb_field_, field.c_str());
    }

    void SaveNetwork() {
        std::string ssid = kb_ssid_;
        std::string password = kb_password_;
        HideKeyboard();
        lv_label_set_text_fmt(wifi_status_label_, "%s kaydedildi, baglaniliyor", ssid.c_str());
        ShowSavedNetworks();

        // NVS yazimi ve WiFi yeniden baslatma LVGL gorevinde yapilmamali.
        Application::GetInstance().Schedule([ssid, password]() {
            SsidManager::GetInstance().AddSsid(ssid, password);
            auto& wifi = WifiManager::GetInstance();
            wifi.StopStation();
            vTaskDelay(pdMS_TO_TICKS(500));
            wifi.StartStation();
        });
    }

    // ------------------------------------------------------------------
    // Widget yardimcilari
    // ------------------------------------------------------------------
    lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text) {
        lv_obj_t* label = lv_label_create(parent);
        lv_label_set_text(label, text);
        plain_labels_.push_back(label);
        return label;
    }

    lv_obj_t* CreateRow(lv_obj_t* parent) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        return row;
    }

    lv_obj_t* CreateButton(lv_obj_t* parent, const char* text, lv_obj_t** out_label) {
        lv_obj_t* button = lv_button_create(parent);
        // Yukari kaydirma paneli kapatabilsin diye hareket olayi ust nesneye iletilmeli.
        lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_width(button, lv_pct(100));
        lv_obj_set_height(button, 38);
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        *out_label = label;
        return button;
    }

    // ------------------------------------------------------------------
    // Tema
    // ------------------------------------------------------------------
    // Panel lv_layer_top() altinda oldugu icin ekranin yazi tipi/rengi mirasla
    // gelmez; hepsini acikca vermek zorundayiz.
    void StylePanel() {
        if (panel_ == nullptr || current_theme_ == nullptr) {
            return;
        }
        auto* theme = static_cast<LvglTheme*>(current_theme_);

        lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(panel_, theme->background_color(), 0);
        lv_obj_set_style_text_color(panel_, theme->text_color(), 0);
        lv_obj_set_style_text_font(panel_, theme->text_font()->font(), 0);

        for (lv_obj_t* label : plain_labels_) {
            lv_obj_set_style_text_color(label, theme->text_color(), 0);
        }
        // Menu simgeleri ve geri oku Material Symbols yazi tipiyle cizilir;
        // normal metin fontunda bu karakterler yok.
        for (lv_obj_t* icon : icon_labels_) {
            lv_obj_set_style_text_font(icon, theme->large_icon_font()->font(), 0);
        }

        for (lv_obj_t* slider : {volume_slider_, brightness_slider_}) {
            lv_obj_set_style_bg_color(slider, theme->chat_background_color(), LV_PART_MAIN);
            lv_obj_set_style_bg_color(slider, theme->text_color(), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(slider, theme->text_color(), LV_PART_KNOB);
        }

        // -Werror=deprecated-enum-enum-conversion: lv_part_t ile lv_state_t dogrudan
        // OR'lanamiyor, secici tipine cevirmek gerekiyor.
        lv_style_selector_t checked_indicator = static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                                static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
        for (lv_obj_t* sw : {theme_switch_, alarm_switch_, sd_server_switch_}) {
            if (sw == nullptr) {
                continue;
            }
            lv_obj_set_style_bg_color(sw, theme->chat_background_color(), LV_PART_MAIN);
            lv_obj_set_style_bg_color(sw, theme->text_color(), checked_indicator);
        }

        StyleButton(close_button_, close_button_label_, theme->chat_background_color(),
                    theme->text_color());
        StyleButton(chat_button_, chat_button_label_, theme->chat_background_color(),
                    theme->text_color());
        StyleButton(wifi_add_button_, wifi_add_button_label_, theme->chat_background_color(),
                    theme->text_color());
        StyleButton(wifi_scan_button_, wifi_scan_button_label_, theme->chat_background_color(),
                    theme->text_color());
        for (const auto& pair : step_buttons_) {
            StyleButton(pair.button, pair.label, theme->chat_background_color(),
                        theme->text_color());
        }
        if (kb_overlay_ != nullptr) {
            lv_obj_set_style_bg_opa(kb_overlay_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(kb_overlay_, theme->background_color(), 0);
            lv_obj_set_style_text_color(kb_overlay_, theme->text_color(), 0);
            lv_obj_set_style_text_font(kb_overlay_, theme->text_font()->font(), 0);
            lv_obj_set_style_text_color(kb_title_, theme->text_color(), 0);
            lv_obj_set_style_text_color(kb_field_, theme->text_color(), 0);
        }
        if (wifi_status_label_ != nullptr) {
            lv_obj_set_style_text_color(wifi_status_label_, theme->text_color(), 0);
        }
        RefreshConfirmStyle(wifi_confirm_);
        RefreshConfirmStyle(restart_confirm_);
    }

    void StyleButton(lv_obj_t* button, lv_obj_t* label, lv_color_t bg, lv_color_t text) {
        if (button == nullptr) {
            return;
        }
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(button, bg, 0);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_text_color(label, text, 0);
    }

    void RefreshConfirmStyle(ConfirmButton& confirm) {
        if (confirm.button == nullptr || current_theme_ == nullptr) {
            return;
        }
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        if (confirm.pending) {
            // Onay rengi temadan bagimsiz sabit: light temada low_battery_color siyah.
            StyleButton(confirm.button, confirm.label, lv_color_hex(0xC62828),
                        lv_color_hex(0xFFFFFF));
        } else {
            StyleButton(confirm.button, confirm.label, theme->chat_background_color(),
                        theme->text_color());
        }
    }

    // ------------------------------------------------------------------
    // Panel ac/kapa
    // ------------------------------------------------------------------
    // Tek gecis noktasi: hangi ekranin gorunecegine burasi karar verir.
    // kChat ve kClock kabugu gizler; altta ekranda duran gozler/saat gorunur.
    void ShowView(View v) {
        if (panel_ == nullptr) {
            return;
        }
        current_view_ = v;
        bool shell_visible = (v != View::kChat && v != View::kClock);

        if (shell_visible) {
            eyes_.SetHidden(true);
            lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(open_strip_, LV_OBJ_FLAG_HIDDEN);
        } else {
            HideKeyboard();
            lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(open_strip_, LV_OBJ_FLAG_HIDDEN);
            eyes_.SetHidden(false);
            eyes_.ForceClock(v == View::kClock);
        }

        for (lv_obj_t* screen : {launcher_, view_settings_, view_wifi_, view_info_, view_actions_,
                                 view_alarm_, view_sd_, view_gallery_, view_photo_}) {
            if (screen != nullptr) {
                lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
            }
        }

        lv_obj_t* active = nullptr;
        switch (v) {
            case View::kLauncher:
                active = launcher_;
                break;
            case View::kSettings:
                active = view_settings_;
                SyncControlsFromDevice();
                break;
            case View::kWifi:
                active = view_wifi_;
                RefreshWifiStatus();
                ShowSavedNetworks();
                break;
            case View::kInfo:
                active = view_info_;
                RefreshInfo();
                break;
            case View::kActions:
                active = view_actions_;
                break;
            case View::kAlarm:
                active = view_alarm_;
                RefreshAlarmView();
                break;
            case View::kSd:
                active = view_sd_;
                RefreshSd();
                break;
            case View::kGallery:
                active = view_gallery_;
                RefreshGallery();
                break;
            case View::kPhoto:
                active = view_photo_;
                break;
            default:
                break;
        }
        if (active != nullptr) {
            lv_obj_remove_flag(active, LV_OBJ_FLAG_HIDDEN);
        }

        ResetConfirm(wifi_confirm_);
        ResetConfirm(restart_confirm_);
        NotifyActivity();
        if (v != View::kClock) {
            eyes_.NotifyActivity();
        }
    }

    void SyncControlsFromDevice() {
        auto& board = Board::GetInstance();

        auto* codec = board.GetAudioCodec();
        if (codec != nullptr) {
            int volume = codec->output_volume();
            lv_slider_set_value(volume_slider_, volume, LV_ANIM_OFF);
            lv_label_set_text_fmt(volume_value_label_, "%d", volume);
        }

        auto* backlight = board.GetBacklight();
        if (backlight != nullptr) {
            int brightness = backlight->brightness();
            if (brightness < kMinBrightness) {
                brightness = kMinBrightness;
            }
            lv_slider_set_value(brightness_slider_, brightness, LV_ANIM_OFF);
            lv_label_set_text_fmt(brightness_value_label_, "%d", brightness);
        }

        if (current_theme_ != nullptr && current_theme_->name() == "dark") {
            lv_obj_add_state(theme_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(theme_switch_, LV_STATE_CHECKED);
        }
    }

    void RefreshInfo() {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            lv_label_set_text_fmt(info_battery_, "%d%s", level, charging ? "% +" : "%");
        } else {
            lv_label_set_text(info_battery_, "-");
        }

        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConnected()) {
            lv_label_set_text_fmt(info_wifi_, "%d dBm", wifi.GetRssi());
            lv_label_set_text(info_ip_, wifi.GetIpAddress().c_str());
        } else {
            lv_label_set_text(info_wifi_, "yok");
            lv_label_set_text(info_ip_, "-");
        }

        if (sd_info_provider_) {
            lv_label_set_text(info_sd_, sd_info_provider_().c_str());
        } else {
            lv_label_set_text(info_sd_, "-");
        }

        const esp_app_desc_t* app_desc = esp_app_get_description();
        lv_label_set_text(info_version_, app_desc != nullptr ? app_desc->version : "-");

        int64_t seconds = esp_timer_get_time() / 1000000;
        lv_label_set_text_fmt(info_uptime_, "%02d:%02d:%02d", static_cast<int>(seconds / 3600),
                              static_cast<int>((seconds / 60) % 60), static_cast<int>(seconds % 60));
    }

    void NotifyActivity() {
        if (on_activity_) {
            on_activity_();
        }
    }

    // ------------------------------------------------------------------
    // Olay isleyicileri
    // ------------------------------------------------------------------
    void OnVolumeEvent(lv_event_t* e) {
        int value = lv_slider_get_value(volume_slider_);
        lv_label_set_text_fmt(volume_value_label_, "%d", value);
        NotifyActivity();

        // SetOutputVolume her cagrida NVS'e yaziyor (audio_codec.cc:44), o yuzden
        // surukleme boyunca degil, sadece parmak kalkinca uyguluyoruz.
        if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
            Application::GetInstance().Schedule([value]() {
                auto* codec = Board::GetInstance().GetAudioCodec();
                if (codec != nullptr) {
                    codec->SetOutputVolume(value);
                }
            });
        }
    }

    void OnBrightnessEvent(lv_event_t* e) {
        int value = lv_slider_get_value(brightness_slider_);
        lv_label_set_text_fmt(brightness_value_label_, "%d", value);
        NotifyActivity();

        auto* backlight = Board::GetInstance().GetBacklight();
        if (backlight == nullptr) {
            return;
        }
        bool released = lv_event_get_code(e) == LV_EVENT_RELEASED;
        // Surukleme sirasinda canli onizleme (sadece PWM), birakinca NVS'e kalici.
        backlight->SetBrightness(static_cast<uint8_t>(value), released);
    }

    void OnThemeEvent() {
        bool dark = lv_obj_has_state(theme_switch_, LV_STATE_CHECKED);
        NotifyActivity();
        // SetTheme LVGL kilidini alir; LVGL gorevinden degil ana gorevden cagiralim.
        Application::GetInstance().Schedule([this, dark]() {
            auto* theme = LvglThemeManager::GetInstance().GetTheme(dark ? "dark" : "light");
            if (theme != nullptr) {
                SetTheme(theme);
            }
        });
    }

    void OnConfirmPress(ConfirmButton& confirm) {
        NotifyActivity();
        if (!confirm.pending) {
            confirm.pending = true;
            lv_label_set_text(confirm.label, confirm.confirm_text);
            RefreshConfirmStyle(confirm);
            if (confirm.timer == nullptr) {
                confirm.timer = lv_timer_create(ConfirmTimeoutCb, kConfirmTimeoutMs, &confirm);
                lv_timer_set_repeat_count(confirm.timer, 1);
            }
            return;
        }

        auto action = confirm.action;
        ResetConfirm(confirm);
        ShowView(View::kChat);
        if (action) {
            action();
        }
    }

    void ResetConfirm(ConfirmButton& confirm) {
        if (confirm.timer != nullptr) {
            lv_timer_delete(confirm.timer);
            confirm.timer = nullptr;
        }
        if (!confirm.pending) {
            return;
        }
        confirm.pending = false;
        lv_label_set_text(confirm.label, confirm.idle_text);
        RefreshConfirmStyle(confirm);
    }

    void OnChatButton() {
        NotifyActivity();
        ShowView(View::kChat);
        Application::GetInstance().ToggleChatState();
    }

    // Ekrana dokunmak sohbeti baslatir/bitirir. Turkce wake word mumkun olmadigi
    // icin (ESP-SR sadece Ingilizce/Mandarin) asil kullanim yolu bu.
    void OnScreenClicked() {
        if (current_view_ != View::kChat || gesture_handled_) {
            return;
        }
        NotifyActivity();
        // ToggleChatState sadece bir event biti set ediyor; durum kontrolunu
        // Application::HandleToggleChatEvent kendisi yapiyor.
        Application::GetInstance().ToggleChatState();
    }

    void OnGesture() {
        lv_indev_t* indev = lv_indev_active();
        if (indev == nullptr) {
            return;
        }
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_BOTTOM) {
            // Sohbet veya saat ekranindan menuye
            if (current_view_ == View::kChat || current_view_ == View::kClock) {
                gesture_handled_ = true;
                ShowView(View::kLauncher);
            }
        } else if (dir == LV_DIR_TOP) {
            // Her yerden sohbete
            gesture_handled_ = true;
            ShowView(View::kChat);
        }
    }

    // ------------------------------------------------------------------
    // Statik LVGL koprulleri
    // ------------------------------------------------------------------
    static SettingsPanelDisplay* Self(lv_event_t* e) {
        return static_cast<SettingsPanelDisplay*>(lv_event_get_user_data(e));
    }

    static void GestureEventCb(lv_event_t* e) { Self(e)->OnGesture(); }
    static void PressedEventCb(lv_event_t* e) {
        auto* self = Self(e);
        self->gesture_handled_ = false;
        self->eyes_.NotifyActivity();
        self->StopRinging();  // ekrana dokunmak calan alarmi susturur
    }
    static void ScreenClickedEventCb(lv_event_t* e) { Self(e)->OnScreenClicked(); }
    static void OpenEventCb(lv_event_t* e) { Self(e)->ShowView(View::kLauncher); }
    static void CloseEventCb(lv_event_t* e) { Self(e)->ShowView(View::kLauncher); }
    // Gorsel tam ekrandan geri okuna basinca menuye degil galeriye donmeli.
    static void BackEventCb(lv_event_t* e) {
        auto* self = Self(e);
        self->ShowView(self->current_view_ == View::kPhoto ? View::kGallery : View::kLauncher);
    }

    static void GalleryRowClickedCb(lv_event_t* e) {
        auto* self = Self(e);
        self->NotifyActivity();
        self->ShowPhoto(RowIndex(e));
    }

    static void AppTileEventCb(lv_event_t* e) {
        auto* self = Self(e);
        // Ikon kutusuna dokunulunca olay oradan gelir; bize isleyicinin bagli
        // oldugu hucre lazim, o yuzden current_target.
        auto* cell = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        int target = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell)));
        self->ShowView(static_cast<View>(target));
    }
    static void VolumeEventCb(lv_event_t* e) { Self(e)->OnVolumeEvent(e); }
    static void BrightnessEventCb(lv_event_t* e) { Self(e)->OnBrightnessEvent(e); }
    static void ThemeEventCb(lv_event_t* e) { Self(e)->OnThemeEvent(); }
    static void ChatEventCb(lv_event_t* e) { Self(e)->OnChatButton(); }

    static void AlarmStepEventCb(lv_event_t* e) {
        auto* self = Self(e);
        auto* button = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        int step = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
        self->NotifyActivity();
        int total = (self->alarm_hour_ * 60 + self->alarm_minute_ + step + 1440) % 1440;
        self->alarm_hour_ = total / 60;
        self->alarm_minute_ = total % 60;
        self->SuppressImmediateRing();
        self->SaveAlarm();
        self->RefreshAlarmView();
    }

    static void AlarmSwitchEventCb(lv_event_t* e) {
        auto* self = Self(e);
        self->NotifyActivity();
        self->alarm_enabled_ = lv_obj_has_state(self->alarm_switch_, LV_STATE_CHECKED);
        self->SuppressImmediateRing();
        self->SaveAlarm();
    }

    static void SdServerEventCb(lv_event_t* e) {
        auto* self = Self(e);
        self->NotifyActivity();
        if (self->sd_.set_server) {
            self->sd_.set_server(lv_obj_has_state(self->sd_server_switch_, LV_STATE_CHECKED));
        }
        self->RefreshSd();
    }

    static void AddNetworkEventCb(lv_event_t* e) { Self(e)->ShowSsidKeyboard(); }
    static void WifiScanEventCb(lv_event_t* e) { Self(e)->StartScan(); }

    static void ScanResultClickedCb(lv_event_t* e) {
        auto* self = Self(e);
        int index = RowIndex(e);
        if (index < 0 || index >= static_cast<int>(self->scan_results_.size())) {
            return;
        }
        self->NotifyActivity();
        self->kb_ssid_ = self->scan_results_[index].ssid;
        self->kb_password_.clear();
        self->ShowPasswordKeyboard();  // ag adi hazir, dogrudan sifreye gec
    }
    static void KeyboardEventCb(lv_event_t* e) { Self(e)->OnKeyPressed(); }
    static void NoopEventCb(lv_event_t* e) { (void)e; }

    static int RowIndex(lv_event_t* e) {
        auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        return static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
    }

    static void SavedNetworkClickedCb(lv_event_t* e) {
        auto* self = Self(e);
        int index = RowIndex(e);
        if (index < 0) {
            return;
        }
        self->NotifyActivity();
        SsidManager::GetInstance().SetDefaultSsid(index);
        lv_label_set_text(self->wifi_status_label_, "Varsayilan ag degistirildi");
        self->ShowSavedNetworks();
    }

    static void SavedNetworkDeleteCb(lv_event_t* e) {
        auto* self = Self(e);
        int index = RowIndex(e);
        if (index < 0) {
            return;
        }
        self->NotifyActivity();
        SsidManager::GetInstance().RemoveSsid(index);
        lv_label_set_text(self->wifi_status_label_, "Ag silindi");
        self->ShowSavedNetworks();
    }

    static void ConfirmEventCb(lv_event_t* e) {
        auto* confirm = static_cast<ConfirmButton*>(lv_event_get_user_data(e));
        confirm->owner->OnConfirmPress(*confirm);
    }

    static void ConfirmTimeoutCb(lv_timer_t* timer) {
        auto* confirm = static_cast<ConfirmButton*>(lv_timer_get_user_data(timer));
        // Tek atimlik timer kendini siliyor; elimizdeki isaretciyi once dusurelim.
        confirm->timer = nullptr;
        confirm->owner->ResetConfirm(*confirm);
    }

    static void FaceTimerCb(lv_timer_t* timer) {
        auto* self = static_cast<SettingsPanelDisplay*>(lv_timer_get_user_data(timer));
        self->eyes_.Tick();
        self->AlarmTick();
    }

    static void InfoTimerCb(lv_timer_t* timer) {
        auto* self = static_cast<SettingsPanelDisplay*>(lv_timer_get_user_data(timer));
        if (self->current_view_ == View::kInfo) {
            self->RefreshInfo();
        } else if (self->current_view_ == View::kWifi) {
            self->RefreshWifiStatus();
        } else if (self->current_view_ == View::kSd) {
            self->RefreshSd();  // yukleme sirasinda dosya sayisi/bos alan degisir
        }
    }
};
