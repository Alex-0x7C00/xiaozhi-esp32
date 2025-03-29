#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "font_awesome_symbols.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lvgl_port_touch.h>
#include <esp_lvgl_port_disp.h>
#include <driver/ledc.h>


#include <esp_lcd_ek79007.h>
#include "esp_lcd_touch_gt911.h"
#include <esp_lcd_touch.h>
#include <esp_ldo_regulator.h>


#define TAG "esp_sparkbot"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class SparkBotEs8311AudioCodec : public Es8311AudioCodec {
private:    

public:
    SparkBotEs8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
                        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                        gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk = true)
        : Es8311AudioCodec(i2c_master_handle, i2c_port, input_sample_rate, output_sample_rate,
                             mclk,  bclk,  ws,  dout,  din,pa_pin,  es8311_addr,  use_mclk = true) {}

    void EnableOutput(bool enable) override {
        if (enable == output_enabled_) {
            return;
        }
        if (enable) {
            Es8311AudioCodec::EnableOutput(enable);
        } else {
           // Nothing todo because the display io and PA io conflict
        }
    }
};

class EspSparkBot : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = 1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            //.glitch_ignore_cnt = 7,
            //.intr_priority = 0,
            //.trans_queue_depth = 0,
            //.flags = {
            //    .enable_internal_pullup = 1,
            //},
        };

        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
    }

    void InitializeDisplay() {
        esp_err_t ret;
        //const ledc_channel_config_t LCD_backlight_channel = {
        //    .gpio_num = DISPLAY_BACKLIGHT_PIN,
        //    .speed_mode = LEDC_LOW_SPEED_MODE,
        //    .channel = DISPLAY_BACKLIGHT_LEDC_CHANNEL,
        //    .intr_type = LEDC_INTR_DISABLE,
        //    .timer_sel = DISPLAY_BACKLIGHT_LEDC_TIMER,
        //    .duty = 0,
        //    .hpoint = 0
        //};
        //const ledc_timer_config_t LCD_backlight_timer = {
        //    .speed_mode = LEDC_LOW_SPEED_MODE,
        //    .duty_resolution = LEDC_TIMER_10_BIT,
        //    .timer_num = DISPLAY_BACKLIGHT_LEDC_TIMER,
        //    .freq_hz = 5000,
        //    .clk_cfg = LEDC_AUTO_CLK
        //};
    //
        //ledc_timer_config(&LCD_backlight_timer);
        //ledc_channel_config(&LCD_backlight_channel);

        static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = 3,
            .voltage_mv = 2500,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
        ESP_LOGI(TAG, "MIPI DSI PHY Powered on");

        esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = 2,
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = 1000,
        };
        ret = esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "DSI bus init failed: %s", esp_err_to_name(ret));
            return;
        }

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        esp_lcd_panel_io_handle_t io;
        esp_lcd_dbi_io_config_t dbi_config = {
            .virtual_channel = 0,
            .lcd_cmd_bits = 8,   // according to the LCD ILI9881C spec
            .lcd_param_bits = 8, // according to the LCD ILI9881C spec
        };

        ret = esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "New panel IO failed %s", esp_err_to_name(ret));
            return;
        }
        esp_lcd_panel_handle_t disp_panel = NULL;
        // create EK79007 control panel
        ESP_LOGI(TAG, "Install EK79007 LCD control panel");

        esp_lcd_dpi_panel_config_t dpi_config = {
            .virtual_channel = 0,
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,             
            .dpi_clock_freq_mhz = 52,
            .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
            .num_fbs = 1,
            .video_timing = {
                .h_size = DISPLAY_WIDTH,
                .v_size = DISPLAY_HEIGHT,
                .hsync_pulse_width = 10,
                .hsync_back_porch = 160,
                .hsync_front_porch = 160,
                .vsync_pulse_width = 1,
                .vsync_back_porch = 23,
                .vsync_front_porch = 12,
            },
            .flags = {
                .use_dma2d = true,
                //.disable_lp = true,
            },
        };
        ek79007_vendor_config_t vendor_config = {
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
        };
        esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = DISPLAY_RST_GPIO,
            .rgb_ele_order = ESP_LCD_COLOR_SPACE_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_config,
        };
        ret = esp_lcd_new_panel_ek79007(io, &lcd_dev_config, &disp_panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "New LCD panel EK79007 failed %s", esp_err_to_name(ret));
            return;
        }
        ret = esp_lcd_panel_reset(disp_panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD panel reset failed %s", esp_err_to_name(ret));
            return;
        }
        ret = esp_lcd_panel_init(disp_panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD panel init failed %s", esp_err_to_name(ret));
            return;
        }

            // create EK79007 control panel
        ESP_LOGI(TAG, "Install EK79007 LCD control panel");
        display_ = new MipiLcdDisplay(io,disp_panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
            {
                .text_font = &font_puhui_20_4,
                .icon_font = &font_awesome_20_4,
                .emoji_font = font_emoji_64_init(),    
            });


        //i2c_master_bus_config_t i2c_bus_conf = {
        //    .i2c_port = 1,
        //    .sda_io_num = GPIO_NUM_7,
        //    .scl_io_num = GPIO_NUM_8,
        //    .clk_source = I2C_CLK_SRC_DEFAULT,
        //};
        //i2c_master_bus_handle_t i2c_handle = NULL;
        //i2c_new_master_bus(&i2c_bus_conf, &i2c_handle);
//
        //const esp_lcd_touch_config_t tp_cfg = {
        //        .x_max = DISPLAY_WIDTH,
        //        .y_max = DISPLAY_HEIGHT,
        //        .rst_gpio_num = GPIO_NUM_NC,
        //        .int_gpio_num = GPIO_NUM_NC,
        //        .levels = {
        //            .reset = 0,
        //            .interrupt = 0,
        //        },
        //        .flags = {
        //            .swap_xy = 0,
        //            .mirror_x = 1,
        //            .mirror_y = 1,
        //        },
        //    };
//
        //
        //esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        //esp_lcd_touch_handle_t tp;
        //esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        //tp_io_config.scl_speed_hz = 400000;
        //ret = esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle);
        //if (ret != ESP_OK) {
        //    ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c failed %s", esp_err_to_name(ret));
        //    return;
        //}
        //esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp);
        //const lvgl_port_touch_cfg_t touch_cfg = {
        //    .disp = disp,
        //    .handle = tp,
        //};
        //lvgl_port_add_touch(&touch_cfg);
    }


    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
    }

public:
    EspSparkBot() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        //InitializeSpi();
        InitializeDisplay();
        //InitializeButtons();
        InitializeIot();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
         static SparkBotEs8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(EspSparkBot);
