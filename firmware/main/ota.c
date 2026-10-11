// Firmware updates over Wi-Fi (stage 4). See ota.h.
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "driver/i2c.h"
#include "cJSON.h"
#include "frame.h"
#include "sync.h"
#include "ota.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "ota";

// CH422G output byte (see sdcard.c): backlight is bit 2; SD CS (bit 4) and USB_SEL (bit 5) stay low.
#define CH422G_I2C_PORT  0
#define CH422G_OUT_ADDR  0x38
#define CH422G_BL_ON     0x0E
#define CH422G_BL_OFF    0x0A

static int running_build;
static int bad_build;                 // a build that failed here; never offered again
static bool pending_verify;           // this boot is a new build on probation
static volatile int waiting_build;    // 0 = nothing to install
static char waiting_bin[96];
static volatile bool want_now, installing;
static int last_night_yday = -1;

static portMUX_TYPE st_lock = portMUX_INITIALIZER_UNLOCKED;
static char st_note[80];              // progress / last result

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    char b[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&st_lock);
    memcpy(st_note, b, sizeof(st_note));
    portEXIT_CRITICAL(&st_lock);
}

void ota_status(char *buf, size_t len)
{
    char n[80];
    portENTER_CRITICAL(&st_lock);
    memcpy(n, st_note, sizeof(n));
    portEXIT_CRITICAL(&st_lock);
    if (installing || n[0]) snprintf(buf, len, "%s", n);
    else if (waiting_build) snprintf(buf, len, "Firmware %d ready, installs 3-4 a.m.", waiting_build);
    else buf[0] = 0;
}

int  ota_running_build(void) { return running_build; }
bool ota_waiting(void) { return waiting_build && !installing && !pending_verify; }
void ota_install_now(void) { want_now = true; }

// ---------------------------------------------------------------- NVS

static int nvs_get(const char *key)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &v);
        nvs_close(h);
    }
    return (int)v;
}

static void nvs_put(const char *key, int v)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void backlight(bool on)
{
    uint8_t out = on ? CH422G_BL_ON : CH422G_BL_OFF;
    i2c_master_write_to_device(CH422G_I2C_PORT, CH422G_OUT_ADDR, &out, 1, pdMS_TO_TICKS(500));
}

// ---------------------------------------------------------------- check

void ota_check(const char *json_text)
{
    if (installing) return;
    int build = 0;
    char bin[96] = "";
    cJSON *root = json_text ? cJSON_Parse(json_text) : NULL;
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "build");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "bin");
    if (cJSON_IsNumber(b) && cJSON_IsString(p)) {
        build = b->valueint;
        snprintf(bin, sizeof(bin), "%s", p->valuestring);
    }
    cJSON_Delete(root);

    if (build > running_build && build != bad_build && bin[0]) {
        if (waiting_build != build) ESP_LOGI(TAG, "firmware %d available (running %d)", build, running_build);
        snprintf(waiting_bin, sizeof(waiting_bin), "%s", bin);
        waiting_build = build;
    } else {
        waiting_build = 0;
    }
}

// ---------------------------------------------------------------- install

