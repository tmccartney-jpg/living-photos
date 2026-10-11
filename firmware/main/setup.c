// Touchscreen Wi-Fi setup (stage 2). See setup.h and docs/Provisioning.md.
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_log.h"
#include "wifi.h"
#include "frame.h"
#include "setup.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "setup";

#define COL_BG      0x101014
#define COL_PANEL   0x1C1C24
#define COL_TEXT    0xE8E8EC
#define COL_DIM     0x8A8A96
#define COL_ACCENT  0x3B82C4
#define COL_BAD     0xE07A6A
#define COL_GOOD    0x5FBF7F
#define COL_KEY      0x24242E   // keyboard keys
#define COL_KEY_CTRL 0x34343F   // shift, backspace, OK ...
#define COL_KEY_DN   0x44445A   // pressed

#define IDLE_CLOSE_MS   (5 * 60 * 1000)
#define RESCAN_MS       30000

typedef enum { V_LIST, V_PASSWORD, V_CONNECTING, V_DONE } view_t;

static lv_obj_t *scr, *lbl_status, *btn_refresh, *btn_back, *list;
static lv_obj_t *pw_row, *lbl_pw, *ta, *btn_show, *lbl_show, *kb;
static bool is_open, is_manual;
static view_t view;
static uint32_t shown_gen = UINT32_MAX, view_t0, last_scan_t;
static wifi_ap_t shown[WIFI_MAX_APS];
static int n_shown;
static char sel_ssid[33];

static void status(const char *text, uint32_t color)
{
    lv_label_set_text(lbl_status, text);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(color), 0);
}

