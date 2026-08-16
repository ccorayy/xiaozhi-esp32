#pragma once

// ---------------------------------------------------------------------------
// Yedi-segment saat yuzu - VolosR/pocketClock gorunumunun bizim ekrana
// olceklenmis hali.
//
// Referans cihaz 368x448 AMOLED, bizimki 240x284. En-boy oranlari birbirine
// cok yakin (0.82 / 0.85), o yuzden tek bir olcek carpani butun yerlesimi
// tasiyor: her olcu referanstaki degerin `scale_` katidir. Ekran degisirse
// tasarim kendiliginden uyar.
//
// Rakamlar icin font KULLANMIYORUZ. Referans G7_Segment_7a.ttf'i 100 px'te
// gomuyor ve tek basina 187 KB yer kapliyor; ayrica fontun kendi lisansi var.
// Yedi-segment zaten yedi dikdortgen - LVGL nesneleriyle ciziyoruz. Boylece
// olcek serbest, binary buyumuyor ve kod tamamen bizim.
//
// Sonuk segmentler de ciziliyor (yalnizca sonmus olanlar gizlenmiyor, koyu
// renkte duruyor): gercek LCD saatlerde oyle gorunur, tasarimi inandirici
// yapan asil detay bu.
// ---------------------------------------------------------------------------

#include <lvgl.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

class SegmentClock {
public:
    void Create(lv_obj_t* parent, int width, int height) {
        scale_ = std::min(width / static_cast<float>(kRefW), height / static_cast<float>(kRefH));

        root_ = lv_obj_create(parent);
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, width, height);
        lv_obj_center(root_);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(root_, lv_color_hex(kBackground), 0);

        // Ic ice iki cerceve: referanstaki panel yiginini veren sey bu.
        lv_obj_t* frame = MakePanel(root_, S(kRefFrameW), S(kRefFrameH), S(18), kFrame);
        lv_obj_set_style_border_width(frame, S(3) < 1 ? 1 : S(3), 0);
        lv_obj_set_style_border_color(frame, lv_color_hex(kFrameEdge), 0);
        lv_obj_center(frame);

        lv_obj_t* face = MakePanel(frame, S(kRefFaceW), S(kRefFaceH), S(12), kFace);
        lv_obj_center(face);

        // Saat: HH:MM. Iki basamak, iki nokta, iki basamak.
        int digit_w = S(kRefDigitW);
        int digit_h = S(kRefDigitH);
        int thick = std::max(3, S(kRefThick));
        int gap = std::max(2, S(kRefDigitGap));
        int colon_w = std::max(4, S(kRefColonW));
        int total = 4 * digit_w + 3 * gap + colon_w;

        int x = -total / 2;
        int y = -S(14);
        for (int i = 0; i < 4; i++) {
            if (i == 2) {
                colon_ = MakeColon(face, x, y, colon_w, digit_h, thick);
                x += colon_w + gap;
            }
            digits_[i].Create(face, x, y, digit_w, digit_h, thick);
            x += digit_w + gap;
        }

        // Saniye: referansta saatin sagina, daha kucuk gosterge olarak duruyor.
        int sec_w = S(kRefSecW);
        int sec_h = S(kRefSecH);
        int sec_thick = std::max(2, S(kRefSecThick));
        int sec_x = total / 2 - 2 * sec_w - gap;
        int sec_y = y + digit_h + S(16);
        for (int i = 0; i < 2; i++) {
            seconds_[i].Create(face, sec_x + i * (sec_w + gap), sec_y, sec_w, sec_h, sec_thick);
        }

        // Yazi satirlari. Referansta gun ustte, tarih altta duruyor.
        day_ = MakeLabel(face, LV_ALIGN_TOP_LEFT, S(14), S(10), kInk);
        battery_ = MakeLabel(face, LV_ALIGN_TOP_RIGHT, -S(14), S(10), kInk);
        date_ = MakeLabel(face, LV_ALIGN_BOTTOM_LEFT, S(14), -S(10), kMuted);
        weather_ = MakeLabel(face, LV_ALIGN_BOTTOM_RIGHT, -S(14), -S(10), kMuted);
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