static void install_task(void *arg)
{
    bool night = (bool)(intptr_t)arg;
    int build = waiting_build;
    if (night) backlight(false);
    note("Firmware %d: downloading ...", build);
    ESP_LOGI(TAG, "installing firmware %d from %s (%s)", build, waiting_bin, night ? "night" : "button");

    char url[192];
    snprintf(url, sizeof(url), SYNC_REPO_RAW "%s?t=%lld", waiting_bin, (long long)time(NULL));
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err == ESP_OK) {
        esp_app_desc_t d;
        err = esp_https_ota_get_img_desc(h, &d);
        if (err == ESP_OK && atoi(d.version) != build) {
            ESP_LOGW(TAG, "image says version '%s', expected %d", d.version, build);
            err = ESP_ERR_INVALID_VERSION;
        }
    }
    int last_pct = -1;
    while (err == ESP_OK) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        err = ESP_OK;
        int size = esp_https_ota_get_image_size(h);
        int got = esp_https_ota_get_image_len_read(h);
        int pct = size > 0 ? got * 100 / size : 0;
        if (pct / 5 != last_pct / 5) {
            last_pct = pct;
            note("Firmware %d: %d%%", build, pct);
        }
    }
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(h)) err = ESP_FAIL;
    if (err == ESP_OK) {
        err = esp_https_ota_finish(h);      // checks the image and makes it the next boot
        h = NULL;
    }
    if (h) esp_https_ota_abort(h);

    if (err == ESP_OK) {
        nvs_put("trying", build);
        note("Firmware %d installed, restarting", build);
        ESP_LOGI(TAG, "firmware %d installed, restarting", build);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    ESP_LOGW(TAG, "firmware %d install failed: %s", build, esp_err_to_name(err));
    note("Firmware %d failed (%s), will retry", build, esp_err_to_name(err));
    if (night) backlight(true);
    installing = false;
    vTaskDelete(NULL);
}

static bool clock_ok(struct tm *tm)
{
    time_t t = time(NULL);
    localtime_r(&t, tm);
    return tm->tm_year + 1900 >= 2025;
}

// ---------------------------------------------------------------- watcher

static void ota_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();

        // probation for a newly installed build
        if (pending_verify) {
            int64_t hb = frame_heartbeat_us();
            bool alive = hb && now - hb < 5000000;
            if (now >= (int64_t)OTA_HEALTHY_S * 1000000 && alive) {
                esp_ota_mark_app_valid_cancel_rollback();
                nvs_put("trying", 0);
                pending_verify = false;
                note("Firmware %d is running well", running_build);
                ESP_LOGI(TAG, "firmware %d kept", running_build);
            } else if (now >= (int64_t)OTA_GIVEUP_S * 1000000) {
                ESP_LOGE(TAG, "firmware %d not healthy, going back", running_build);
                nvs_put("bad", running_build);
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
            continue;
        }

        if (!waiting_build || installing) { want_now = false; continue; }
        struct tm tm;
        bool night = clock_ok(&tm) && tm.tm_hour == OTA_HOUR && tm.tm_yday != last_night_yday;
        if (!want_now && !night) continue;
        if (sync_busy()) continue;            // one download at a time
        want_now = false;
        if (night) last_night_yday = tm.tm_yday;
        installing = true;
        // internal-RAM stack: this task writes flash
        if (xTaskCreatePinnedToCore(install_task, "ota_inst", 8192, (void *)(intptr_t)night, 2, NULL, 0) != pdPASS) {
            installing = false;
            note("Firmware %d: not enough memory to install", waiting_build);
        }
    }
}

void ota_start(void)
{
    running_build = atoi(esp_app_get_description()->version);
    bad_build = nvs_get("bad");
    int trying = nvs_get("trying");

    esp_ota_img_states_t s;
    const esp_partition_t *run = esp_ota_get_running_partition();
    pending_verify = esp_ota_get_state_partition(run, &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY;

    if (trying && trying != running_build) {          // the new build didn't stick: we're back on the old one
        bad_build = trying;
        nvs_put("bad", trying);
        nvs_put("trying", 0);
        note("Firmware %d failed, back on %d", trying, running_build);
        ESP_LOGW(TAG, "firmware %d failed and was rolled back", trying);
    } else if (trying && !pending_verify) {            // e.g. USB flash of the same build
        nvs_put("trying", 0);
    }
    ESP_LOGI(TAG, "running firmware %d from %s%s (bad build %d)", running_build, run ? run->label : "?",
             pending_verify ? ", on probation" : "", bad_build);

    // internal-RAM stack: it writes flash (NVS, the firmware-slot state), which a PSRAM stack can't do
    if (xTaskCreatePinnedToCore(ota_task, "ota", 3584, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "could not start the ota task");
    }
}