static void set_view(view_t v)
{
    view = v;
    view_t0 = lv_tick_get();
    bool list_v = v == V_LIST;
    bool pw_v = v == V_PASSWORD;
    if (list_v) lv_obj_clear_flag(list, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    if (list_v) lv_obj_clear_flag(btn_refresh, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(btn_refresh, LV_OBJ_FLAG_HIDDEN);
    if (pw_v) lv_obj_clear_flag(btn_back, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(btn_back, LV_OBJ_FLAG_HIDDEN);
    if (pw_v) lv_obj_clear_flag(pw_row, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pw_row, LV_OBJ_FLAG_HIDDEN);
    if (pw_v) lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void close_setup(void)
{
    if (!is_open) return;
    is_open = false;
    frame_show_scene();
    ESP_LOGI(TAG, "closed");
}

static void not_now(void)
{
    wifi_snooze();
    close_setup();
}

static void scan_now(void)
{
    wifi_scan();
    last_scan_t = lv_tick_get();
}

// ---------------------------------------------------------------- list

static const char *strength(int rssi)
{
    return rssi >= -60 ? "Strong" : rssi >= -72 ? "Good" : "Weak";
}

static void start_try(const char *pass)
{
    char b[80];
    wifi_try(sel_ssid, pass);
    set_view(V_CONNECTING);
    snprintf(b, sizeof(b), "Connecting to %s ...", sel_ssid);
    status(b, COL_TEXT);
}

static void net_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= n_shown) return;
    bool same = !strcmp(sel_ssid, shown[i].ssid);
    snprintf(sel_ssid, sizeof(sel_ssid), "%s", shown[i].ssid);
    if (!shown[i].secure) {
        start_try("");
        return;
    }
    char b[80];
    snprintf(b, sizeof(b), "Password for %s", sel_ssid);
    lv_label_set_text(lbl_pw, b);
    if (!same) lv_textarea_set_text(ta, "");
    lv_textarea_set_password_mode(ta, true);
    lv_label_set_text(lbl_show, "Show");
    status("Type the password, then tap Connect", COL_DIM);
    set_view(V_PASSWORD);
}

static lv_obj_t *row_info[WIFI_MAX_APS];
static bool list_has_rows;

static void row_text(char *t, size_t n, const wifi_ap_t *ap, const wifi_status_t *ws)
{
    bool saved = ws->has_creds && !strcmp(ws->ssid, ap->ssid);
    snprintf(t, n, "%s%s%s", saved ? "Saved   " : "", strength(ap->rssi), ap->secure ? "   secured" : "   open");
}

static void list_message(const char *msg)
{
    lv_obj_clean(list);
    list_has_rows = false;
    lv_obj_t *t = lv_list_add_text(list, msg);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
}

// New scan results. The list is only rebuilt when the set of networks changes;
// otherwise rows stay put and only their signal text is updated (no redraw flash,
// no rows swapping places).
static void rebuild_list(void)
{
    wifi_status_t ws;
    wifi_get_status(&ws);
    shown_gen = ws.scan_gen;
    wifi_ap_t fresh[WIFI_MAX_APS];
    int n = wifi_scan_results(fresh, WIFI_MAX_APS);
    if (n == 0) {
        if (!list_has_rows) list_message(ws.scanning ? "Looking for networks ..." : "No networks found - tap Refresh");
        return;                                   // keep the last good list through an empty scan
    }

    bool same = list_has_rows && n == n_shown;
    for (int i = 0; same && i < n; i++) {
        bool found = false;
        for (int j = 0; j < n_shown; j++) if (!strcmp(fresh[i].ssid, shown[j].ssid)) found = true;
        same = found;
    }
    char t[48];
    if (same) {                                   // update in place, keep the order
        for (int j = 0; j < n_shown; j++)
            for (int i = 0; i < n; i++)
                if (!strcmp(fresh[i].ssid, shown[j].ssid)) {
                    shown[j].rssi = fresh[i].rssi;
                    shown[j].secure = fresh[i].secure;
                    row_text(t, sizeof(t), &shown[j], &ws);
                    if (strcmp(lv_label_get_text(row_info[j]), t)) lv_label_set_text(row_info[j], t);
                }
        return;
    }

    // set changed: keep known networks in their place, add new ones (strongest first) below
    wifi_ap_t merged[WIFI_MAX_APS];
    int m = 0;
    for (int j = 0; j < n_shown && list_has_rows; j++)
        for (int i = 0; i < n; i++)
            if (!strcmp(fresh[i].ssid, shown[j].ssid)) merged[m++] = fresh[i];
    for (int i = 0; i < n && m < WIFI_MAX_APS; i++) {
        bool known = false;
        for (int k = 0; k < m; k++) if (!strcmp(merged[k].ssid, fresh[i].ssid)) known = true;
        if (!known) merged[m++] = fresh[i];
    }
    memcpy(shown, merged, sizeof(merged));
    n_shown = m;

    lv_obj_clean(list);
    list_has_rows = true;
    for (int i = 0; i < n_shown; i++) {
        lv_obj_t *b = lv_list_add_btn(list, LV_SYMBOL_WIFI, shown[i].ssid);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(b, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C38), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, lv_color_hex(0x2C2C38), 0);
        lv_obj_set_height(b, 56);
        lv_obj_add_event_cb(b, net_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *r = lv_label_create(b);
        row_text(t, sizeof(t), &shown[i], &ws);
        lv_label_set_text(r, t);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(COL_DIM), 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, -8, 0);
        row_info[i] = r;
    }
}

// ---------------------------------------------------------------- buttons

static void refresh_cb(lv_event_t *e) { (void)e; scan_now(); status("Looking for networks ...", COL_DIM); }
static void back_cb(lv_event_t *e) { (void)e; set_view(V_LIST); status("Choose your network", COL_DIM); }
static void notnow_cb(lv_event_t *e) { (void)e; not_now(); }

static void show_cb(lv_event_t *e)
{
    (void)e;
    bool pw = lv_textarea_get_password_mode(ta);
    lv_textarea_set_password_mode(ta, !pw);
    lv_label_set_text(lbl_show, pw ? "Hide" : "Show");
}

static void connect_cb(lv_event_t *e)
{
    (void)e;
    if (view == V_PASSWORD) start_try(lv_textarea_get_text(ta));
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) connect_cb(e);
    else if (c == LV_EVENT_CANCEL) back_cb(e);
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, uint32_t color,
                        lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

// ---------------------------------------------------------------- policy timer

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!is_open) {
        if (frame_scene_visible() && wifi_setup_wanted()) setup_open(false);
        return;
    }

    wifi_status_t ws;
    wifi_get_status(&ws);
    char b[96];

    switch (view) {
    case V_CONNECTING:
        if (ws.try_state == WIFI_TRY_OK) {
            snprintf(b, sizeof(b), LV_SYMBOL_OK "  Connected to %s", sel_ssid);
            status(b, COL_GOOD);
            set_view(V_DONE);
        } else if (ws.try_state == WIFI_TRY_FAIL) {
            snprintf(b, sizeof(b), "%s  Tap Connect to try again.", ws.try_msg[0] ? ws.try_msg : "Couldn't connect.");
            status(b, COL_BAD);
            bool secure = false;
            for (int i = 0; i < n_shown; i++) if (!strcmp(shown[i].ssid, sel_ssid)) secure = shown[i].secure;
            set_view(secure ? V_PASSWORD : V_LIST);
        }
        return;
    case V_DONE:
        if (lv_tick_elaps(view_t0) > 1500) close_setup();
        return;
    default:
        break;
    }

    // saved network came back by itself
    if (!is_manual && ws.has_creds && ws.state == WIFI_ST_CONNECTED) {
        ESP_LOGI(TAG, "saved network is back");
        close_setup();
        return;
    }
    // nobody is using it: don't leave it on the wall
    if (lv_disp_get_inactive_time(NULL) > IDLE_CLOSE_MS) {
        ESP_LOGI(TAG, "untouched for 5 min");
        not_now();
        return;
    }
    if (view == V_LIST) {
        if (ws.scan_gen != shown_gen) rebuild_list();
        if (!ws.scanning && lv_tick_elaps(last_scan_t) > RESCAN_MS) scan_now();
    }
}

