// Remote tuning from state.json + easter-egg trigger. See tuning.h.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "sdcard.h"
#include "presence.h"
#include "tuning.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "tuning";
#define STATE_PATH SD_MOUNT_POINT "/state.json"

static tuning_t tu = { .max_gate = 4, .away_ms = PRESENCE_AWAY_HOLD_MS };
static egg_status_t eg;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t egg_fired_us;     // 0 = not fired / taken

void tuning_get(tuning_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = tu;
    portEXIT_CRITICAL(&lock);
}

void egg_get_status(egg_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = eg;
    portEXIT_CRITICAL(&lock);
}

bool egg_take(char *clip, size_t len)
{
    int64_t f = egg_fired_us;
    if (!f) return false;
    egg_fired_us = 0;
    if (esp_timer_get_time() - f > 2000000) return false;     // too old: the frame was busy
    portENTER_CRITICAL(&lock);
    snprintf(clip, len, "%s", tu.egg_clip);
    portEXIT_CRITICAL(&lock);
    return clip[0] != 0;
}

// ---------------------------------------------------------------- state.json

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static void jlist(const cJSON *o, const char *k, uint32_t *out)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    for (int g = 0; g < HMMD_GATES; g++) {
        const cJSON *v = cJSON_IsArray(a) ? cJSON_GetArrayItem(a, g) : NULL;
        out[g] = cJSON_IsNumber(v) && v->valuedouble > 0 ? (uint32_t)v->valuedouble : 0;
    }
}

static void load(void)
{
    FILE *f = fopen(STATE_PATH, "rb");
    if (!f) return;
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = buf ? fread(buf, 1, 8191, f) : 0;
    fclose(f);
    if (!buf) return;
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    if (!root) return;

    tuning_t t = { .max_gate = 4, .away_ms = PRESENCE_AWAY_HOLD_MS };
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "sensor");
    if (cJSON_IsObject(s)) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(s, "mode");
        t.max_gate = jint(s, "max_gate", 4);
        if (t.max_gate < 0) t.max_gate = 0;
        if (t.max_gate >= HMMD_GATES) t.max_gate = HMMD_GATES - 1;
        jlist(s, "trigger", t.trigger);
        jlist(s, "hold", t.hold);
        bool any = false;
        for (int g = 0; g <= t.max_gate; g++) {
            if (!t.hold[g]) t.hold[g] = t.trigger[g];
            any |= t.trigger[g] != 0;
        }
        t.frame_decides = cJSON_IsString(m) && !strcmp(m->valuestring, "frame") && any;
        int away = jint(s, "away_after_s", PRESENCE_AWAY_HOLD_MS / 1000);
        t.away_ms = (uint32_t)(away < 2 ? 2 : away > 600 ? 600 : away) * 1000;
    }
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "easter_egg");
    if (cJSON_IsObject(e)) {
        t.egg_enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "enabled"));
        t.egg_max_cm = jint(e, "max_cm", 80);
        t.egg_jitter_cm = jint(e, "jitter_cm", 15);
        t.egg_still_max = (uint32_t)jint(e, "still_max", 65535);
        t.egg_hold_s = (uint32_t)jint(e, "hold_s", 30);
        t.egg_every_h = (uint32_t)jint(e, "every_h", 24);
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(e, "clip");
        if (cJSON_IsString(c) && c->valuestring[0] && !strstr(c->valuestring, ".."))
            snprintf(t.egg_clip, sizeof(t.egg_clip), SD_MOUNT_POINT "/%s", c->valuestring);
        struct stat st;
        if (t.egg_enabled && (!t.egg_clip[0] || stat(t.egg_clip, &st) != 0)) t.egg_enabled = false;  // no clip yet
    }
    cJSON_Delete(root);

    tuning_t old;
    tuning_get(&old);
    if (old.frame_decides != t.frame_decides || old.away_ms != t.away_ms || old.max_gate != t.max_gate)
        ESP_LOGI(TAG, "presence decided by the %s, gates 0-%d, away after %lu s",
                 t.frame_decides ? "frame (state.json)" : "sensor", t.max_gate, (unsigned long)(t.away_ms / 1000));
    if (old.egg_enabled != t.egg_enabled)
        ESP_LOGI(TAG, "easter egg %s", t.egg_enabled ? "armed" : "off");
    portENTER_CRITICAL(&lock);
    tu = t;
    portEXIT_CRITICAL(&lock);
}

// ---------------------------------------------------------------- egg watcher

static void tuning_task(void *arg)
{
    (void)arg;
    sd_state_t sd;
    do { vTaskDelay(pdMS_TO_TICKS(500)); sdcard_get(&sd); } while (!sd.done);
    if (!sd.mounted) vTaskDelete(NULL);

    enum { RING = 25 };                   // 5 s of distance samples at 200 ms
    int ring[RING] = {0}, ri = 0, filled = 0;
    int64_t last_load = -60000000, still_since = 0, last_fire = 0, last_log = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        int64_t now = esp_timer_get_time();
        if (now - last_load >= 30000000) { last_load = now; load(); }

        tuning_t t;
        tuning_get(&t);
        hmmd_state_t s;
        hmmd_get_state(&s);
        presence_t p;
        presence_get(&p);

        uint32_t peak = 0;
        for (int g = 0; g < HMMD_GATES; g++) if (s.energy[g] > peak) peak = s.energy[g];
        int dist = s.distance_cm;
        ring[ri] = dist; ri = (ri + 1) % RING; if (filled < RING) filled++;
        int lo = 100000, hi = 0;
        for (int i = 0; i < filled; i++) { if (ring[i] < lo) lo = ring[i]; if (ring[i] > hi) hi = ring[i]; }
        int range = filled ? hi - lo : 0;

        bool close_still = t.egg_enabled && p.sensor_ok && p.raw_present && dist > 0 &&
                           dist <= t.egg_max_cm && filled == RING && range <= t.egg_jitter_cm &&
                           peak <= t.egg_still_max;
        if (close_still) { if (!still_since) still_since = now; }
        else still_since = 0;
        int still_s = still_since ? (int)((now - still_since) / 1000000) : 0;

        int64_t gap = (int64_t)t.egg_every_h * 3600 * 1000000;
        int cooldown = last_fire && now - last_fire < gap ? (int)((gap - (now - last_fire)) / 60000000) + 1 : 0;
        if (close_still && still_s >= (int)t.egg_hold_s && !cooldown) {
            last_fire = now;
            still_since = 0;
            egg_fired_us = now;
            ESP_LOGI(TAG, "easter egg fired (dist %d cm, range %d cm, peak %lu)", dist, range, (unsigned long)peak);
        }

        // data for tuning: one line a second while someone is near
        if (t.egg_enabled && dist > 0 && dist <= t.egg_max_cm * 2 && now - last_log >= 1000000) {
            last_log = now;
            ESP_LOGI(TAG, "egg: dist %d cm, range %d cm, peak %lu, still %d/%lu s%s", dist, range,
                     (unsigned long)peak, still_s, (unsigned long)t.egg_hold_s, cooldown ? " (cooling down)" : "");
        }

        portENTER_CRITICAL(&lock);
        eg.enabled = t.egg_enabled;
        eg.still_s = still_s;
        eg.need_s = (int)t.egg_hold_s;
        eg.dist_cm = dist;
        eg.range_cm = range;
        eg.peak = peak;
        eg.cooldown_min = cooldown;
        portEXIT_CRITICAL(&lock);
    }
}

void tuning_start(void)
{
    if (xTaskCreatePinnedToCoreWithCaps(tuning_task, "tuning", 6144, NULL, 3, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE(TAG, "could not start");
}
