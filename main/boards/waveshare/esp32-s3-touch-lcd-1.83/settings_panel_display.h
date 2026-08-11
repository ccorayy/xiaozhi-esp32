#pragma once

// ---------------------------------------------------------------------------
// Bu board'a ozel LVGL ayar paneli.
//
// Upstream'in kendi kalibi: SpiLcdDisplay'i alt siniflayip SetupUI()'yi
// override etmek (bkz. diger waveshare board'lari). Tum panel kodu bu
// header'da duruyor ki upstream rebase'lerinde board .cc dosyasindaki diff
// iki satirda kalsin.
//
// Panel lv_layer_top() uzerine kuruluyor; upstream'in SetupUI()/SetTheme()
// fonksiyonlari sadece lv_screen_active() cocuklariyla ugrastigi icin
// birbirimizin ayagina basmiyoruz.
//
// Acma: asagi kaydirma (LV_EVENT_GESTURE) veya ekranin ust seridine dokunma.
// Kapama: yukari kaydirma veya "Kapat" butonu.
// ---------------------------------------------------------------------------

#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "lcd_display.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <lvgl.h>

#include <functional>
#include <initializer_list>

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
        CreateSettingsPanel();
    }

    virtual void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);

        DisplayLockGuard lock(this);
        ApplySafeAreaInsets();
        StylePanel();
    }

