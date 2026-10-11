// Diagnostics screen (step 5).
// Left: presence panel, sensor settings, system info. Right: live gate energy bars.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "hmmd.h"
#include "presence.h"
#include "sdcard.h"
#include "frame.h"
#include "wifi.h"
#include "setup.h"
#include "sync.h"
#include "ota.h"
#include "diag.h"

#define COL_BG       0x101014
#define COL_PANEL    0x1C1C24
#define COL_TEXT     0xE8E8EC
#define COL_DIM      0x8A8A96
#define COL_PRESENT  0x2E9E4F
#define COL_AWAY     0x3A3A46
#define COL_BAR      0x3B82C4
#define COL_BAR_HOT  0xD9534F
#define COL_TICK     0xF2C14E

#define BAR_MAX      963      // log10(65536) * 200
#define ROW_PITCH    26
#define ROWS_Y       58
#define BAR_X        456
#define BAR_W        200
#define MAX_ROWS     10       // keep rows above the presence-logic panel

static lv_obj_t *presence_panel, *lbl_presence, *lbl_detail, *lbl_link;
static lv_obj_t *lbl_config, *lbl_sys, *lbl_gate_title;
static lv_obj_t *lbl_logic, *btn_source, *lbl_source, *lbl_sd;
static lv_obj_t *row_name[HMMD_GATES], *row_bar[HMMD_GATES], *row_tick[HMMD_GATES], *row_val[HMMD_GATES];

