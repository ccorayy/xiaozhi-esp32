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
//   ekrana dokunma      -> sohbeti baslat/bitir (boot butonuyla ayni is)
//   asagi kaydirma      -> panel acilir  (ust seride dokunmak da acar)
//   yukari kaydirma     -> panel kapanir
//   saga/sola kaydirma  -> sayfalar: Ayarlar / Bilgi / Kisayollar
// ---------------------------------------------------------------------------

#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "lcd_display.h"
#include "lvgl_theme.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <wifi_manager.h>

#include <functional>
#include <initializer_list>
#include <vector>

class SettingsPanelDisplay : public SpiLcdDisplay {
public:
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

    virtual void SetupUI() override {
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        ApplySafeAreaInsets();
        CreatePanel();
    }

    virtual void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);

        DisplayLockGuard lock(this);
        ApplySafeAreaInsets();
        StylePanel();
    }

private:
    // Ekranin fiziksel yuvarlak koseleri ust bardaki wifi/pil ikonlarini kirpiyordu.
    // Upstream degeri spacing(4)=8 px yaniydi; kose yaricapini asacak kadar bosluk.
    static constexpr int kSafeInsetX = 20;
    static constexpr int kSafeInsetTop = 10;

    static constexpr int kOpenStripHeight = 28;
    static constexpr int kConfirmTimeoutMs = 5000;
    static constexpr int kInfoRefreshMs = 2000;
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

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* pager_ = nullptr;
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
    lv_obj_t* info_version_ = nullptr;
    lv_obj_t* info_uptime_ = nullptr;
    lv_timer_t* info_timer_ = nullptr;

    // Sayfa 3 - Kisayollar
    lv_obj_t* chat_button_ = nullptr;
    lv_obj_t* chat_button_label_ = nullptr;
    ConfirmButton wifi_confirm_;
    ConfirmButton restart_confirm_;

    // Tema degisiminde yeniden renklendirilecek duz yazi etiketleri.
    std::vector<lv_obj_t*> plain_labels_;

    // Kaydirma hareketi de LV_EVENT_CLICKED uretebiliyor; paneli acan kaydirmanin
    // ayni zamanda sohbeti baslatmasini engellemek icin.
    bool gesture_handled_ = false;

    std::function<void()> on_wifi_config_;
    std::function<void()> on_activity_;

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
    // Kurulum
    // ------------------------------------------------------------------
    void CreatePanel() {
        if (panel_ != nullptr) {
            return;  // SetupUI iki kez cagrilirsa paneli tekrar kurmayalim
        }

        HookScreenEvents();
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

        // lv_tileview upstream'de kapali (CONFIG_LV_USE_TILEVIEW=n, flash tasarrufu).
        // Paylasilan sdkconfig'i degistirmek yerine sayfalamayi yatay scroll snap ile
        // kendimiz kuruyoruz - sadece temel nesne ozellikleri, ek bagimlilik yok.
        pager_ = lv_obj_create(panel_);
        lv_obj_set_size(pager_, width_, height_);
        lv_obj_set_pos(pager_, 0, 0);
        lv_obj_set_style_bg_opa(pager_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pager_, 0, 0);
        lv_obj_set_style_pad_all(pager_, 0, 0);
        lv_obj_set_style_pad_column(pager_, 0, 0);
        lv_obj_set_flex_flow(pager_, LV_FLEX_FLOW_ROW);
        lv_obj_set_scroll_dir(pager_, LV_DIR_HOR);
        lv_obj_set_scroll_snap_x(pager_, LV_SCROLL_SNAP_CENTER);
        lv_obj_add_flag(pager_, LV_OBJ_FLAG_SCROLL_ONE);  // tek hamlede tek sayfa
        lv_obj_set_scrollbar_mode(pager_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(pager_, LV_OBJ_FLAG_EVENT_BUBBLE);

        BuildSettingsTile(CreatePage());
        BuildInfoTile(CreatePage());
        BuildActionsTile(CreatePage());

        info_timer_ = lv_timer_create(InfoTimerCb, kInfoRefreshMs, this);

        StylePanel();
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

    lv_obj_t* CreatePage() {
        lv_obj_t* page = lv_obj_create(pager_);
        lv_obj_set_size(page, width_, height_);
        lv_obj_set_style_radius(page, 0, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(page, kSafeInsetX, 0);
        lv_obj_set_style_pad_right(page, kSafeInsetX, 0);
        lv_obj_set_style_pad_top(page, kSafeInsetTop, 0);
        lv_obj_set_style_pad_bottom(page, 14, 0);
        lv_obj_set_style_pad_row(page, 8, 0);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        // Sayfa kendi icinde kaymasin; yatay kaydirmayi pager_ yonetiyor, dikey
        // hareket de LV_EVENT_GESTURE olarak panele ulassin diye.
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE);
        return page;
    }

    // ------------------------------------------------------------------
    // Sayfa 1 - Ayarlar
    // ------------------------------------------------------------------
    void BuildSettingsTile(lv_obj_t* tile) {
        CreateHeader(tile, "Ayarlar  1/3");

        lv_obj_t* volume_row = CreateRow(tile);
        CreateLabel(volume_row, "Ses");
        volume_value_label_ = CreateLabel(volume_row, "0");

        volume_slider_ = lv_slider_create(tile);
        lv_obj_set_width(volume_slider_, lv_pct(100));
        lv_obj_set_height(volume_slider_, 10);
        lv_slider_set_range(volume_slider_, 0, 100);
        lv_obj_add_event_cb(volume_slider_, VolumeEventCb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(volume_slider_, VolumeEventCb, LV_EVENT_RELEASED, this);

        lv_obj_t* brightness_row = CreateRow(tile);
        CreateLabel(brightness_row, "Parlaklik");
        brightness_value_label_ = CreateLabel(brightness_row, "0");

        brightness_slider_ = lv_slider_create(tile);
        lv_obj_set_width(brightness_slider_, lv_pct(100));
        lv_obj_set_height(brightness_slider_, 10);
        lv_slider_set_range(brightness_slider_, kMinBrightness, 100);
        lv_obj_add_event_cb(brightness_slider_, BrightnessEventCb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(brightness_slider_, BrightnessEventCb, LV_EVENT_RELEASED, this);

        lv_obj_t* theme_row = CreateRow(tile);
        CreateLabel(theme_row, "Koyu tema");
        theme_switch_ = lv_switch_create(theme_row);
        lv_obj_add_event_cb(theme_switch_, ThemeEventCb, LV_EVENT_VALUE_CHANGED, this);

        close_button_ = CreateButton(tile, "Kapat", &close_button_label_);
        lv_obj_add_event_cb(close_button_, CloseEventCb, LV_EVENT_CLICKED, this);
    }

    // ------------------------------------------------------------------
    // Sayfa 2 - Bilgi
    // ------------------------------------------------------------------
    void BuildInfoTile(lv_obj_t* tile) {
        CreateHeader(tile, "Bilgi  2/3");
        info_battery_ = CreateInfoRow(tile, "Pil");
        info_wifi_ = CreateInfoRow(tile, "WiFi");
        info_ip_ = CreateInfoRow(tile, "IP");
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
        CreateHeader(tile, "Kisayollar  3/3");

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
    // Widget yardimcilari
    // ------------------------------------------------------------------
    void CreateHeader(lv_obj_t* tile, const char* text) { CreateLabel(tile, text); }

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

        for (lv_obj_t* slider : {volume_slider_, brightness_slider_}) {
            lv_obj_set_style_bg_color(slider, theme->chat_background_color(), LV_PART_MAIN);
            lv_obj_set_style_bg_color(slider, theme->text_color(), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(slider, theme->text_color(), LV_PART_KNOB);
        }

        lv_obj_set_style_bg_color(theme_switch_, theme->chat_background_color(), LV_PART_MAIN);
        // -Werror=deprecated-enum-enum-conversion: lv_part_t ile lv_state_t dogrudan
        // OR'lanamiyor, secici tipine cevirmek gerekiyor.
        lv_style_selector_t checked_indicator = static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                                static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(theme_switch_, theme->text_color(), checked_indicator);

        StyleButton(close_button_, close_button_label_, theme->chat_background_color(),
                    theme->text_color());
        StyleButton(chat_button_, chat_button_label_, theme->chat_background_color(),
                    theme->text_color());
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
    void OpenPanel() {
        if (panel_ == nullptr || !lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }
        SyncControlsFromDevice();
        RefreshInfo();
        lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(open_strip_, LV_OBJ_FLAG_HIDDEN);
        NotifyActivity();
    }

    void ClosePanel() {
        if (panel_ == nullptr || lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }
        ResetConfirm(wifi_confirm_);
        ResetConfirm(restart_confirm_);
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(open_strip_, LV_OBJ_FLAG_HIDDEN);
        NotifyActivity();
    }

    bool IsPanelOpen() const {
        return panel_ != nullptr && !lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN);
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
        ClosePanel();
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
        ClosePanel();
        Application::GetInstance().ToggleChatState();
    }

    // Ekrana dokunmak sohbeti baslatir/bitirir. Turkce wake word mumkun olmadigi
    // icin (ESP-SR sadece Ingilizce/Mandarin) asil kullanim yolu bu.
    void OnScreenClicked() {
        if (IsPanelOpen() || gesture_handled_) {
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
            gesture_handled_ = true;
            OpenPanel();
        } else if (dir == LV_DIR_TOP) {
            gesture_handled_ = true;
            ClosePanel();
        }
    }

    // ------------------------------------------------------------------
    // Statik LVGL koprulleri
    // ------------------------------------------------------------------
    static SettingsPanelDisplay* Self(lv_event_t* e) {
        return static_cast<SettingsPanelDisplay*>(lv_event_get_user_data(e));
    }

    static void GestureEventCb(lv_event_t* e) { Self(e)->OnGesture(); }
    static void PressedEventCb(lv_event_t* e) { Self(e)->gesture_handled_ = false; }
    static void ScreenClickedEventCb(lv_event_t* e) { Self(e)->OnScreenClicked(); }
    static void OpenEventCb(lv_event_t* e) { Self(e)->OpenPanel(); }
    static void CloseEventCb(lv_event_t* e) { Self(e)->ClosePanel(); }
    static void VolumeEventCb(lv_event_t* e) { Self(e)->OnVolumeEvent(e); }
    static void BrightnessEventCb(lv_event_t* e) { Self(e)->OnBrightnessEvent(e); }
    static void ThemeEventCb(lv_event_t* e) { Self(e)->OnThemeEvent(); }
    static void ChatEventCb(lv_event_t* e) { Self(e)->OnChatButton(); }

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

    static void InfoTimerCb(lv_timer_t* timer) {
        auto* self = static_cast<SettingsPanelDisplay*>(lv_timer_get_user_data(timer));
        if (self->IsPanelOpen()) {
            self->RefreshInfo();
        }
    }
};
