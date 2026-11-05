// Remote tuning from state.json + easter-egg trigger. See tuning.h.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "sdcard.h"
#include "presence.h"
#include "frame.h"
#include "tuning.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "tuning";
#define STATE_PATH SD_MOUNT_POINT "/state.json"

static tuning_t tu = { .max_gate = 4, .away_ms = PRESENCE_AWAY_HOLD_MS };
static egg_status_t eg;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t egg_fired_us;     // 0 = not fired / taken
static volatile bool stare_on;

bool stare_active(void) { return stare_on; }

// ---------------------------------------------------------------- quiet hours (fw 15)
// CH422G output byte (see ota.c / sdcard.c): backlight is bit 2; SD CS and USB_SEL stay low.
#define CH422G_I2C_PORT  0
#define CH422G_OUT_ADDR  0x38
#define CH422G_BL_ON     0x0E
#define CH422G_BL_OFF    0x0A
static volatile bool dark;
bool quiet_dark(void) { return dark; }

static void backlight(bool on)
{
    uint8_t out = on ? CH422G_BL_ON : CH422G_BL_OFF;
    i2c_master_write_to_device(CH422G_I2C_PORT, CH422G_OUT_ADDR, &out, 1, pdMS_TO_TICKS(500));
}

static int hhmm(const cJSON *o, const char *k, int def)     // "08:30" -> 510
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    int h, m;
    if (cJSON_IsString(v) && sscanf(v->valuestring, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 24 && m >= 0 && m < 60)
        return h * 60 + m;
    return def;
}

