#pragma once

// ---------------------------------------------------------------------------
// QMI8658 6 eksenli IMU - sadece ivmeolcer kismi.
//
// Kartta bu cip var (uretici teknik tablosu: "Motion sensor: QMI8658 six-axis
// IMU") ama BSP bileseni onu sarmalamiyor (BSP_CAPS_IMU 0) ve xiaozhi agacinda
// da surucusu yoktu. Register haritasi Waveshare'in kendi bileseninden alindi
// (waveshare/qmi8658 v2.0.0, Apache-2.0).
//
// Neden kendi surucumuz: bileseni eklemek main/idf_component.yml'i degistirmek
// demekti, o da butun board'larin derlemesini etkiliyor. Ihtiyacimiz olan kisim
// zaten birkac register.
//
// DIKKAT: I2cDevice::ReadReg icinde ESP_ERROR_CHECK var; cip yanit vermezse
// cihaz komple cokuyor. Bu yuzden okumalari burada elle yapip hatayi yutuyoruz.
// ---------------------------------------------------------------------------

#include "i2c_device.h"

#include <esp_log.h>

class Qmi8658 : public I2cDevice {
public:
    // SA0 pinine gore iki olasi adres; hangisi cevap verirse o kullanilir.
    static constexpr uint8_t kAddrLow = 0x6A;
    static constexpr uint8_t kAddrHigh = 0x6B;

    Qmi8658(i2c_master_bus_handle_t bus, uint8_t addr) : I2cDevice(bus, addr) {}

    bool Initialize() {
        uint8_t who = 0;
        if (!SafeRead(kRegWhoAmI, &who, 1) || who != 0x05) {
            ESP_LOGW(kTag, "WHO_AM_I beklenen 0x05 degil: 0x%02X", who);
            return false;
        }
        WriteReg(kRegCtrl1, 0x60);  // adres otomatik artsin, ic ayarlar
        // CTRL2: ust 4 bit olcek (0x01 = +-4g), alt 4 bit ornekleme (0x06 = 125 Hz)
        WriteReg(kRegCtrl2, (0x01 << 4) | 0x06);
        WriteReg(kRegCtrl7, 0x01);  // yalnizca ivmeolcer acik (jiroskop kapali,
                                    // pil icin: jiroskop belirgin akim cekiyor)
        ESP_LOGI(kTag, "QMI8658 hazir (adres 0x%02X)", device_address_);
        return true;
    }

    // Yer cekimi katsayisi cinsinden (1.0 = 1g). Basarisiz olursa false.
    bool ReadAccel(float& x, float& y, float& z) {
        uint8_t buf[6];
        if (!SafeRead(kRegAccelX, buf, sizeof(buf))) {
            return false;
        }
        x = ToG(buf[0], buf[1]);
        y = ToG(buf[2], buf[3]);
        z = ToG(buf[4], buf[5]);
        return true;
    }

private:
    static constexpr const char* kTag = "Qmi8658";
    static constexpr uint8_t kRegWhoAmI = 0x00;
    static constexpr uint8_t kRegCtrl1 = 0x02;
    static constexpr uint8_t kRegCtrl2 = 0x03;
    static constexpr uint8_t kRegCtrl7 = 0x08;
    static constexpr uint8_t kRegAccelX = 0x35;
    // +-4g secildiginde 1g = 8192 sayim
    static constexpr float kCountsPerG = 8192.0f;

    static float ToG(uint8_t low, uint8_t high) {
        return static_cast<int16_t>((static_cast<uint16_t>(high) << 8) | low) / kCountsPerG;
    }

    bool SafeRead(uint8_t reg, uint8_t* buffer, size_t length) {
        return i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, 100) == ESP_OK;
    }
};
