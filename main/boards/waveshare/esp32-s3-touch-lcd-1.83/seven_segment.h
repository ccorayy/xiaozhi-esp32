#pragma once

// ---------------------------------------------------------------------------
// Yedi-segment rakam cizici.
//
// Font yerine yedi dikdortgen: olcek serbest, binary buyumuyor, lisans derdi
// yok (referans projenin G7 fontu 187 KB ve ticaride lisansli). Hem saat yuzu
// (segment_clock.h) hem radyo tuner ekrani bunu kullaniyor.
//
// ⚠️ Her basamak KENDI kutusunu aliyor ve segmentler o kutuya
// LV_ALIGN_TOP_LEFT ile konuyor. Merkez hizalama kullanilirsa yatay (ince) ve
// dikey (uzun) segmentler kendi boylarinin yarisi kadar farkli kayar, rakam
// ikiye boluner - bir surum tam olarak bu yuzden bozuk cikti.
// ---------------------------------------------------------------------------

#include <lvgl.h>

#include <cstdint>

struct SevenSegmentDigit {
    lv_obj_t* seg[7] = {};
    lv_obj_t* box = nullptr;
    uint32_t on_color = 0xFFFFFF;
    uint32_t off_color = 0x101010;

    void Create(lv_obj_t* parent, int x, int y, int w, int h, int t,
                uint32_t on = 0xFFFFFF, uint32_t off = 0x101010) {
        on_color = on;
        off_color = off;

        box = lv_obj_create(parent);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, w, h);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);

        int vert = (h - 3 * t) / 2;
        int horiz = w - 2 * t;
        int mid = (h - t) / 2;
        Make(0, t, 0, horiz, t);           // a  ust
        Make(1, w - t, t, t, vert);        // b  sag ust
        Make(2, w - t, mid + t, t, vert);  // c  sag alt
        Make(3, t, h - t, horiz, t);       // d  alt
        Make(4, 0, mid + t, t, vert);      // e  sol alt
        Make(5, 0, t, t, vert);            // f  sol ust
        Make(6, t, mid, horiz, t);         // g  orta
    }

    void Set(int value) {
        // Bit sirasi a=0 ... g=6
        static const uint8_t kMap[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                         0x6D, 0x7D, 0x07, 0x7F, 0x6F};
        if (value < 0 || value > 9) {
            SetBlank();
            return;
        }
        for (int i = 0; i < 7; i++) {
            Paint(i, (kMap[value] >> i) & 1);
        }
    }

    void SetBlank() {
        for (int i = 0; i < 7; i++) {
            Paint(i, false);
        }
    }

    void SetHidden(bool hidden) {
        if (box == nullptr) {
            return;
        }
        if (hidden) {
            lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
        }
    }

private:
    void Make(int index, int x, int y, int w, int h) {
        lv_obj_t* s = lv_obj_create(box);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, w < 1 ? 1 : w, h < 1 ? 1 : h);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s, 1, 0);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s, LV_OBJ_FLAG_EVENT_BUBBLE);
        seg[index] = s;
        Paint(index, false);
    }

    void Paint(int index, bool on) {
        if (seg[index] != nullptr) {
            lv_obj_set_style_bg_color(seg[index], lv_color_hex(on ? on_color : off_color), 0);
        }
    }
};
