#pragma once

// ---------------------------------------------------------------------------
// Emoji yerine animasyonlu gozler + bosta saat ekrani.
//
// Neden elle cizildi: upstream'de hazir bir goz motoru var (EmoteDisplay,
// esp_emote_gfx) ama o LcdDisplay'in yerine geciyor ve emote modunda LVGL hic
// derlenmiyor (bkz. display.h). Yani onu secmek panel, WiFi sayfasi, klavye,
// bilgi ekrani - hepsini silmek demekti. Gozler LVGL'de iki yuvarlatilmis
// nesne ve birkac animasyonla cizilebildigi icin kendimiz yaptik.
//
// Her goz bir lv_obj; ifadeler gozun boyutu, yuvarlakligi, kaymasi ve uzerine
// binen "kapak" dikdortgenleriyle olusuyor. Kapaklar arka plan rengiyle boyanip
// gozun bir kismini ortuyor: alttan ortmek "^" (mutlu), ustten ortmek "v"
// (uzgun), egimli ustten ortmek kizgin/supheli goruntusu veriyor.
// ---------------------------------------------------------------------------

#include "lvgl_theme.h"

#include <esp_timer.h>
#include <lvgl.h>

#include <cstdio>
#include <cstring>
#include <ctime>

// Yazi tipi bileseni src/*.c dosyalarinin hepsini derliyor, yani 30 piksellik
// font zaten binary'de. Saat icin onu kullaniyoruz (varsayilan metin 16 px).
LV_FONT_DECLARE(font_noto_sans_basic_30_4);

class EyesFace {
public:
    // Taban olculer (240x284 ekran icin)
    static constexpr int kEyeW = 56;
    static constexpr int kEyeH = 72;
    static constexpr int kEyeGap = 100;  // iki goz merkezi arasi
    static constexpr int kEyeY = -6;     // merkeze gore dikey kayma
    static constexpr int kIdleSeconds = 15;

    void Create(lv_obj_t* parent, LvglTheme* theme) {
        theme_ = theme;

        root_ = lv_obj_create(parent);
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
        lv_obj_center(root_);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_EVENT_BUBBLE);

        eyes_ = lv_obj_create(root_);
        lv_obj_remove_style_all(eyes_);
        lv_obj_set_size(eyes_, LV_PCT(100), LV_PCT(100));
        lv_obj_center(eyes_);
        lv_obj_remove_flag(eyes_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(eyes_, LV_OBJ_FLAG_EVENT_BUBBLE);

        CreateEye(left_, -kEyeGap / 2);
        CreateEye(right_, kEyeGap / 2);

        CreateClock();
        ApplyTheme(theme);
        SetExpression("neutral");
        NotifyActivity();
    }

    void ApplyTheme(LvglTheme* theme) {
        theme_ = theme;
        if (root_ == nullptr) {
            return;
        }
        RefreshColors();
        lv_obj_set_style_text_color(clock_time_, theme->text_color(), 0);
        lv_obj_set_style_text_color(clock_date_, theme->text_color(), 0);
    }