// true while the clock says the frame should be lit anyway
static bool office_hours(const tuning_t *t)
{
    time_t tt = time(NULL);
    struct tm lt;
    localtime_r(&tt, &lt);
    if (lt.tm_year + 1900 < 2025) return true;               // no clock yet: stay lit
    if (!(t->awake_days & (1u << lt.tm_wday))) return false;
    int m = lt.tm_hour * 60 + lt.tm_min;
    if (t->awake_from_min <= t->awake_to_min) return m >= t->awake_from_min && m < t->awake_to_min;
    return m >= t->awake_from_min || m < t->awake_to_min;     // window across midnight
}

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
    t.egg_max_cm = 80; t.egg_jitter_cm = 15; t.egg_still_max = 65535; t.egg_hold_s = 30; t.egg_every_h = 24;
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(root, "reveal");
    t.stare_mode = cJSON_IsString(rv) && !strcmp(rv->valuestring, "stare");
    t.stare_s = (uint32_t)jint(root, "stare_s", 15);
    if (t.stare_s < 3) t.stare_s = 3;
    t.stare_far_cm = jint(root, "stare_far_cm", 65);
    if (t.stare_far_cm < 30) t.stare_far_cm = 30;
    if (t.stare_far_cm > 400) t.stare_far_cm = 400;
    int grace = jint(root, "stare_grace_s", 5);
    t.stare_grace_s = (uint32_t)(grace < 1 ? 1 : grace > 60 ? 60 : grace);
    const cJSON *q = cJSON_GetObjectItemCaseSensitive(root, "quiet");
    if (cJSON_IsObject(q)) {
        t.quiet_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(q, "enabled"));
        t.awake_from_min = hhmm(q, "awake_from", 8 * 60);
        t.awake_to_min = hhmm(q, "awake_to", 18 * 60);
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(q, "days");
        t.awake_days = 0x3E;                                  // Mon-Fri
        if (cJSON_IsArray(d)) {
            t.awake_days = 0;
            const cJSON *x;
            cJSON_ArrayForEach(x, d) if (cJSON_IsNumber(x) && x->valueint >= 0 && x->valueint <= 6) t.awake_days |= 1u << x->valueint;
        }
        int lm = jint(q, "linger_min", 2);
        t.linger_s = (uint32_t)(lm < 1 ? 1 : lm > 120 ? 120 : lm) * 60;
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
    if (old.stare_mode != t.stare_mode)
        ESP_LOGI(TAG, "reveal: %s", t.stare_mode ? "stare (a %lu s look brings the scene to life)" : "hidden");
    if (old.quiet_on != t.quiet_on || old.awake_from_min != t.awake_from_min || old.awake_to_min != t.awake_to_min || old.awake_days != t.awake_days)
        ESP_LOGI(TAG, "quiet hours %s (lit %02d:%02d-%02d:%02d, days 0x%02x)", t.quiet_on ? "on" : "off",
                 t.awake_from_min / 60, t.awake_from_min % 60, t.awake_to_min / 60, t.awake_to_min % 60, t.awake_days);
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
    int64_t last_load = -60000000, still_since = 0, last_fire = 0, last_log = 0, gone_since = 0;
    int64_t seen_us = 0, last_bl = 0;
    bool egg_done = false;                // egg already fired during this still period

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

        // quiet hours: dark outside office hours unless someone was seen recently or a menu is open
        if (p.present || p.raw_present) seen_us = now;
        bool want_dark = t.quiet_on && !office_hours(&t) && !frame_overlay_open() &&
                         !(seen_us && now - seen_us < (int64_t)t.linger_s * 1000000);
        if (want_dark != dark) {
            dark = want_dark;
            backlight(!dark);
            last_bl = now;
            ESP_LOGI(TAG, "quiet hours: screen %s", dark ? "dark" : "lit");
        } else if (dark && now - last_bl > 30000000) {         // keep it dark (e.g. after a night update)
            backlight(false);
            last_bl = now;
        }

        uint32_t peak = 0;
        for (int g = 0; g < HMMD_GATES; g++) if (s.energy[g] > peak) peak = s.energy[g];
        int dist = s.distance_cm;
        ring[ri] = dist; ri = (ri + 1) % RING; if (filled < RING) filled++;
        int lo = 100000, hi = 0;
        for (int i = 0; i < filled; i++) { if (ring[i] < lo) lo = ring[i]; if (ring[i] > hi) hi = ring[i]; }
        int range = filled ? hi - lo : 0;

        bool watching = t.egg_enabled || t.stare_mode;
        bool close_still = watching && p.sensor_ok && p.raw_present && dist > 0 &&
                           dist <= t.egg_max_cm && filled == RING && range <= t.egg_jitter_cm &&
                           peak <= t.egg_still_max;
        if (close_still) { if (!still_since) still_since = now; }
        else { still_since = 0; egg_done = false; }
        int still_s = still_since ? (int)((now - still_since) / 1000000) : 0;

        int64_t gap = (int64_t)t.egg_every_h * 3600 * 1000000;
        int cooldown = last_fire && now - last_fire < gap ? (int)((gap - (now - last_fire)) / 60000000) + 1 : 0;
        if (t.egg_enabled && close_still && !egg_done && still_s >= (int)t.egg_hold_s && !cooldown) {
            last_fire = now;
            egg_done = true;
            egg_fired_us = now;
            ESP_LOGI(TAG, "easter egg fired (dist %d cm, range %d cm, peak %lu)", dist, range, (unsigned long)peak);
        }

        // stare: a long still look brings the scene to life; it goes back to sleep once the viewer
        // has stepped back past stare_far_cm (or left) for stare_grace_s
        if (!t.stare_mode) {
            stare_on = false;
        } else if (!stare_on && close_still && still_s >= (int)t.stare_s) {
            stare_on = true;
            gone_since = 0;
            ESP_LOGI(TAG, "stare: scene comes alive");
        } else if (stare_on) {
            bool near = p.raw_present && dist > 0 && dist <= t.stare_far_cm;
            if (near) gone_since = 0;
            else if (!gone_since) gone_since = now;
            else if (now - gone_since >= (int64_t)t.stare_grace_s * 1000000) {
                stare_on = false;
                ESP_LOGI(TAG, "stare: viewer stepped away (%s)", p.raw_present ? "still in the room" : "gone");
            }
        }

        // data for tuning: one line a second while someone is near
        if (watching && dist > 0 && dist <= t.egg_max_cm * 2 && now - last_log >= 1000000) {
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
        eg.stare_mode = t.stare_mode;
        eg.stare_active = stare_on;
        eg.stare_need_s = (int)t.stare_s;
        portEXIT_CRITICAL(&lock);
    }
}

void tuning_start(void)
{
    if (xTaskCreatePinnedToCoreWithCaps(tuning_task, "tuning", 6144, NULL, 3, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE(TAG, "could not start");
}
