#pragma once

// ---------------------------------------------------------------------------
// PCF85063 RTC - pil yedekli gercek zaman saati.
//
// Kartta var (uretici teknik tablosu: "RTC | PCF85063") ama xiaozhi agacinda
// surucusu yoktu. Kayit haritasi Waveshare'in Arduino ornegindeki SensorLib
// dosyasindan dogrulandi (PCF85063Constants.h).
//
// Ne ise yariyor: cihaz saati sunucudan aliyor, ama WiFi baglanana kadar saat
// yanlis oluyor ve elektrik kesilirse sifirlaniyor. RTC pil uzerinden calisip
// saati koruyor; acilista sistem saatini ondan kuruyoruz.
//
// DIKKAT: I2cDevice::ReadReg icinde ESP_ERROR_CHECK var (cip yoksa cihaz
// cokuyor). IMU'da oldugu gibi okumalari burada elle yapiyoruz.
// ---------------------------------------------------------------------------

#include "i2c_device.h"

#include <esp_log.h>

#include <ctime>

class Pcf85063 : public I2cDevice {
public:
    static constexpr uint8_t kAddr = 0x51;

    Pcf85063(i2c_master_bus_handle_t bus, uint8_t addr) : I2cDevice(bus, addr) {}

    // Uretici surucusunun yaptigi ile ayni: 24 saat kipi (Control_1 bit 1 = 0)
    // ve saati calistir (STOP biti = 0). Kristal yuk kondansatoru bitine
    // (CAP_SEL) uretici de dokunmuyor, biz de dokunmuyoruz.
    bool Initialize() {
        uint8_t control = 0;
        if (!SafeRead(kRegControl1, &control, 1)) {
            return false;
        }
        uint8_t wanted = control & ~0x22;
        if (wanted != control) {
            WriteReg(kRegControl1, wanted);
        }
        return true;
    }

    // Saat gecerliyse true. Saniye kaydinin 7. biti "osilator durdu" demek,
    // yani cip enerjisiz kalmis ve icindeki zaman anlamsiz.
    bool ReadTime(struct tm& out) {
        uint8_t b[7];
        if (!SafeRead(kRegSeconds, b, sizeof(b))) {
            return false;
        }
        if (b[0] & 0x80) {
            return false;  // osilator durmus
        }
        out = {};
        out.tm_sec = FromBcd(b[0] & 0x7F);
        out.tm_min = FromBcd(b[1] & 0x7F);
        out.tm_hour = FromBcd(b[2] & 0x3F);
        out.tm_mday = FromBcd(b[3] & 0x3F);
        out.tm_wday = b[4] & 0x07;
        out.tm_mon = FromBcd(b[5] & 0x1F) - 1;
        out.tm_year = FromBcd(b[6]) + 100;  // cip 00-99 tutuyor, tm 1900 tabanli
        out.tm_isdst = -1;
        return out.tm_year + 1900 >= 2024;
    }

    void WriteTime(const struct tm& t) {
        uint8_t b[7];
        b[0] = ToBcd(t.tm_sec) & 0x7F;  // 7. bit 0 -> osilator calisiyor
        b[1] = ToBcd(t.tm_min);
        b[2] = ToBcd(t.tm_hour);
        b[3] = ToBcd(t.tm_mday);
        b[4] = static_cast<uint8_t>(t.tm_wday & 0x07);
        b[5] = ToBcd(t.tm_mon + 1);
        b[6] = ToBcd((t.tm_year + 1900) % 100);
        for (size_t i = 0; i < sizeof(b); i++) {
            WriteReg(kRegSeconds + i, b[i]);
        }
    }

private:
    static constexpr uint8_t kRegControl1 = 0x00;
    static constexpr uint8_t kRegSeconds = 0x04;

    static int FromBcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
    static uint8_t ToBcd(int v) {
        return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
    }

    bool SafeRead(uint8_t reg, uint8_t* buffer, size_t length) {
        return i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, 100) == ESP_OK;
    }
};