// ---------------------------------------------------------------- public

bool setup_is_open(void) { return is_open; }

void setup_open(bool manual)
{
    if (!scr) return;
    is_open = true;
    is_manual = manual;
    shown_gen = UINT32_MAX;
    n_shown = 0;
    list_message("Looking for networks ...");
    status("Choose your network", COL_DIM);
    set_view(V_LIST);
    scan_now();
    frame_show_other(scr);
    lv_disp_trig_activity(NULL);
    ESP_LOGI(TAG, "opened (%s)", manual ? "by hand" : "automatic");
}

void setup_start(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Connect to Wi-Fi");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_pos(title, 24, 16);

    lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, 0);
    lv_obj_set_width(lbl_status, 520);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(lbl_status, 24, 52);
    status("", COL_DIM);

    btn_refresh = button(scr, 556, 12, 116, 44, LV_SYMBOL_REFRESH " Refresh", COL_PANEL, refresh_cb, NULL);
    btn_back = button(scr, 556, 12, 116, 44, LV_SYMBOL_LEFT " Back", COL_PANEL, back_cb, NULL);
    button(scr, 680, 12, 104, 44, "Not now", COL_PANEL, notnow_cb, NULL);

    list = lv_list_create(scr);
    lv_obj_set_pos(list, 24, 86);
    lv_obj_set_size(list, 752, 380);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_width(list, 0, 0);

    // password row
    pw_row = lv_obj_create(scr);
    lv_obj_remove_style_all(pw_row);
    lv_obj_set_pos(pw_row, 0, 80);
    lv_obj_set_size(pw_row, 800, 140);
    lv_obj_clear_flag(pw_row, LV_OBJ_FLAG_SCROLLABLE);
    lbl_pw = lv_label_create(pw_row);
    lv_obj_set_style_text_font(lbl_pw, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_pw, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_pos(lbl_pw, 24, 6);
    ta = lv_textarea_create(pw_row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, 63);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    // dark password box
    lv_obj_set_style_bg_color(ta, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_KEY_DN), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_TEXT), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_pos(ta, 24, 44);
    lv_obj_set_size(ta, 500, 56);
    btn_show = button(pw_row, 536, 44, 96, 56, "Show", COL_PANEL, show_cb, &lbl_show);
    button(pw_row, 644, 44, 132, 56, "Connect", COL_ACCENT, connect_cb, NULL);

    kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, 800, 256);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
    // dark keyboard: letters on dark keys, control keys (shift, backspace, ...) one shade lighter
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_KEY), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_KEY_DN), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_KEY_CTRL), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_hex(COL_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);

    set_view(V_LIST);
    lv_timer_create(tick_cb, 1000, NULL);
}
