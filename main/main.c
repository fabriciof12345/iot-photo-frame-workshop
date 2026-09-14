#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "network.h"
#include "photo_storage.h"

#define LCD_WIDTH 800
#define LCD_HEIGHT 480
#define RGB565_BYTES_PER_PIXEL 2
#define EXPECTED_IMAGE_SIZE (LCD_WIDTH * LCD_HEIGHT * RGB565_BYTES_PER_PIXEL)

#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO 8
#define I2C_SCL_GPIO 9

// CH422G uses separate I2C addresses for each write-only register.
#define CH422G_WR_SET_ADDR 0x24
#define CH422G_WR_IO_ADDR 0x38
#define CH422G_IO_OUTPUT_ENABLE 0x01

#define EXIO_LCD_BACKLIGHT 2
#define EXIO_LCD_RESET 3

extern const uint8_t embedded_photo_start[] asm("_binary_sze_rgb565_start");
extern const uint8_t embedded_photo_end[] asm("_binary_sze_rgb565_end");

static const char *TAG = "pixel_frame";
static uint8_t s_expander_output = 0xFF;

static esp_err_t ch422g_write(uint8_t address, uint8_t value)
{
    return i2c_master_write_to_device(
        I2C_PORT, address, &value, sizeof(value), pdMS_TO_TICKS(100)
    );
}

static esp_err_t expander_set_output(int pin, bool high)
{
    if (high) {
        s_expander_output |= (uint8_t)(1U << pin);
    } else {
        s_expander_output &= (uint8_t)~(1U << pin);
    }

    return ch422g_write(CH422G_WR_IO_ADDR, s_expander_output);
}

static esp_err_t init_board_control(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
        .clk_flags = 0,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &config), TAG, "configure I2C");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_PORT, config.mode, 0, 0, 0), TAG, "install I2C driver");
    ESP_RETURN_ON_ERROR(ch422g_write(CH422G_WR_SET_ADDR, CH422G_IO_OUTPUT_ENABLE), TAG, "enable CH422G outputs");

    // Keep the screen dark while its framebuffer is being populated.
    ESP_RETURN_ON_ERROR(expander_set_output(EXIO_LCD_BACKLIGHT, false), TAG, "turn backlight off");
    ESP_RETURN_ON_ERROR(expander_set_output(EXIO_LCD_RESET, false), TAG, "assert LCD reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(expander_set_output(EXIO_LCD_RESET, true), TAG, "release LCD reset");
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}

static esp_err_t init_lcd(esp_lcd_panel_handle_t *panel)
{
    const esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = LCD_WIDTH,
            .v_res = LCD_HEIGHT,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = {
                .pclk_active_neg = true,
            },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_WIDTH * 10,
        .sram_trans_align = 64,
        .psram_trans_align = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            14, 38, 18, 17, 10, 39, 0, 45,
            48, 47, 21, 1, 2, 42, 41, 40,
        },
        .flags = {
            .fb_in_psram = true,
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&config, panel), TAG, "create RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "reset RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "initialize RGB panel");

    return ESP_OK;
}

static esp_err_t draw_embedded_photo(esp_lcd_panel_handle_t panel)
{
    const size_t image_size = (size_t)(embedded_photo_end - embedded_photo_start);
    ESP_RETURN_ON_FALSE(
        image_size == EXPECTED_IMAGE_SIZE,
        ESP_ERR_INVALID_SIZE,
        TAG,
        "embedded image size is %u bytes; expected %u",
        (unsigned)image_size,
        (unsigned)EXPECTED_IMAGE_SIZE
    );

    return esp_lcd_panel_draw_bitmap(
        panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, embedded_photo_start
    );
}

static esp_err_t draw_startup_photo(esp_lcd_panel_handle_t panel)
{
    uint8_t *persisted_frame = heap_caps_malloc(
        PHOTO_STORAGE_FRAME_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (persisted_frame != NULL) {
        esp_err_t load_result = photo_storage_load(persisted_frame, PHOTO_STORAGE_FRAME_SIZE);
        if (load_result == ESP_OK) {
            esp_err_t draw_result = esp_lcd_panel_draw_bitmap(
                panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, persisted_frame
            );
            free(persisted_frame);
            if (draw_result == ESP_OK) {
                ESP_LOGI(TAG, "Persisted cloud photo displayed");
            }
            return draw_result;
        }
        free(persisted_frame);
        if (load_result != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Persisted photo is invalid: %s", esp_err_to_name(load_result));
        }
    } else {
        ESP_LOGW(TAG, "Could not allocate persisted photo buffer; using fallback");
    }

    ESP_LOGI(TAG, "No valid persisted cloud photo; drawing bundled fallback");
    return draw_embedded_photo(panel);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Waveshare ESP32-S3 Touch LCD 7 photo test");
    ESP_LOGI(TAG, "Free PSRAM before LCD init: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ESP_ERROR_CHECK(init_board_control());

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(init_lcd(&panel));
    ESP_LOGI(TAG, "RGB panel initialized; selecting startup photo");

    ESP_ERROR_CHECK(draw_startup_photo(panel));
    ESP_ERROR_CHECK(expander_set_output(EXIO_LCD_BACKLIGHT, true));

    ESP_LOGI(TAG, "Photo displayed");

    esp_err_t network_result = photo_frame_network_start(panel);
    if (network_result != ESP_OK) {
        ESP_LOGW(TAG, "Cloud updates are disabled: %s", esp_err_to_name(network_result));
    }
}
