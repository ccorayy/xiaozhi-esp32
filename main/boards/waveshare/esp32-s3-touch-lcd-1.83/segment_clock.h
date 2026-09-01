#pragma once

// ---------------------------------------------------------------------------
// Yedi-segment saat yuzu - VolosR/pocketClock gorunumune benzetildi.
//
// ⚠️ Ilk surumu yalnizca ui_Screen1.c'deki koordinatlardan kurmustum ve
// tasarimi yanlis anlamistim: "cerceve icinde buyuk saat" sandim. Cihaz
// fotografi gosterdi ki referans aslinda bilgi yogun bir gosterge paneli -
// kenarlikli mini kutular (ALM/CHR/BRIGHT), sag ustte segment tarih, buyuk
// gun adi, saatin sag ustunde ust simge saniye, altta kimlik seridi.
// Bu surum onu hedefliyor.
//
// Ekran farki: referans 368x448, bizimki 240x284 (%65). Referansin kucuk
// yazilari oransal olarak bizim en kucuk fontumuzdan (14 px) ince; ayni
// yogunluga ulasamiyoruz, o yuzden ogeler secilerek alindi.
//
// Rakamlar icin font yok, yedi dikdortgen ciziliyor: olcek serbest, binary
// buyumuyor, lisans derdi yok (referansin G7 fontu ticaride lisansli).
// Sonuk segmentler de koyu renkte duruyor - gercek LCD gorunumunu veren
// detay bu.
// ---------------------------------------------------------------------------

#include "seven_segment.h"

#include <lvgl.h>

#include <cstdio>
#include <ctime>
#include <string>

LV_FONT_DECLARE(font_noto_sans_basic_14_1);
LV_FONT_DECLARE(font_noto_sans_basic_20_4);

class SegmentClock {
public:
    void Create(lv_obj_t* parent, int width, int height) {
        root_ = lv_obj_create(parent);
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, width, height);
        lv_obj_center(root_);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(root_, lv_color_hex(kBg), 0);

        // Ekranin fiziksel yuvarlak koseleri kenari kirpiyor; icerik iceride.
        const int left = kMargin;
        const int right = width - kMargin;

        // --- ust serit: alarm gostergesi (referanstaki kirmizi E-SHOCK yeri)
        alarm_dot_ = MakeBlock(root_, left, 10, 8, 8, kRedDim);
        alarm_text_ = MakeLabel(root_, left + 14, 6, kSmall(), kFaint);
        lv_label_set_text(alarm_text_, "ALARM YOK");

        // --- sol sutun: etiketli mini kutular (referanstaki ALM / CHR)
        pil_tag_ = MakeTag(left, 30, "PIL", kGreen);
        battery_ = MakeLabel(root_, left + kTagW + 6, 32, kSmall(), kInk);
        MakeTag(left, 54, "SES", kAccent);
        volume_ = MakeLabel(root_, left + kTagW + 6, 56, kSmall(), kInk);

        // --- sag ust: segment tarih, referanstaki "19-03" gibi gun-ay
        const int dw = 11, dh = 19, dt = 3, dg = 3, dash = 7;
        int date_w = 4 * dw + 3 * dg + dash;
        int date_x = right - date_w;
        for (int i = 0; i < 4; i++) {
            int x = date_x + i * (dw + dg) + (i >= 2 ? dash : 0);
            date_digits_[i].Create(root_, x, 28, dw, dh, dt, kAccent, kCyanDim);
        }
        MakeBlock(root_, date_x + 2 * (dw + dg) + 1, 28 + dh / 2 - 1, 5, 3, kAccent);

        // --- gun adi: buyuk, saga yasli
        day_ = MakeLabel(root_, left, 54, kLarge(), kInk);
        lv_obj_set_width(day_, right - left);
        lv_obj_set_style_text_align(day_, LV_TEXT_ALIGN_RIGHT, 0);

        // Referanstaki mor ayirici cizgi
        MakeBlock(root_, right - 88, 80, 88, 2, kViolet);

        // --- buyuk saat + ust simge saniye (referansta saatin sag ustunde)
        int time_w = 4 * kDigitW + 3 * kDigitGap + kColonW;
        int sec_w = 2 * kSecW + kSecGap;
        int x = (width - (time_w + kSecPad + sec_w)) / 2;
        for (int i = 0; i < 4; i++) {
            if (i == 2) {
                MakeColon(x, kTimeY, kColonW, kDigitH, kDigitT);
                x += kColonW + kDigitGap;
            }
            digits_[i].Create(root_, x, kTimeY, kDigitW, kDigitH, kDigitT);
            x += kDigitW + kDigitGap;
        }
        x += kSecPad - kDigitGap;
        for (int i = 0; i < 2; i++) {
            seconds_[i].Create(root_, x + i * (kSecW + kSecGap), kTimeY, kSecW, kSecH, kSecT,
                               kAmber, kAmberDim);
        }