private:
    // Ekranin fiziksel yuvarlak koseleri ust bardaki wifi/pil ikonlarini kirpiyordu.
    // Upstream degeri spacing(4)=8 px yaniydi; bu panelde kose yaricapini asacak
    // kadar ic bosluk birakiyoruz.
    static constexpr int kSafeInsetX = 20;
    static constexpr int kSafeInsetTop = 10;

    static constexpr int kOpenStripHeight = 28;
    static constexpr int kWifiConfirmTimeoutMs = 5000;
    static constexpr int kMinBrightness = 5;  // 0 = ekran tamamen kapanir, kilitlenmeyelim

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* open_strip_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* volume_caption_ = nullptr;
    lv_obj_t* volume_value_label_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* brightness_caption_ = nullptr;
    lv_obj_t* brightness_value_label_ = nullptr;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* theme_caption_ = nullptr;
    lv_obj_t* theme_switch_ = nullptr;
    lv_obj_t* wifi_button_ = nullptr;
    lv_obj_t* wifi_button_label_ = nullptr;
    lv_obj_t* close_button_ = nullptr;
    lv_obj_t* close_button_label_ = nullptr;

    lv_timer_t* wifi_confirm_timer_ = nullptr;
    bool wifi_confirm_pending_ = false;

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
    // Panel kurulumu
    // ------------------------------------------------------------------
    void CreateSettingsPanel() {
        if (panel_ != nullptr) {
            return;  // SetupUI iki kez cagrilirsa paneli tekrar kurmayalim
        }

        // Kaydirma hareketi parmagin altindaki nesneye gider; ekranin tamamini
        // kaplayan container_ ve emoji_box_ tiklanabilir oldugu icin olayi onlar
        // yakalar. EVENT_BUBBLE ile ekrana kadar cikarip tek yerde ele aliyoruz.
        lv_obj_t* screen = lv_screen_active();
        lv_obj_add_event_cb(screen, GestureEventCb, LV_EVENT_GESTURE, this);
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

        // Yedek acma yolu: ust kenarda gorunmez dokunma seridi.
        open_strip_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(open_strip_);
        lv_obj_set_size(open_strip_, width_, kOpenStripHeight);
        lv_obj_set_pos(open_strip_, 0, 0);
        lv_obj_add_flag(open_strip_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(open_strip_, OpenEventCb, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(open_strip_, GestureEventCb, LV_EVENT_GESTURE, this);

        panel_ = lv_obj_create(lv_layer_top());
        lv_obj_set_size(panel_, width_, height_);
        lv_obj_set_pos(panel_, 0, 0);
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_style_border_width(panel_, 0, 0);
        lv_obj_set_style_pad_left(panel_, kSafeInsetX, 0);
        lv_obj_set_style_pad_right(panel_, kSafeInsetX, 0);
        lv_obj_set_style_pad_top(panel_, kSafeInsetTop, 0);
        lv_obj_set_style_pad_bottom(panel_, 14, 0);
        lv_obj_set_style_pad_row(panel_, 8, 0);
        lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(panel_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(panel_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(panel_, GestureEventCb, LV_EVENT_GESTURE, this);

        title_label_ = lv_label_create(panel_);
        lv_label_set_text(title_label_, "Ayarlar");

        // Ses
        lv_obj_t* volume_row = CreateRow(panel_);
        volume_caption_ = lv_label_create(volume_row);
        lv_label_set_text(volume_caption_, "Ses");
        volume_value_label_ = lv_label_create(volume_row);
        lv_label_set_text(volume_value_label_, "0");

        volume_slider_ = lv_slider_create(panel_);
        lv_obj_set_width(volume_slider_, lv_pct(100));
        lv_obj_set_height(volume_slider_, 10);
        lv_slider_set_range(volume_slider_, 0, 100);
        lv_obj_add_event_cb(volume_slider_, VolumeEventCb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(volume_slider_, VolumeEventCb, LV_EVENT_RELEASED, this);

        // Parlaklik
        lv_obj_t* brightness_row = CreateRow(panel_);
        brightness_caption_ = lv_label_create(brightness_row);
        lv_label_set_text(brightness_caption_, "Parlaklik");
        brightness_value_label_ = lv_label_create(brightness_row);
        lv_label_set_text(brightness_value_label_, "0");

        brightness_slider_ = lv_slider_create(panel_);
        lv_obj_set_width(brightness_slider_, lv_pct(100));
        lv_obj_set_height(brightness_slider_, 10);
        lv_slider_set_range(brightness_slider_, kMinBrightness, 100);
        lv_obj_add_event_cb(brightness_slider_, BrightnessEventCb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(brightness_slider_, BrightnessEventCb, LV_EVENT_RELEASED, this);

        // Tema
        lv_obj_t* theme_row = CreateRow(panel_);
        theme_caption_ = lv_label_create(theme_row);
        lv_label_set_text(theme_caption_, "Koyu tema");
        theme_switch_ = lv_switch_create(theme_row);
        lv_obj_add_event_cb(theme_switch_, ThemeEventCb, LV_EVENT_VALUE_CHANGED, this);

        // WiFi ayar modu (iki asamali onay)
        wifi_button_ = lv_button_create(panel_);
        lv_obj_set_width(wifi_button_, lv_pct(100));
        lv_obj_set_height(wifi_button_, 38);
        wifi_button_label_ = lv_label_create(wifi_button_);
        lv_label_set_text(wifi_button_label_, "WiFi Ayari");
        lv_obj_center(wifi_button_label_);
        lv_obj_add_event_cb(wifi_button_, WifiEventCb, LV_EVENT_CLICKED, this);

        close_button_ = lv_button_create(panel_);
        lv_obj_set_width(close_button_, lv_pct(100));
        lv_obj_set_height(close_button_, 34);
        close_button_label_ = lv_label_create(close_button_);
        lv_label_set_text(close_button_label_, "Kapat");
        lv_obj_center(close_button_label_);
        lv_obj_add_event_cb(close_button_, CloseEventCb, LV_EVENT_CLICKED, this);

        StylePanel();
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
        return row;
    }

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

        for (lv_obj_t* obj : {title_label_, volume_caption_, volume_value_label_,
                              brightness_caption_, brightness_value_label_, theme_caption_}) {
            if (obj != nullptr) {
                lv_obj_set_style_text_color(obj, theme->text_color(), 0);
            }
        }

        for (lv_obj_t* slider : {volume_slider_, brightness_slider_}) {
            lv_obj_set_style_bg_color(slider, theme->chat_background_color(), LV_PART_MAIN);
            lv_obj_set_style_bg_color(slider, theme->text_color(), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(slider, theme->text_color(), LV_PART_KNOB);
        }

        lv_obj_set_style_bg_color(theme_switch_, theme->chat_background_color(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(theme_switch_, theme->text_color(),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);

        StyleButton(close_button_, close_button_label_, theme->chat_background_color(),
                    theme->text_color());
        RefreshWifiButtonStyle();
    }

    void StyleButton(lv_obj_t* button, lv_obj_t* label, lv_color_t bg, lv_color_t text) {
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(button, bg, 0);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_text_color(label, text, 0);
    }

    void RefreshWifiButtonStyle() {
        if (wifi_button_ == nullptr || current_theme_ == nullptr) {
            return;
        }
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        if (wifi_confirm_pending_) {
            // Onay rengi temadan bagimsiz sabit: light temada low_battery_color siyah.
            StyleButton(wifi_button_, wifi_button_label_, lv_color_hex(0xC62828),
                        lv_color_hex(0xFFFFFF));
        } else {
            StyleButton(wifi_button_, wifi_button_label_, theme->chat_background_color(),
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
        lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(open_strip_, LV_OBJ_FLAG_HIDDEN);
        NotifyActivity();
    }

    void ClosePanel() {
        if (panel_ == nullptr || lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }
        ResetWifiConfirm();
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(open_strip_, LV_OBJ_FLAG_HIDDEN);
        NotifyActivity();
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

    void OnWifiEvent() {
        NotifyActivity();
        if (!wifi_confirm_pending_) {
            wifi_confirm_pending_ = true;
            lv_label_set_text(wifi_button_label_, "Emin misin? Tekrar dokun");
            RefreshWifiButtonStyle();
            if (wifi_confirm_timer_ == nullptr) {
                wifi_confirm_timer_ = lv_timer_create(WifiConfirmTimeoutCb,
                                                      kWifiConfirmTimeoutMs, this);
                lv_timer_set_repeat_count(wifi_confirm_timer_, 1);
            }
            return;
        }

        ResetWifiConfirm();
        ClosePanel();
        if (on_wifi_config_) {
            auto callback = on_wifi_config_;
            Application::GetInstance().Schedule([callback]() { callback(); });
        }
    }

    void ResetWifiConfirm() {
        if (wifi_confirm_timer_ != nullptr) {
            lv_timer_delete(wifi_confirm_timer_);
            wifi_confirm_timer_ = nullptr;
        }
        if (!wifi_confirm_pending_) {
            return;
        }
        wifi_confirm_pending_ = false;
        lv_label_set_text(wifi_button_label_, "WiFi Ayari");
        RefreshWifiButtonStyle();
    }

    void OnGesture() {
        lv_indev_t* indev = lv_indev_active();
        if (indev == nullptr) {
            return;
        }
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_BOTTOM) {
            OpenPanel();
        } else if (dir == LV_DIR_TOP) {
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
    static void OpenEventCb(lv_event_t* e) { Self(e)->OpenPanel(); }
    static void CloseEventCb(lv_event_t* e) { Self(e)->ClosePanel(); }
    static void VolumeEventCb(lv_event_t* e) { Self(e)->OnVolumeEvent(e); }
    static void BrightnessEventCb(lv_event_t* e) { Self(e)->OnBrightnessEvent(e); }
    static void ThemeEventCb(lv_event_t* e) { Self(e)->OnThemeEvent(); }
    static void WifiEventCb(lv_event_t* e) { Self(e)->OnWifiEvent(); }

    static void WifiConfirmTimeoutCb(lv_timer_t* timer) {
        auto* self = static_cast<SettingsPanelDisplay*>(lv_timer_get_user_data(timer));
        // Tek atimlik timer kendini siliyor; elimizdeki isaretciyi once dusurelim.
        self->wifi_confirm_timer_ = nullptr;
        self->ResetWifiConfirm();
    }
};