static int log_scale(uint32_t e)
{
    int v = (int)(log10f((float)e + 1.0f) * 200.0f);
    return v > BAR_MAX ? BAR_MAX : v;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_pad_all(p, 12, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *scr_diag;

lv_obj_t *diag_screen(void) { return scr_diag; }

static void scene_btn_cb(lv_event_t *e)
{
    (void)e;
    frame_show_scene();
}

static lv_obj_t *lbl_preview;

static void preview_btn_cb(lv_event_t *e)
{
    (void)e;
    int next = frame_preview_cycle();    // the frame task applies it within a second
    char b[32];
    if (next) snprintf(b, sizeof(b), "Preview %ds", next);
    else snprintf(b, sizeof(b), "Preview off");
    lv_label_set_text(lbl_preview, b);
}

static void test_btn_cb(lv_event_t *e)
{
    (void)e;
    frame_test_next();
}

static void diag_toggle(void)
{
    if (lv_scr_act() == scr_diag) frame_show_scene();
    else frame_show_diag();
}

static lv_obj_t *lbl_radio;

static uint32_t radio_tap_ms;

static void radio_btn_cb(lv_event_t *e)     // "Sync now", or "Update" when new firmware is waiting
{
    (void)e;
    radio_tap_ms = lv_tick_get();
    if (ota_waiting()) {
        ota_install_now();
        lv_label_set_text(lbl_radio, "Updating");
    } else {
        sync_now();
        lv_label_set_text(lbl_radio, "Syncing");
    }
}

static void wifi_btn_cb(lv_event_t *e)
{
    (void)e;
    setup_open(true);
}

// Hidden 2 s hold zone (top-left corner). The callback fires once per press.
typedef struct { uint32_t t0; bool fired; void (*cb)(void); } hold_t;

static void hold_cb(lv_event_t *e)
{
    hold_t *h = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        h->t0 = lv_tick_get();
        h->fired = false;
        break;
    case LV_EVENT_PRESSING:
        if (!h->fired && lv_tick_elaps(h->t0) >= 2000) {
            h->fired = true;
            h->cb();
        }
        break;
    default:
        break;
    }
}

void diag_corner_hold(lv_obj_t *parent, int w, int h, void (*cb)(void))
{
    hold_t *ud = calloc(1, sizeof(hold_t));
    ud->cb = cb;
    lv_obj_t *z = lv_obj_create(parent);
    lv_obj_remove_style_all(z);
    lv_obj_set_pos(z, 0, 0);
    lv_obj_set_size(z, w, h);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(z, hold_cb, LV_EVENT_PRESSED, ud);
    lv_obj_add_event_cb(z, hold_cb, LV_EVENT_PRESSING, ud);
}

static void source_btn_cb(lv_event_t *e)
{
    (void)e;
    presence_cycle_source();
}

static void update_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_diag) return;
    if (lv_disp_get_inactive_time(NULL) > 60000) {   // 60 s without a touch: back to the scene
        frame_show_scene();
        return;
    }
    hmmd_state_t s;
    hmmd_get_state(&s);
    int64_t now = esp_timer_get_time();
    char buf[256];

    // presence panel
    bool fresh = s.last_frame_us && (now - s.last_frame_us) < 2000000;
    presence_t p;
    presence_get(&p);
    lv_label_set_text(lbl_presence, p.present ? "PRESENT  -  frozen" : "AWAY  -  moving");
    lv_obj_set_style_bg_color(presence_panel, lv_color_hex(p.present ? COL_PRESENT : COL_AWAY), 0);
    static uint32_t last_frames;
    static int64_t last_rate_us;
    static float rate;
    if (now - last_rate_us >= 1000000) {
        rate = (s.frames - last_frames) * 1e6f / (float)(now - last_rate_us);
        last_frames = s.frames;
        last_rate_us = now;
    }
    snprintf(buf, sizeof(buf), "Distance  %u cm\n%.0f readings/s", s.distance_cm, rate);
    lv_label_set_text(lbl_detail, buf);

    if (s.connected && fresh) {
        lv_label_set_text(lbl_link, "Sensor: connected");
    } else if (s.connected) {
        lv_label_set_text(lbl_link, "Sensor: no data (stalled)");
    } else {
        snprintf(buf, sizeof(buf), "Sensor: waiting (retry %d)", s.retries);
        lv_label_set_text(lbl_link, buf);
    }

    // settings
    if (s.config_valid) {
        snprintf(buf, sizeof(buf),
                 "Firmware   %s\nGates      %lu - %lu  (0 - %.1f m)\nAway delay %lu s",
                 s.firmware, (unsigned long)s.min_gate, (unsigned long)s.max_gate,
                 (s.max_gate + 1) * 0.7, (unsigned long)s.delay_s);
    } else {
        snprintf(buf, sizeof(buf), "Settings not read yet");
    }
    lv_label_set_text(lbl_config, buf);

    // presence logic panel
    lv_label_set_text(lbl_source, presence_source_name(p.source));
    lv_obj_set_style_bg_color(btn_source, lv_color_hex(p.source == PRES_SRC_REAL ? COL_BAR : COL_TICK), 0);
    lv_obj_set_style_text_color(lbl_source, lv_color_hex(p.source == PRES_SRC_REAL ? COL_TEXT : COL_BG), 0);
    if (!p.sensor_ok) {
        snprintf(buf, sizeof(buf), "Sensor says:  NO DATA (treated as present)\nChanges since boot: %lu",
                 (unsigned long)p.changes);
    } else if (p.raw_present) {
        snprintf(buf, sizeof(buf), "Sensor says:  PRESENT\nAway after %lu s clear\nChanges since boot: %lu",
                 (unsigned long)(p.hold_ms / 1000), (unsigned long)p.changes);
    } else {
        uint32_t left = p.clear_ms >= p.hold_ms ? 0 : (p.hold_ms - p.clear_ms + 999) / 1000;
        snprintf(buf, sizeof(buf), "Sensor says:  clear for %lu s\n%s\nChanges since boot: %lu",
                 (unsigned long)(p.clear_ms / 1000),
                 left ? "" : "Away hold reached",
                 (unsigned long)p.changes);
        if (left) {
            snprintf(buf, sizeof(buf), "Sensor says:  clear for %lu s\nAWAY in %lu s\nChanges since boot: %lu",
                     (unsigned long)(p.clear_ms / 1000), (unsigned long)left, (unsigned long)p.changes);
        }
    }
    lv_label_set_text(lbl_logic, buf);

    // gate rows
    int shown = s.config_valid ? (int)s.max_gate + 1 : MAX_ROWS;
    if (shown > MAX_ROWS) shown = MAX_ROWS;
    for (int g = 0; g < HMMD_GATES; g++) {
        if (g >= shown) {
            lv_obj_add_flag(row_name[g], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar[g], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_tick[g], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_val[g], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(row_name[g], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(row_bar[g], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(row_val[g], LV_OBJ_FLAG_HIDDEN);
        uint32_t e = s.energy[g];
        uint32_t trig = s.trigger[g];
        lv_bar_set_value(row_bar[g], log_scale(e), LV_ANIM_OFF);
        bool hot = s.config_valid && e >= trig;
        lv_obj_set_style_bg_color(row_bar[g], lv_color_hex(hot ? COL_BAR_HOT : COL_BAR), LV_PART_INDICATOR);
        if (s.config_valid) {
            lv_obj_clear_flag(row_tick[g], LV_OBJ_FLAG_HIDDEN);
            int x = BAR_X + (log_scale(trig) * BAR_W) / BAR_MAX;
            lv_obj_set_x(row_tick[g], x - 1);
            snprintf(buf, sizeof(buf), "%5lu / %lu", (unsigned long)e, (unsigned long)trig);
        } else {
            lv_obj_add_flag(row_tick[g], LV_OBJ_FLAG_HIDDEN);
            snprintf(buf, sizeof(buf), "%5lu", (unsigned long)e);
        }
        lv_label_set_text(row_val[g], buf);
    }

    // system line
    int64_t up = now / 1000000;
    size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    sd_state_t sd;
    sdcard_get(&sd);
    if (!sd.done) {
        lv_label_set_text(lbl_sd, "SD card: checking...");
    } else if (!sd.mounted) {
        snprintf(buf, sizeof(buf), "SD card: NOT MOUNTED\n%s", sd.error);
        lv_label_set_text(lbl_sd, buf);
    } else {
        char fs[192];
        frame_status(fs, sizeof(fs));
        char sy[96], ot[80];
        sync_status(sy, sizeof(sy));
        ota_status(ot, sizeof(ot));
        if (lbl_radio && lv_tick_elaps(radio_tap_ms) > 2000) {
            const char *cur = lv_label_get_text(lbl_radio);
            const char *want = ota_waiting() ? LV_SYMBOL_DOWNLOAD " Update" : LV_SYMBOL_REFRESH " Sync";
            bool hold = (!strcmp(cur, "Syncing") && sync_busy()) ||
                        (!strcmp(cur, "Updating") && !strstr(ot, "failed") && !strstr(ot, "memory"));
            if (!hold && strcmp(cur, want)) lv_label_set_text(lbl_radio, want);
        }
        if (ot[0]) snprintf(buf, sizeof(buf), "%s\n%s\n%s", sy, ot, fs);
        else snprintf(buf, sizeof(buf), "%s\n%s", sy, fs);
        lv_label_set_text(lbl_sd, buf);
    }

    wifi_status_t w;
    wifi_get_status(&w);
    char wl[64];
    if (w.radio_off)
        snprintf(wl, sizeof(wl), "Wi-Fi OFF (test)");
    else if (w.state == WIFI_ST_CONNECTED)
        snprintf(wl, sizeof(wl), "Wi-Fi %s %d dBm %s%s", w.ssid, w.rssi, w.ip, w.time_ok ? "" : " (no time yet)");
    else if (!w.has_creds)
        snprintf(wl, sizeof(wl), "Wi-Fi not set up");
    else
        snprintf(wl, sizeof(wl), "Wi-Fi %s missing %lu min", w.ssid, (unsigned long)(w.missing_s / 60));
    snprintf(buf, sizeof(buf), "fw %d  Up %02d:%02d:%02d  RAM %uK  PSRAM %.1fM   %s", ota_running_build(),
             (int)(up / 3600), (int)(up / 60 % 60), (int)(up % 60),
             (unsigned)(heap / 1024), psram / 1048576.0, wl);
    lv_label_set_text(lbl_sys, buf);
}

void diag_create(void)
{
    scr_diag = lv_obj_create(NULL);
    lv_obj_t *scr = scr_diag;
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(scr, &lv_font_montserrat_20, COL_DIM);
    lv_label_set_text(title, "Diagnostics");
    lv_obj_set_pos(title, 16, 14);

    lv_obj_t *btn_scene = lv_btn_create(scr);
    lv_obj_set_pos(btn_scene, 226, 8);
    lv_obj_set_size(btn_scene, 90, 34);
    lv_obj_set_style_bg_color(btn_scene, lv_color_hex(COL_BAR), 0);
    lv_obj_add_event_cb(btn_scene, scene_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_scene = make_label(btn_scene, &lv_font_montserrat_16, COL_TEXT);
    lv_label_set_text(lbl_scene, LV_SYMBOL_PLAY " Scene");
    lv_obj_center(lbl_scene);

    lv_obj_t *btn_test = lv_btn_create(scr);
    lv_obj_set_pos(btn_test, 130, 8);
    lv_obj_set_size(btn_test, 86, 34);
    lv_obj_set_style_bg_color(btn_test, lv_color_hex(COL_PANEL), 0);
    lv_obj_add_event_cb(btn_test, test_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_test = make_label(btn_test, &lv_font_montserrat_16, COL_TEXT);
    lv_label_set_text(lbl_test, "Next " LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_test);

    // presence panel
    presence_panel = make_panel(scr, 16, 50, 300, 170, COL_AWAY);
    lbl_presence = make_label(presence_panel, &lv_font_montserrat_24, COL_TEXT);
    lv_obj_set_pos(lbl_presence, 0, 0);
    lbl_detail = make_label(presence_panel, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_pos(lbl_detail, 0, 44);
    lbl_link = make_label(presence_panel, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_pos(lbl_link, 0, 116);

    // settings panel
    lv_obj_t *cfg = make_panel(scr, 16, 228, 300, 112, COL_PANEL);
    lv_obj_t *cfg_title = make_label(cfg, &lv_font_montserrat_16, COL_DIM);
    lv_label_set_text(cfg_title, "Sensor settings");
    lbl_config = make_label(cfg, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_pos(lbl_config, 0, 28);

    // gate rows (right side)
    lv_obj_t *btn_prev = lv_btn_create(scr);
    lv_obj_set_pos(btn_prev, 336, 8);
    lv_obj_set_size(btn_prev, 124, 34);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(COL_PANEL), 0);
    lv_obj_add_event_cb(btn_prev, preview_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl_preview = make_label(btn_prev, &lv_font_montserrat_16, COL_TEXT);
    lv_label_set_text(lbl_preview, "Preview off");
    lv_obj_center(lbl_preview);

    lbl_gate_title = make_label(scr, &lv_font_montserrat_16, COL_DIM);
    lv_label_set_text(lbl_gate_title, "Gate energy (yellow = trigger)");
    lv_obj_set_pos(lbl_gate_title, 474, 17);
    for (int g = 0; g < HMMD_GATES; g++) {
        int y = ROWS_Y + g * ROW_PITCH;
        char name[24];
        snprintf(name, sizeof(name), "G%-2d %.1f-%.1fm", g, g * 0.7, (g + 1) * 0.7);
        row_name[g] = make_label(scr, &lv_font_montserrat_14, COL_TEXT);
        lv_label_set_text(row_name[g], name);
        lv_obj_set_pos(row_name[g], 336, y);

        row_bar[g] = lv_bar_create(scr);
        lv_obj_set_pos(row_bar[g], BAR_X, y + 2);
        lv_obj_set_size(row_bar[g], BAR_W, 16);
        lv_bar_set_range(row_bar[g], 0, BAR_MAX);
        lv_obj_set_style_bg_color(row_bar[g], lv_color_hex(COL_PANEL), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row_bar[g], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row_bar[g], 3, LV_PART_MAIN);
        lv_obj_set_style_radius(row_bar[g], 3, LV_PART_INDICATOR);
        lv_obj_set_style_anim_time(row_bar[g], 0, 0);

        row_tick[g] = lv_obj_create(scr);
        lv_obj_set_size(row_tick[g], 3, 22);
        lv_obj_set_pos(row_tick[g], BAR_X, y - 1);
        lv_obj_set_style_bg_color(row_tick[g], lv_color_hex(COL_TICK), 0);
        lv_obj_set_style_bg_opa(row_tick[g], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row_tick[g], 0, 0);
        lv_obj_set_style_radius(row_tick[g], 0, 0);
        lv_obj_set_style_pad_all(row_tick[g], 0, 0);
        lv_obj_clear_flag(row_tick[g], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row_tick[g], LV_OBJ_FLAG_HIDDEN);

        row_val[g] = make_label(scr, &lv_font_montserrat_14, COL_DIM);
        lv_obj_set_pos(row_val[g], 666, y);
    }

    // presence logic panel (right, below the gate rows)
    lv_obj_t *logic = make_panel(scr, 336, 322, 448, 116, COL_PANEL);
    btn_source = lv_btn_create(logic);
    lv_obj_set_pos(btn_source, 0, 0);
    lv_obj_set_size(btn_source, 170, 92);
    lv_obj_set_style_radius(btn_source, 8, 0);
    lv_obj_add_event_cb(btn_source, source_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl_source = make_label(btn_source, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_center(lbl_source);
    lv_obj_t *hint = make_label(logic, &lv_font_montserrat_12, COL_DIM);
    lv_label_set_text(hint, "tap to change source");
    lv_obj_set_pos(hint, 186, 0);
    lbl_logic = make_label(logic, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_pos(lbl_logic, 186, 20);

    // SD card panel (left, under the settings)
    lv_obj_t *sdp = make_panel(scr, 16, 348, 300, 92, COL_PANEL);
    lv_obj_set_style_pad_all(sdp, 8, 0);
    lbl_sd = make_label(sdp, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(lbl_sd, 284);
    lv_label_set_long_mode(lbl_sd, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(lbl_sd, 76);

    // system line
    lbl_sys = make_label(scr, &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_pos(lbl_sys, 16, 448);

    lv_obj_t *btn_wifi = lv_btn_create(scr);
    lv_obj_set_pos(btn_wifi, 690, 442);
    lv_obj_set_size(btn_wifi, 94, 34);
    lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(COL_PANEL), 0);
    lv_obj_add_event_cb(btn_wifi, wifi_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_wifi = make_label(btn_wifi, &lv_font_montserrat_16, COL_TEXT);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI " Wi-Fi");
    lv_obj_center(lbl_wifi);

    lv_obj_t *btn_radio = lv_btn_create(scr);
    lv_obj_set_pos(btn_radio, 586, 442);
    lv_obj_set_size(btn_radio, 96, 34);
    lv_obj_set_style_bg_color(btn_radio, lv_color_hex(COL_PANEL), 0);
    lv_obj_add_event_cb(btn_radio, radio_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl_radio = make_label(btn_radio, &lv_font_montserrat_16, COL_TEXT);
    lv_label_set_text(lbl_radio, LV_SYMBOL_REFRESH " Sync");
    lv_obj_center(lbl_radio);

    diag_corner_hold(scr, 120, 46, diag_toggle);   // same gesture leaves

    lv_timer_create(update_cb, 200, NULL);
}
