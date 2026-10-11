// Presence decision (step 6). See presence.h for the rules.
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hmmd.h"
#include "presence.h"
#include "tuning.h"

static const char *TAG = "presence";
static presence_t ps = { .present = true, .hold_ms = PRESENCE_AWAY_HOLD_MS };
static portMUX_TYPE ps_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool source_changed;

const char *presence_source_name(pres_source_t src)
{
    switch (src) {
    case PRES_SRC_REAL:          return "Real sensor";
    case PRES_SRC_FORCE_PRESENT: return "Forced PRESENT";
    case PRES_SRC_FORCE_AWAY:    return "Forced AWAY";
    default:                     return "?";
    }
}

void presence_get(presence_t *out)
{
    portENTER_CRITICAL(&ps_lock);
    *out = ps;
    portEXIT_CRITICAL(&ps_lock);
}

void presence_cycle_source(void)
{
    portENTER_CRITICAL(&ps_lock);
    ps.source = (ps.source + 1) % PRES_SRC_COUNT;
    portEXIT_CRITICAL(&ps_lock);
    source_changed = true;
}

static void presence_task(void *arg)
{
    (void)arg;
    int64_t clear_since = 0;   // 0 = sensor currently sees someone
    bool decided = true;
    hmmd_state_t s;
    static tuning_t t;         // static: keeps it off this task's small stack
    bool prev_raw = true;

    while (1) {
        hmmd_get_state(&s);
        tuning_get(&t);
        int64_t now = esp_timer_get_time();
        bool sensor_ok = s.connected && s.last_frame_us && (now - s.last_frame_us) < 2000000;
        bool raw;
        if (!sensor_ok) {
            raw = true;                            // fail safe
        } else if (t.frame_decides) {              // state.json limits: trigger to start, hold to stay
            raw = false;
            for (int g = 0; g <= t.max_gate; g++) {
                uint32_t lim = prev_raw ? t.hold[g] : t.trigger[g];
                if (lim && s.energy[g] >= lim) { raw = true; break; }
            }
        } else {
            raw = s.present;                       // the sensor's own decision
        }
        prev_raw = raw;
        uint32_t hold_ms = t.away_ms ? t.away_ms : PRESENCE_AWAY_HOLD_MS;

        presence_t cur;
        presence_get(&cur);
        if (source_changed) {
            source_changed = false;
            clear_since = 0;
            ESP_LOGI(TAG, "source -> %s", presence_source_name(cur.source));
        }

        if (raw) {
            clear_since = 0;
        } else if (clear_since == 0) {
            clear_since = now;
        }
        uint32_t clear_ms = clear_since ? (uint32_t)((now - clear_since) / 1000) : 0;

        bool next = decided;
        switch (cur.source) {
        case PRES_SRC_FORCE_PRESENT: next = true;  break;
        case PRES_SRC_FORCE_AWAY:    next = false; break;
        default:
            if (raw) next = true;
            else if (clear_ms >= hold_ms) next = false;
            break;
        }

        portENTER_CRITICAL(&ps_lock);
        if (next != decided) ps.changes++;
        ps.present = next;
        ps.raw_present = raw;
        ps.sensor_ok = sensor_ok;
        ps.clear_ms = clear_ms;
        ps.hold_ms = hold_ms;
        ps.frame_decides = t.frame_decides;
        portEXIT_CRITICAL(&ps_lock);

        if (next != decided) {
            ESP_LOGI(TAG, "-> %s (%s)", next ? "PRESENT" : "AWAY", presence_source_name(cur.source));
            decided = next;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void presence_start(void)
{
    xTaskCreatePinnedToCore(presence_task, "presence", 3072, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "started (away after %d s clear)", PRESENCE_AWAY_HOLD_MS / 1000);
}
