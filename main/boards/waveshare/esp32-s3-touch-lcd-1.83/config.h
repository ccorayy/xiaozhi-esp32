#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8

#define AUDIO_CODEC_PA_PIN      GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_15
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_14
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define PWR_BUTTON_GPIO         GPIO_NUM_41

#define DISPLAY_SPI_MODE        3
#define DISPLAY_CS_PIN          GPIO_NUM_5
#define DISPLAY_MOSI_PIN        GPIO_NUM_7
#define DISPLAY_MISO_PIN        GPIO_NUM_NC
#define DISPLAY_CLK_PIN         GPIO_NUM_6
#define DISPLAY_DC_PIN          GPIO_NUM_4
#define DISPLAY_RST_PIN         GPIO_NUM_38

#define DISPLAY_WIDTH           240
#define DISPLAY_HEIGHT          284
#define DISPLAY_MIRROR_X        false
#define DISPLAY_MIRROR_Y        false
#define DISPLAY_SWAP_XY         false

#define DISPLAY_OFFSET_X        0
#define DISPLAY_OFFSET_Y        0

#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_40
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// microSD - SDMMC 1-bit. Kart algilama (CD) ve yazma korumasi (WP) pini yok.
// Kaynak: Waveshare BSP bileseni waveshare/esp32_s3_touch_lcd_1_83 v2.0.0,
// include/bsp/esp32_s3_touch_lcd_1_83.h satir 57-59. Stok config.h'da yoktu.
#define SD_D0_PIN               GPIO_NUM_3
#define SD_CMD_PIN              GPIO_NUM_1
#define SD_CLK_PIN              GPIO_NUM_2
#define SD_MOUNT_POINT          "/sdcard"

#endif // _BOARD_CONFIG_H_