    void SetWeather(const std::string& text) {
        if (weather_ != nullptr) {
            lv_label_set_text(weather_, text.c_str());
        }
    }

    void SetBattery(int level, bool charging) {
        if (battery_ == nullptr) {
            return;
        }
        lv_label_set_text_fmt(battery_, "%d%s", level, charging ? "%+" : "%");
        lv_obj_set_style_text_color(battery_, lv_color_hex(level <= 20 ? kWarn : kInk), 0);
    }

    // Saat gecerli degilse (ag yok, RTC bos) rakamlari sondurup sebebini yaz.
    void Update(const struct tm* t) {
        if (root_ == nullptr) {
            return;
        }
        if (t == nullptr || t->tm_year + 1900 < 2024) {
            for (auto& digit : digits_) {
                digit.SetBlank();
            }
            for (auto& digit : seconds_) {
                digit.SetBlank();
            }
            lv_label_set_text(day_, "");
            lv_label_set_text(date_, "saat alinamadi");
            return;
        }

        digits_[0].Set(t->tm_hour / 10);
        digits_[1].Set(t->tm_hour % 10);
        digits_[2].Set(t->tm_min / 10);
        digits_[3].Set(t->tm_min % 10);
        seconds_[0].Set(t->tm_sec / 10);
        seconds_[1].Set(t->tm_sec % 10);
        // Iki nokta saniyede bir yanip sonsun - duran ekranin canli oldugunu
        // gosteren tek isaret bu.
        SetColonOn(t->tm_sec % 2 == 0);

        static const char* kDays[] = {"PAZAR",    "PAZARTESI", "SALI", "CARSAMBA",
                                      "PERSEMBE", "CUMA",      "CUMARTESI"};
        static const char* kMonths[] = {"Ocak",   "Subat",   "Mart", "Nisan", "Mayis", "Haziran",
                                        "Temmuz", "Agustos", "Eylul", "Ekim", "Kasim", "Aralik"};
        if (t->tm_wday >= 0 && t->tm_wday < 7) {
            lv_label_set_text(day_, kDays[t->tm_wday]);
        }
        if (t->tm_mon >= 0 && t->tm_mon < 12) {
            lv_label_set_text_fmt(date_, "%d %s", t->tm_mday, kMonths[t->tm_mon]);
        }
    }

private:
    // Referans cihazin ekrani ve o ekrana gore secilmis olculer. Buradaki
    // sayilar 368x448 icindir; S() hepsini bizim ekrana tasir.
    static constexpr int kRefW = 368;
    static constexpr int kRefH = 448;
    static constexpr int kRefFrameW = 356;
    static constexpr int kRefFrameH = 321;
    static constexpr int kRefFaceW = 327;
    static constexpr int kRefFaceH = 232;
    static constexpr int kRefDigitW = 62;
    static constexpr int kRefDigitH = 104;
    static constexpr int kRefThick = 13;
    static constexpr int kRefDigitGap = 9;
    static constexpr int kRefColonW = 16;
    static constexpr int kRefSecW = 26;
    static constexpr int kRefSecH = 44;
    static constexpr int kRefSecThick = 6;

    // Referanstan alinan palet: buzlu acik mavi, koyu zemin.
    static constexpr uint32_t kBackground = 0x1D1A21;
    static constexpr uint32_t kFrame = 0x232028;
    static constexpr uint32_t kFrameEdge = 0x3A3737;
    static constexpr uint32_t kFace = 0x16141A;
    static constexpr uint32_t kInk = 0xDEF2F8;
    static constexpr uint32_t kDim = 0x262A2E;  // sonmus segment
    static constexpr uint32_t kMuted = 0xB2A8A8;
    static constexpr uint32_t kWarn = 0xD81515;

    // Tek bir yedi-segment basamak. Segment sirasi: a b c d e f g.
    struct Digit {
        lv_obj_t* seg[7] = {};