    void SetExpression(const char* emotion) {
        const Expression* e = Find(emotion);
        if (e == nullptr) {
            return;
        }
        current_ = e;
        Apply(left_, e->left);
        Apply(right_, e->right);
        RefreshColors();
        lv_obj_add_flag(blush_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blush_r_, LV_OBJ_FLAG_HIDDEN);
        if (e->blush) {
            lv_obj_remove_flag(blush_l_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(blush_r_, LV_OBJ_FLAG_HIDDEN);
        }
        if (e->tear) {
            lv_obj_remove_flag(tear_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tear_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void NotifyActivity() {
        last_activity_us_ = esp_timer_get_time();
        if (clock_visible_) {
            ShowClock(false);
        }
    }

    // Panel acikken yuzu tamamen gizle
    void SetHidden(bool hidden) {
        if (root_ == nullptr) {
            return;
        }
        if (hidden) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Saniyede bir cagrilir
    void Tick() {
        if (root_ == nullptr) {
            return;
        }
        int64_t idle_s = (esp_timer_get_time() - last_activity_us_) / 1000000;

        if (!clock_visible_ && idle_s >= kIdleSeconds) {
            ShowClock(true);
        }
        if (clock_visible_) {
            UpdateClock();
            return;
        }

        // Bosta ara sira goz kirp
        if (current_ != nullptr && current_->blink && ++tick_count_ >= next_blink_) {
            tick_count_ = 0;
            next_blink_ = 3 + (esp_timer_get_time() / 1000000) % 5;  // 3-7 sn
            Blink();
        }
    }

private:
    struct EyeShape {
        int w = 100;         // taban genisligin yuzdesi
        int h = 100;         // taban yuksekligin yuzdesi
        int radius = 50;     // yuvarlaklik yuzdesi (50 = tam oval)
        int dx = 0;          // yatay kayma (px)
        int dy = 0;          // dikey kayma (px)
        int lid_top = 0;     // ustten ortulen yuzde
        int lid_bottom = 0;  // alttan ortulen yuzde
        int lid_angle = 0;   // ust kapak egimi (derece)
    };

    struct Expression {
        const char* name;
        EyeShape left;
        EyeShape right;
        bool blink = false;
        bool blush = false;
        bool tear = false;
        uint32_t color = 0;  // 0 = tema metin rengi
    };

    struct Eye {
        lv_obj_t* obj = nullptr;
        lv_obj_t* lid_top = nullptr;
        lv_obj_t* lid_bottom = nullptr;
        int base_x = 0;
        int height = kEyeH;  // kirpma animasyonu icin
    };

    static constexpr uint32_t kPink = 0xFF5C8A;

    // 21 ifade - isimler firmware'in gonderdikleriyle birebir ayni olmali
    // (noto_emoji.c icindeki tablo: neutral, happy, laughing, ...)
    static const Expression* Table(size_t& count) {
        static const Expression table[] = {
            {"neutral",     {}, {}, true},
            {"happy",       {100, 90, 50, 0, 0, 0, 55}, {100, 90, 50, 0, 0, 0, 55}},
            {"laughing",    {110, 85, 50, 0, -4, 0, 65}, {110, 85, 50, 0, -4, 0, 65}},
            {"funny",       {100, 90, 50, 0, 0, 0, 60}, {95, 55, 50, 0, 2}},
            {"sad",         {100, 85, 50, 0, 8, 30, 0, -14}, {100, 85, 50, 0, 8, 30, 0, 14}},
            {"angry",       {105, 95, 40, 0, 0, 45, 0, 22}, {105, 95, 40, 0, 0, 45, 0, -22}},
            {"crying",      {100, 85, 50, 0, 10, 25, 0, -10}, {100, 85, 50, 0, 10, 25, 0, 10},
                            false, false, true},
            {"loving",      {95, 85, 50, 0, 0, 0, 45}, {95, 85, 50, 0, 0, 0, 45},
                            false, true, false, kPink},
            {"embarrassed", {100, 45, 50, 0, 4}, {100, 45, 50, 0, 4}, false, true},
            {"surprised",   {115, 125, 50}, {115, 125, 50}},
            {"shocked",     {125, 140, 50, 0, -4}, {125, 140, 50, 0, -4}},
            {"thinking",    {100, 95, 50, 12, -10, 20}, {100, 95, 50, 12, -10, 30}, true},
            {"winking",     {100, 10, 50, 0, 6}, {100, 100, 50}},
            {"cool",        {105, 60, 30, 0, 2, 40}, {105, 60, 30, 0, 2, 40}, true},
            {"relaxed",     {100, 70, 50, 0, 2, 35, 15}, {100, 70, 50, 0, 2, 35, 15}, true},
            {"delicious",   {100, 85, 50, 0, 2, 0, 60}, {100, 85, 50, 0, 2, 0, 60}, false, true},
            {"kissy",       {100, 12, 50, 0, 6}, {85, 80, 50, 0, 0}, false, true},
            {"confident",   {100, 85, 45, -6, -2, 28}, {100, 85, 45, 6, -2, 28}, true},
            {"sleepy",      {100, 18, 50, 0, 10}, {100, 18, 50, 0, 10}},
            {"silly",       {110, 110, 50, -8, -4}, {90, 70, 50, 10, 6}, true},
            {"confused",    {115, 115, 50, -4, -6}, {90, 70, 50, 6, 6, 20}, true},
        };
        count = sizeof(table) / sizeof(table[0]);
        return table;
    }

    const Expression* Find(const char* name) {
        size_t count = 0;
        const Expression* table = Table(count);
        for (size_t i = 0; i < count; i++) {
            if (strcmp(table[i].name, name) == 0) {
                return &table[i];
            }
        }
        return &table[0];  // taninmayan ifade -> neutral
    }

    void CreateEye(Eye& eye, int base_x) {
        eye.base_x = base_x;

        eye.obj = lv_obj_create(eyes_);
        lv_obj_remove_style_all(eye.obj);
        lv_obj_set_size(eye.obj, kEyeW, kEyeH);
        lv_obj_align(eye.obj, LV_ALIGN_CENTER, base_x, kEyeY);
        lv_obj_set_style_bg_opa(eye.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye.obj, kEyeW / 2, 0);
        lv_obj_set_style_clip_corner(eye.obj, true, 0);
        lv_obj_remove_flag(eye.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(eye.obj, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Kapaklar gozun cocugu; arka plan rengiyle boyanip gozu ortuyorlar.
        // Genislik %200 cunku egildiklerinde kenarlarda bosluk kalmasin.
        eye.lid_top = lv_obj_create(eye.obj);
        lv_obj_remove_style_all(eye.lid_top);
        lv_obj_set_width(eye.lid_top, kEyeW * 2);
        lv_obj_set_height(eye.lid_top, 0);
        lv_obj_align(eye.lid_top, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_opa(eye.lid_top, LV_OPA_COVER, 0);

        eye.lid_bottom = lv_obj_create(eye.obj);
        lv_obj_remove_style_all(eye.lid_bottom);
        lv_obj_set_width(eye.lid_bottom, kEyeW * 2);
        lv_obj_set_height(eye.lid_bottom, 0);
        lv_obj_align(eye.lid_bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(eye.lid_bottom, LV_OPA_COVER, 0);

        // Yanak (utanma/ask) ve gozyasi
        lv_obj_t*& blush = (base_x < 0) ? blush_l_ : blush_r_;
        blush = lv_obj_create(eyes_);
        lv_obj_remove_style_all(blush);
        lv_obj_set_size(blush, 26, 12);
        lv_obj_align(blush, LV_ALIGN_CENTER, base_x, kEyeY + kEyeH / 2 + 14);
        lv_obj_set_style_bg_opa(blush, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(blush, lv_color_hex(kPink), 0);
        lv_obj_set_style_radius(blush, 6, 0);
        lv_obj_add_flag(blush, LV_OBJ_FLAG_HIDDEN);

        if (base_x > 0 && tear_ == nullptr) {
            tear_ = lv_obj_create(eyes_);
            lv_obj_remove_style_all(tear_);
            lv_obj_set_size(tear_, 10, 16);
            lv_obj_align(tear_, LV_ALIGN_CENTER, base_x + 18, kEyeY + kEyeH / 2 + 10);
            lv_obj_set_style_bg_opa(tear_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(tear_, lv_color_hex(0x4FA8FF), 0);
            lv_obj_set_style_radius(tear_, 5, 0);
            lv_obj_add_flag(tear_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void Apply(Eye& eye, const EyeShape& s) {
        int w = kEyeW * s.w / 100;
        int h = kEyeH * s.h / 100;
        eye.height = h;

        lv_obj_set_size(eye.obj, w, h);
        lv_obj_align(eye.obj, LV_ALIGN_CENTER, eye.base_x + s.dx, kEyeY + s.dy);
        lv_obj_set_style_radius(eye.obj, w * s.radius / 100, 0);

        lv_obj_set_width(eye.lid_top, w * 2);
        lv_obj_set_height(eye.lid_top, h * s.lid_top / 100);
        lv_obj_set_width(eye.lid_bottom, w * 2);
        lv_obj_set_height(eye.lid_bottom, h * s.lid_bottom / 100);

        // LVGL aci birimi 0.1 derece
        lv_obj_set_style_transform_pivot_x(eye.lid_top, w, 0);
        lv_obj_set_style_transform_pivot_y(eye.lid_top, 0, 0);
        lv_obj_set_style_transform_rotation(eye.lid_top, s.lid_angle * 10, 0);
    }

    void RefreshColors() {
        if (theme_ == nullptr) {
            return;
        }
        lv_color_t eye_color = theme_->text_color();
        if (current_ != nullptr && current_->color != 0) {
            eye_color = lv_color_hex(current_->color);
        }
        lv_color_t bg = theme_->background_color();
        for (Eye* eye : {&left_, &right_}) {
            lv_obj_set_style_bg_color(eye->obj, eye_color, 0);
            lv_obj_set_style_bg_color(eye->lid_top, bg, 0);
            lv_obj_set_style_bg_color(eye->lid_bottom, bg, 0);
        }
    }

    static void BlinkExecCb(void* var, int32_t value) {
        lv_obj_set_height(static_cast<lv_obj_t*>(var), value);
    }

    void Blink() {
        for (Eye* eye : {&left_, &right_}) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, eye->obj);
            lv_anim_set_exec_cb(&a, BlinkExecCb);
            lv_anim_set_values(&a, eye->height, 4);
            lv_anim_set_duration(&a, 90);
            lv_anim_set_playback_duration(&a, 110);
            lv_anim_start(&a);
        }
    }

    void CreateClock() {
        clock_ = lv_obj_create(root_);
        lv_obj_remove_style_all(clock_);
        lv_obj_set_size(clock_, LV_PCT(100), LV_PCT(100));
        lv_obj_center(clock_);
        lv_obj_remove_flag(clock_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(clock_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(clock_, LV_OBJ_FLAG_HIDDEN);

        clock_time_ = lv_label_create(clock_);
        lv_label_set_text(clock_time_, "--:--");
        lv_obj_set_style_text_font(clock_time_, &font_noto_sans_basic_30_4, 0);
        // NOT: Burada transform_scale ile 2 kat buyutmustuk; cihazda saat HIC
        // gorunmedi (gozler gizleniyordu ama yerine bir sey cizilmiyordu).
        // LVGL olcekli nesneyi ayri bir katmana ciziyor ve katman olusmazsa
        // nesne tamamen kayboluyor. Olcekleme yok, 30 px font oldugu gibi.
        lv_obj_align(clock_time_, LV_ALIGN_CENTER, 0, -14);

        clock_date_ = lv_label_create(clock_);
        lv_label_set_text(clock_date_, "");
        lv_obj_align(clock_date_, LV_ALIGN_CENTER, 0, 24);
    }

    void ShowClock(bool on) {
        clock_visible_ = on;
        if (on) {
            UpdateClock();
            lv_obj_add_flag(eyes_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(clock_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(clock_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(eyes_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void UpdateClock() {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        if (t == nullptr || t->tm_year + 1900 < 2024) {
            // Saat sunucudan gelmemis; bos ekran yerine durumu goster
            lv_label_set_text(clock_time_, "--:--");
            lv_label_set_text(clock_date_, "saat alinamadi");
            return;
        }
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M", t);
        lv_label_set_text(clock_time_, buf);

        static const char* gunler[] = {"Pazar",    "Pazartesi", "Sali", "Carsamba",
                                       "Persembe", "Cuma",      "Cumartesi"};
        char date[48];
        snprintf(date, sizeof(date), "%d.%02d  %s", t->tm_mday, t->tm_mon + 1,
                 gunler[t->tm_wday % 7]);
        lv_label_set_text(clock_date_, date);
    }

    LvglTheme* theme_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* eyes_ = nullptr;
    lv_obj_t* clock_ = nullptr;
    lv_obj_t* clock_time_ = nullptr;
    lv_obj_t* clock_date_ = nullptr;
    lv_obj_t* blush_l_ = nullptr;
    lv_obj_t* blush_r_ = nullptr;
    lv_obj_t* tear_ = nullptr;
    Eye left_;
    Eye right_;
    const Expression* current_ = nullptr;
    int64_t last_activity_us_ = 0;
    bool clock_visible_ = false;
    int tick_count_ = 0;
    int next_blink_ = 4;
};
