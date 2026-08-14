#include "wifi_board.h"
#include "display/lcd_display.h"
#include "imu_qmi8658.h"
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

#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <cmath>
#include <cstdio>

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
    // Hareket algilama (QMI8658)
    //   eline alinca / masaya vurunca  -> ekrani uyandir
    //   ters cevirince (yuzustu)       -> sesi kapat, duzelince geri ac
    // ------------------------------------------------------------------
    Qmi8658* imu_ = nullptr;
    esp_timer_handle_t imu_timer_ = nullptr;
    float imu_last_x_ = 0, imu_last_y_ = 0, imu_last_z_ = 0;
    bool imu_have_sample_ = false;
    int imu_log_tick_ = 0;
    int imu_face_down_count_ = 0;
    bool imu_muted_ = false;
    int imu_volume_before_mute_ = 60;

    static constexpr float kMotionThreshold = 0.18f;   // g cinsinden degisim
    static constexpr float kFaceDownZ = -0.65f;        // yuzustu esigi
    static constexpr float kFaceUpZ = 0.30f;           // duzeldi kabul esigi
    static constexpr int kFaceDownSamples = 4;         // ~0.8 sn dogrulama

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
            esp_timer_start_periodic(imu_timer_, 200000);  // 200 ms
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
                ESP_LOGI(TAG, "IMU hareket: delta=%.2f -> uyandirildi", delta);
            }
        }

        // TESHIS: eksenlerin gercek degerlerini gorelim (2 saniyede bir).
        // Esikler dogrulaninca bu blok silinecek.
        if (++imu_log_tick_ >= 10) {
            imu_log_tick_ = 0;
            ESP_LOGI(TAG, "IMU x=%+.2f y=%+.2f z=%+.2f delta=%.2f yuzustu_sayac=%d sessiz=%d",
                     x, y, z, delta, imu_face_down_count_, imu_muted_ ? 1 : 0);
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
        } else if (z > kFaceUpZ) {
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
                EnterWifiConfigMode();
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
        settings_display->SetOnWifiConfigRequest([this]() { EnterWifiConfigMode(); });
        settings_display->SetOnUserActivity([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
        });
        settings_display->SetSdInfoProvider([this]() { return sd_status_; });
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
            "Valid values for `app`: menu, chat, clock, settings, wifi, info. "
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

        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchLCD1inch83() : boot_button_(BOOT_BUTTON_GPIO), pwr_button_(PWR_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeAxp2101();
        InitializeImu();
        InitializeSdCard();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
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