        void Create(lv_obj_t* parent, int x, int y, int w, int h, int t) {
            int vert = (h - t) / 2 - t;      // dikey segment boyu
            int horiz = w - 2 * t;           // yatay segment boyu
            int mid = (h - t) / 2;
            //            x,          y,        w,      h
            Make(parent, 0, x + t, y, horiz, t);              // a  ust
            Make(parent, 1, x + w - t, y + t, t, vert);       // b  sag ust
            Make(parent, 2, x + w - t, y + mid + t, t, vert); // c  sag alt
            Make(parent, 3, x + t, y + h - t, horiz, t);      // d  alt
            Make(parent, 4, x, y + mid + t, t, vert);         // e  sol alt
            Make(parent, 5, x, y + t, t, vert);               // f  sol ust
            Make(parent, 6, x + t, y + mid, horiz, t);        // g  orta
        }

        void Set(int value) {
            // Bit sirasi a=0 ... g=6
            static const uint8_t kMap[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                             0x6D, 0x7D, 0x07, 0x7F, 0x6F};
            if (value < 0 || value > 9) {
                SetBlank();
                return;
            }
            uint8_t mask = kMap[value];
            for (int i = 0; i < 7; i++) {
                Paint(i, (mask >> i) & 1);
            }
        }

        void SetBlank() {
            for (int i = 0; i < 7; i++) {
                Paint(i, false);
            }
        }

    private:
        void Make(lv_obj_t* parent, int index, int x, int y, int w, int h) {
            lv_obj_t* s = lv_obj_create(parent);
            lv_obj_remove_style_all(s);
            lv_obj_set_size(s, w < 1 ? 1 : w, h < 1 ? 1 : h);
            lv_obj_align(s, LV_ALIGN_CENTER, x, y);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s, 1, 0);
            lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s, LV_OBJ_FLAG_EVENT_BUBBLE);
            seg[index] = s;
            Paint(index, false);
        }

        void Paint(int index, bool on) {
            if (seg[index] != nullptr) {
                lv_obj_set_style_bg_color(seg[index], lv_color_hex(on ? kInk : kDim), 0);
            }
        }
    };

    int S(int reference_px) const { return static_cast<int>(reference_px * scale_ + 0.5f); }

    lv_obj_t* MakePanel(lv_obj_t* parent, int w, int h, int radius, uint32_t color) {
        lv_obj_t* panel = lv_obj_create(parent);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, w, h);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
        lv_obj_set_style_radius(panel, radius, 0);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
        return panel;
    }

    lv_obj_t* MakeLabel(lv_obj_t* parent, lv_align_t align, int x, int y, uint32_t color) {
        lv_obj_t* label = lv_label_create(parent);
        lv_label_set_text(label, "");
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_align(label, align, x, y);
        return label;
    }

    lv_obj_t* MakeColon(lv_obj_t* parent, int x, int y, int w, int h, int t) {
        lv_obj_t* holder = lv_obj_create(parent);
        lv_obj_remove_style_all(holder);
        lv_obj_set_size(holder, w, h);
        lv_obj_align(holder, LV_ALIGN_CENTER, x, y);
        lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(holder, LV_OBJ_FLAG_EVENT_BUBBLE);
        for (int i = 0; i < 2; i++) {
            lv_obj_t* dot = lv_obj_create(holder);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, t, t);
            lv_obj_align(dot, LV_ALIGN_CENTER, 0, i == 0 ? -h / 5 : h / 5);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(kInk), 0);
            lv_obj_set_style_radius(dot, 1, 0);
            colon_dots_[i] = dot;
        }
        return holder;
    }

    void SetColonOn(bool on) {
        for (lv_obj_t* dot : colon_dots_) {
            if (dot != nullptr) {
                lv_obj_set_style_bg_color(dot, lv_color_hex(on ? kInk : kDim), 0);
            }
        }
    }

    float scale_ = 1.0f;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* colon_ = nullptr;
    lv_obj_t* colon_dots_[2] = {};
    lv_obj_t* day_ = nullptr;
    lv_obj_t* date_ = nullptr;
    lv_obj_t* battery_ = nullptr;
    lv_obj_t* weather_ = nullptr;
    Digit digits_[4];
    Digit seconds_[2];
};
