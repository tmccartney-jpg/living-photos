/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "waveshare_rgb_lcd_port.h"
#include "hmmd.h"
#include "diag.h"
#include "presence.h"
#include "sdcard.h"
#include "frame.h"
#include "wifi.h"
#include "setup.h"
#include "nvs_flash.h"

static const char *TAG = "aura";

// The RGB panel's interrupt (it refills the display's line buffers) runs on the
// core that creates the panel. Wi-Fi lives on core 0, so the panel is created
// from a short task on core 1; otherwise Wi-Fi interrupts delay the refills and
// the screen flickers (stage 2).
typedef struct {
    esp_lv_adapter_tear_avoid_mode_t tear_mode;
    esp_lv_adapter_rotation_t rotation;
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;
    esp_err_t err;
    TaskHandle_t caller;
} lcd_init_t;

static void lcd_init_task(void *arg)
{
    lcd_init_t *c = arg;
    c->err = waveshare_esp32_s3_rgb_lcd_init(c->tear_mode, c->rotation, &c->panel, &c->touch);
    xTaskNotifyGive(c->caller);
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nv = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nv);

    // Wi-Fi first: its buffers need internal RAM, which the display and LVGL
    // otherwise use up (stage 2 boot loop: esp_wifi_init -> ESP_ERR_NO_MEM).
    wifi_start();

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_180;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;

    lcd_init_t lcd = { .tear_mode = tear_mode, .rotation = rotation, .caller = xTaskGetCurrentTaskHandle() };
    xTaskCreatePinnedToCore(lcd_init_task, "lcd_init", 6144, &lcd, 5, NULL, 1);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_ERROR_CHECK(lcd.err);
    esp_lcd_panel_handle_t panel_handle = lcd.panel;
    esp_lcd_touch_handle_t touch_handle = lcd.touch;
    ESP_ERROR_CHECK(waveshare_rgb_lcd_backlight_on());

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.stack_in_psram = true;
    adapter_config.task_core_id = 1;          // drawing stays off the Wi-Fi core
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel_handle,
        NULL,
        EXAMPLE_LCD_H_RES,
        EXAMPLE_LCD_V_RES,
        rotation);
    disp_config.profile.use_psram = true;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != NULL);

    if (touch_handle != NULL) {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != NULL);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    hmmd_start();
    presence_start();
    sdcard_start();

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        diag_create();          // hidden until the 2 s corner hold
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "internal RAM free before frame mode: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "Starting frame mode");
    frame_start();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        setup_start();          // opens itself at first boot / after 1 h without the saved network
        esp_lv_adapter_unlock();
    }
}