        // --- saatin altinda: sicaklik solda, tam tarih sagda
        weather_ = MakeLabel(root_, left, kTimeY + kDigitH + 12, kLarge(), kAccent);
        date_text_ = MakeLabel(root_, left, kTimeY + kDigitH + 18, kSmall(), kMuted);
        lv_obj_set_width(date_text_, right - left);
        lv_obj_set_style_text_align(date_text_, LV_TEXT_ALIGN_RIGHT, 0);

        // --- en altta kimlik seridi ("AMOLED 1.8" ESP32 S3 TOUCH" karsiligi)
        lv_obj_t* strip = MakeLabel(root_, 0, height - 28, kSmall(), kFaint);
        lv_obj_set_width(strip, width);
        lv_obj_set_style_text_align(strip, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(strip, "AGON   1.83\" ESP32-S3   TOUCH");
    }

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

    // Board metni "24<derece>  Aciklama" veriyor; satira yalnizca sicaklik
    // sigiyor, aciklama tarihle cakisiyordu.
    void SetWeather(const std::string& text) {
        if (weather_ == nullptr) {
            return;
        }
        auto cut = text.find("  ");
        lv_label_set_text(weather_,
                          cut == std::string::npos ? text.c_str() : text.substr(0, cut).c_str());
    }

    void SetBattery(int level, bool charging) {
        if (battery_ == nullptr) {
            return;
        }
        lv_label_set_text_fmt(battery_, "%d%%%s", level, charging ? " +" : "");
        uint32_t color = charging ? kAccent : (level <= 20 ? kRed : (level <= 50 ? kYellow : kGreen));
        lv_obj_set_style_text_color(battery_, lv_color_hex(color), 0);
        if (pil_tag_ != nullptr) {
            lv_obj_set_style_border_color(pil_tag_, lv_color_hex(color), 0);
            lv_obj_t* label = lv_obj_get_child(pil_tag_, 0);
            if (label != nullptr) {
                lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
            }
        }
    }

    void SetVolume(int volume) {
        if (volume_ != nullptr) {
            lv_label_set_text_fmt(volume_, "%d", volume);
        }
    }

    void SetAlarm(bool enabled, int hour, int minute) {
        if (alarm_text_ == nullptr) {
            return;
        }
        if (enabled) {
            lv_label_set_text_fmt(alarm_text_, "ALARM %02d:%02d", hour, minute);
            lv_obj_set_style_text_color(alarm_text_, lv_color_hex(kInk), 0);
            lv_obj_set_style_bg_color(alarm_dot_, lv_color_hex(kRed), 0);
        } else {
            lv_label_set_text(alarm_text_, "ALARM YOK");
            lv_obj_set_style_text_color(alarm_text_, lv_color_hex(kFaint), 0);
            lv_obj_set_style_bg_color(alarm_dot_, lv_color_hex(kRedDim), 0);
        }
    }

    void Update(const struct tm* t) {
        if (root_ == nullptr) {
            return;
        }
        if (t == nullptr || t->tm_year + 1900 < 2024) {
            for (auto& d : digits_) d.SetBlank();
            for (auto& d : seconds_) d.SetBlank();
            for (auto& d : date_digits_) d.SetBlank();
            lv_label_set_text(day_, "");
            lv_label_set_text(date_text_, "saat alinamadi");
            return;
        }

        digits_[0].Set(t->tm_hour / 10);
        digits_[1].Set(t->tm_hour % 10);
        digits_[2].Set(t->tm_min / 10);
        digits_[3].Set(t->tm_min % 10);
        seconds_[0].Set(t->tm_sec / 10);
        seconds_[1].Set(t->tm_sec % 10);
        date_digits_[0].Set(t->tm_mday / 10);
        date_digits_[1].Set(t->tm_mday % 10);
        date_digits_[2].Set((t->tm_mon + 1) / 10);
        date_digits_[3].Set((t->tm_mon + 1) % 10);
        // Iki nokta saniyede bir yanip sonsun: duran ekranin canli oldugunu
        // gosteren tek isaret.
        SetColonOn(t->tm_sec % 2 == 0);

        static const char* kDays[] = {"PAZAR",    "PAZARTESI", "SALI", "CARSAMBA",
                                      "PERSEMBE", "CUMA",      "CUMARTESI"};
        static const char* kMonths[] = {"Ocak",  "Subat",   "Mart",   "Nisan",
                                        "Mayis", "Haziran", "Temmuz", "Agustos",
                                        "Eylul", "Ekim",    "Kasim",  "Aralik"};
        if (t->tm_wday >= 0 && t->tm_wday < 7) {
            lv_label_set_text(day_, kDays[t->tm_wday]);
        }
        if (t->tm_mon >= 0 && t->tm_mon < 12) {
            lv_label_set_text_fmt(date_text_, "%d %s %d", t->tm_mday, kMonths[t->tm_mon],
                                  t->tm_year + 1900);
        }
    }

private:
    // 240x284 icin secilmis olculer. Saat blogu 160, saniye 33, arada 8 ->
    // 201 px; 240'a kenar bosluklariyla siger.
    static constexpr int kMargin = 12;
    static constexpr int kDigitW = 34;
    static constexpr int kDigitH = 58;
    static constexpr int kDigitT = 7;
    static constexpr int kDigitGap = 5;
    static constexpr int kColonW = 9;
    static constexpr int kSecW = 15;
    static constexpr int kSecH = 25;
    static constexpr int kSecT = 3;
    static constexpr int kSecGap = 3;
    static constexpr int kSecPad = 8;
    static constexpr int kTimeY = 96;
    static constexpr int kTagW = 34;
    static constexpr int kTagH = 18;

    // Referans fotograftan alinan palet: siyah zemin, buzlu beyaz rakamlar,
    // camgobegi kenarliklar, kirmizi uyari.
    static constexpr uint32_t kBg = 0x000000;
    static constexpr uint32_t kInk = 0xDEF2F8;
    static constexpr uint32_t kDim = 0x141A1D;  // sonmus segment
    static constexpr uint32_t kAccent = 0x37C8D8;
    static constexpr uint32_t kMuted = 0x9AA7AC;
    static constexpr uint32_t kFaint = 0x4A5459;
    static constexpr uint32_t kRed = 0xE02020;
    // Referanstaki renk vurgulari: kehribar saniye, mor ayirici, pil icin
    // yesil/sari/kirmizi kademe.
    static constexpr uint32_t kRedDim = 0x3A1010;
    static constexpr uint32_t kAmber = 0xFFB020;
    static constexpr uint32_t kAmberDim = 0x241A08;
    static constexpr uint32_t kCyanDim = 0x0A1E22;
    static constexpr uint32_t kViolet = 0x8B5CF6;
    static constexpr uint32_t kGreen = 0x35D07F;
    static constexpr uint32_t kYellow = 0xFFC53D;

    static const lv_font_t* kSmall() { return &font_noto_sans_basic_14_1; }
    static const lv_font_t* kLarge() { return &font_noto_sans_basic_20_4; }

    using Digit = SevenSegmentDigit;

    lv_obj_t* MakeBlock(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
        lv_obj_t* b = lv_obj_create(parent);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, w, h);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
        return b;
    }

    lv_obj_t* MakeLabel(lv_obj_t* parent, int x, int y, const lv_font_t* font, uint32_t color) {
        lv_obj_t* label = lv_label_create(parent);
        lv_label_set_text(label, "");
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, y);
        return label;
    }

    // Referanstaki "ALM" / "CHR" kutulari: ince camgobegi kenarlik, icinde
    // kisa etiket. Deger kutunun saginda ayri yazi olarak duruyor.
    lv_obj_t* MakeTag(int x, int y, const char* text, uint32_t color) {
        lv_obj_t* box = lv_obj_create(root_);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, kTagW, kTagH);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(color), 0);
        lv_obj_set_style_radius(box, 2, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t* label = lv_label_create(box);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, kSmall(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_center(label);
        tag_label_ = label;
        return box;
    }

    void MakeColon(int x, int y, int w, int h, int t) {
        for (int i = 0; i < 2; i++) {
            colon_dots_[i] =
                MakeBlock(root_, x + (w - t) / 2, y + (i == 0 ? h / 4 : 3 * h / 5), t, t, kInk);
        }
    }

    void SetColonOn(bool on) {
        for (lv_obj_t* dot : colon_dots_) {
            if (dot != nullptr) {
                lv_obj_set_style_bg_color(dot, lv_color_hex(on ? kInk : kDim), 0);
            }
        }
    }

    lv_obj_t* root_ = nullptr;
    lv_obj_t* colon_dots_[2] = {};
    lv_obj_t* pil_tag_ = nullptr;
    lv_obj_t* tag_label_ = nullptr;
    lv_obj_t* alarm_dot_ = nullptr;
    lv_obj_t* alarm_text_ = nullptr;
    lv_obj_t* battery_ = nullptr;
    lv_obj_t* volume_ = nullptr;
    lv_obj_t* day_ = nullptr;
    lv_obj_t* date_text_ = nullptr;
    lv_obj_t* weather_ = nullptr;
    Digit digits_[4];
    Digit seconds_[2];
    Digit date_digits_[4];
};
