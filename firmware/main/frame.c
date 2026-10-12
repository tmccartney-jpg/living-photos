// Frame mode (stage 1). See frame.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "cJSON.h"
#include "sdcard.h"
#include "presence.h"
#include "diag.h"
#include "frame.h"
#include "tuning.h"

// Paths and captions are bounded on purpose; truncation is acceptable.
#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "frame";

#define SD               SD_MOUNT_POINT "/"
#define ROTATION_PATH    SD "rotation.json"
#define DEFAULT_INTRO    "intros/device/tz_intro.jpg"
#define DEFAULT_THEME    "twilight-zone"
#define INTRO_MS         4000
#define ROT_CHECK_MS     10000
#define MAX_W            800
#define MAX_H            480
#define CORNER_W         120
#define CORNER_H         120
#define CAP_BOX_H        150
#define CAPTION_COLOR    0xEEEAE0

LV_FONT_DECLARE(font_caption);

// ---------------------------------------------------------------- data

typedef struct {
    char key[64];         // "<theme>/<id>"
    char id[32];
    char theme[32];
    char clip[128];       // full SD path or ""
    char still[128];      // full SD path or ""
    char caption[192];
    bool rest;            // presence "rest": show frame 0 while someone is there
    bool once;            // motion "once": play the clip one time, then hold its last frame
} scene_t;

typedef struct {
    bool ok;
    char date[12];        // YYYY-MM-DD the file was written for
    int change_min;       // minutes after midnight (09:30 -> 570)
    char change_at[8];
    bool has_prev;
    scene_t today, prev;
    char intro[128];      // full SD path
} rotation_t;

static rotation_t rot;
static time_t rot_mtime;
static off_t rot_size;
static int64_t rot_checked_us;

static volatile int test_requests;
static int test_idx = -1;           // -1 = follow the schedule
static int test_count;
static scene_t test_scene;

static volatile bool diag_open;

static portMUX_TYPE st_lock = portMUX_INITIALIZER_UNLOCKED;
static char in_use[3][128];   // clip, still, intro currently shown (for the SD sync)

static void set_in_use(const char *clip, const char *still, const char *intro)
{
    portENTER_CRITICAL(&st_lock);
    snprintf(in_use[0], sizeof(in_use[0]), "%s", clip ? clip : "");
    snprintf(in_use[1], sizeof(in_use[1]), "%s", still ? still : "");
    snprintf(in_use[2], sizeof(in_use[2]), "%s", intro ? intro : "");
    portEXIT_CRITICAL(&st_lock);
}

bool frame_file_in_use(const char *full_path)
{
    bool r = false;
    portENTER_CRITICAL(&st_lock);
    for (int i = 0; i < 3; i++) if (in_use[i][0] && !strcmp(in_use[i], full_path)) r = true;
    portEXIT_CRITICAL(&st_lock);
    return r;
}
static char st_line1[96] = "Starting";
static char st_line2[96] = "";

// ---------------------------------------------------------------- display objects

static lv_obj_t *scr_scene, *img, *cap_box, *cap_text, *cap_shadow;
static lv_img_dsc_t dsc[2];
static uint8_t *pixbuf[2];
static int back;
static uint8_t grad_px[CAP_BOX_H * 3];
static lv_img_dsc_t grad_dsc;
static jpeg_dec_handle_t dec;
static uint8_t *jbuf;
static size_t jbuf_cap;

// ---------------------------------------------------------------- helpers

static void set_status(const char *l1, const char *l2)
{
    portENTER_CRITICAL(&st_lock);
    if (l1) snprintf(st_line1, sizeof(st_line1), "%s", l1);
    if (l2) snprintf(st_line2, sizeof(st_line2), "%s", l2);
    portEXIT_CRITICAL(&st_lock);
}

void frame_status(char *buf, size_t len)
{
    portENTER_CRITICAL(&st_lock);
    snprintf(buf, len, "%s\n%s", st_line1, st_line2);
    portEXIT_CRITICAL(&st_lock);
}

static bool exists(const char *path)
{
    struct stat st;
    return path[0] && stat(path, &st) == 0;
}

static void sd_path(char *out, size_t len, const char *rel)
{
    if (!rel || !rel[0]) { out[0] = 0; return; }
    while (*rel == '/') rel++;
    snprintf(out, len, SD "%s", rel);
}

static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    char *b = heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; if (len_out) *len_out = (size_t)n; }
    return b;
}

static cJSON *read_json(const char *path)
{
    char *txt = read_file(path, NULL);
    if (!txt) return NULL;
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    return j;
}

static const char *jstr(const cJSON *o, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool jbuf_reserve(size_t n)
{
    if (n <= jbuf_cap) return true;
    uint8_t *nb = heap_caps_realloc(jbuf, n, MALLOC_CAP_SPIRAM);
    if (!nb) return false;
    jbuf = nb;
    jbuf_cap = n;
    return true;
}

static bool clock_now(struct tm *tm)
{
    time_t t = time(NULL);
    localtime_r(&t, tm);
    return tm->tm_year + 1900 >= 2025;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

// ---------------------------------------------------------------- scene resolution

// Manifest entry for a scene id (caller deletes the returned root).
static const cJSON *manifest_scene(const char *theme, const char *id, cJSON **root)
{
    char path[96];
    snprintf(path, sizeof(path), SD "%s/manifest.json", theme);
    *root = read_json(path);
    const cJSON *scenes = cJSON_GetObjectItemCaseSensitive(*root, "scenes");
    const cJSON *s;
    cJSON_ArrayForEach(s, scenes) {
        const char *sid = jstr(s, "id");
        if (sid && !strcmp(sid, id)) return s;
    }
    return NULL;
}

// Fill clip/still paths from a "device" object whose paths are relative to `base`
// ("" for repo-relative paths from rotation.json, "<theme>/" for manifest paths).
static void device_paths(scene_t *sc, const cJSON *device, const char *base)
{
    char rel[128];
    const char *c = jstr(device, "clip"), *s = jstr(device, "still");
    if (c) { snprintf(rel, sizeof(rel), "%s%s", base, c); sd_path(sc->clip, sizeof(sc->clip), rel); }
    if (s) { snprintf(rel, sizeof(rel), "%s%s", base, s); sd_path(sc->still, sizeof(sc->still), rel); }
}

static void derive_paths(scene_t *sc)
{
    char p[128];
    if (!sc->clip[0]) {
        snprintf(p, sizeof(p), SD "%s/device/%s.lpv", sc->theme, sc->id);
        if (exists(p)) snprintf(sc->clip, sizeof(sc->clip), "%s", p);
    }
    if (!sc->still[0]) {
        snprintf(sc->still, sizeof(sc->still), SD "%s/device/%s.jpg", sc->theme, sc->id);
    }
}

// Scene object from rotation.json ("scene" or "previous_scene").
static bool scene_from_rotation(const cJSON *o, scene_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    if (!cJSON_IsObject(o)) return false;
    const char *key = jstr(o, "key"), *id = jstr(o, "id"), *theme = jstr(o, "theme");
    if (!id && key && strchr(key, '/')) id = strchr(key, '/') + 1;
    if (!id) return false;
    if (!theme) theme = DEFAULT_THEME;
    snprintf(sc->id, sizeof(sc->id), "%s", id);
    snprintf(sc->theme, sizeof(sc->theme), "%s", theme);
    snprintf(sc->key, sizeof(sc->key), "%s/%s", theme, id);
    const char *cap = jstr(o, "caption");
    snprintf(sc->caption, sizeof(sc->caption), "%s", cap ? cap : "");

    const cJSON *device = cJSON_GetObjectItemCaseSensitive(o, "device");
    const char *presence = jstr(o, "presence");
    const char *motion = jstr(o, "motion");
    cJSON *mroot = NULL;
    const cJSON *m = NULL;
    if (!device || !presence || !motion) m = manifest_scene(theme, id, &mroot);   // older rotation.json
    if (!presence && m) presence = jstr(m, "presence");
    if (!motion && m) motion = jstr(m, "motion");
    sc->rest = presence && !strcmp(presence, "rest");
    sc->once = motion && !strcmp(motion, "once");
    if (device) {
        device_paths(sc, device, "");
    } else if (m && cJSON_GetObjectItemCaseSensitive(m, "device")) {
        char base[40];
        snprintf(base, sizeof(base), "%s/", theme);
        device_paths(sc, cJSON_GetObjectItemCaseSensitive(m, "device"), base);
    }
    if (!device) derive_paths(sc);
    cJSON_Delete(mroot);
    return true;
}

static void load_rotation(void)
{
    cJSON *j = read_json(ROTATION_PATH);
    if (!j) {
        ESP_LOGW(TAG, "rotation.json unreadable");
        return;   // keep what we had
    }
    rotation_t r = {0};
    const char *date = jstr(j, "date");
    snprintf(r.date, sizeof(r.date), "%s", date ? date : "");
    const char *ch = jstr(j, "change_at");
    int hh = 9, mm = 30;
    if (ch) sscanf(ch, "%d:%d", &hh, &mm);
    r.change_min = hh * 60 + mm;
    snprintf(r.change_at, sizeof(r.change_at), "%02d:%02d", hh, mm);

    r.ok = scene_from_rotation(cJSON_GetObjectItemCaseSensitive(j, "scene"), &r.today);
    r.has_prev = scene_from_rotation(cJSON_GetObjectItemCaseSensitive(j, "previous_scene"), &r.prev);

    const cJSON *intro = cJSON_GetObjectItemCaseSensitive(j, "intro");
    const char *idev = jstr(intro, "device");
    char rel[128];
    if (idev) {
        snprintf(rel, sizeof(rel), "%s", idev);
    } else {
        const char *f = jstr(intro, "file");          // intros/tz_intro.png -> intros/device/tz_intro.jpg
        const char *base = f ? strrchr(f, '/') : NULL;
        base = base ? base + 1 : f;
        if (base && *base) {
            char name[64];
            snprintf(name, sizeof(name), "%s", base);
            char *dot = strrchr(name, '.');
            if (dot) *dot = 0;
            snprintf(rel, sizeof(rel), "intros/device/%s.jpg", name);
        } else {
            snprintf(rel, sizeof(rel), DEFAULT_INTRO);
        }
    }
    sd_path(r.intro, sizeof(r.intro), rel);
    if (!exists(r.intro)) sd_path(r.intro, sizeof(r.intro), DEFAULT_INTRO);
    cJSON_Delete(j);

    if (r.ok) {
        rot = r;
        ESP_LOGI(TAG, "rotation %s: today %s%s, previous %s, change %s", rot.date, rot.today.key,
                 rot.today.rest ? " (rest)" : "", rot.has_prev ? rot.prev.key : "-", rot.change_at);
    } else {
        ESP_LOGW(TAG, "rotation.json has no usable scene");
    }
}

static void check_rotation(bool force)
{
    int64_t now = esp_timer_get_time();
    if (!force && now - rot_checked_us < ROT_CHECK_MS * 1000LL) return;
    rot_checked_us = now;
    struct stat st;
    if (stat(ROTATION_PATH, &st) != 0) return;
    if (!force && st.st_mtime == rot_mtime && st.st_size == rot_size) return;
    rot_mtime = st.st_mtime;
    rot_size = st.st_size;
    load_rotation();
}

// Build scene number `idx` of today's theme from its manifest. Returns the scene count.
static int manifest_scene_at(int idx, scene_t *sc)
{
    const char *theme = rot.ok ? rot.today.theme : DEFAULT_THEME;
    char path[96];
    snprintf(path, sizeof(path), SD "%s/manifest.json", theme);
    cJSON *root = read_json(path);
    const cJSON *scenes = cJSON_GetObjectItemCaseSensitive(root, "scenes");
    int count = cJSON_GetArraySize(scenes);
    if (idx >= 0 && idx < count) {
        const cJSON *s = cJSON_GetArrayItem(scenes, idx);
        memset(sc, 0, sizeof(*sc));
        snprintf(sc->id, sizeof(sc->id), "%s", jstr(s, "id") ? jstr(s, "id") : "?");
        snprintf(sc->theme, sizeof(sc->theme), "%s", theme);
        snprintf(sc->key, sizeof(sc->key), "%s/%s", theme, sc->id);
        const cJSON *caps = cJSON_GetObjectItemCaseSensitive(s, "captions");
        const cJSON *c0 = cJSON_GetArrayItem(caps, 0);
        snprintf(sc->caption, sizeof(sc->caption), "%s", cJSON_IsString(c0) ? c0->valuestring : "");
        const char *pr = jstr(s, "presence");
        sc->rest = pr && !strcmp(pr, "rest");
        const char *mo = jstr(s, "motion");
        sc->once = mo && !strcmp(mo, "once");
        char base[40];
        snprintf(base, sizeof(base), "%s/", theme);
        device_paths(sc, cJSON_GetObjectItemCaseSensitive(s, "device"), base);
        derive_paths(sc);
    }
    cJSON_Delete(root);
    return count;
}

// Diagnostics "next scene" test: walk the manifest of today's theme, then back to the schedule.
static void handle_test_requests(void)
{
    while (test_requests > 0) {
        test_requests--;
        test_idx++;
        test_count = manifest_scene_at(test_idx, &test_scene);
        if (test_idx >= test_count) test_idx = -1;
    }
}

// Preview (diagnostics): every scene of the manifest in turn, PREVIEW_SECS each, scenes move
// regardless of presence; the intro plays at the start of each round. Off again after a restart.
static const int preview_steps[] = { 0, 10, 20, 30 };
static volatile int preview_want;   // set by the diagnostics button, applied by the frame task
static int preview_secs;            // 0 = off
static int preview_idx;
static int preview_count;
static int64_t preview_scene_us;    // when the current preview scene started
static bool preview_intro;          // play the intro before the next scene
static scene_t preview_scene;

static void preview_load(int idx)
{
    preview_idx = idx;
    preview_count = manifest_scene_at(idx, &preview_scene);
    if (preview_idx >= preview_count) {        // wrapped: start the next round with the intro
        preview_idx = 0;
        preview_count = manifest_scene_at(0, &preview_scene);
    }
    if (preview_idx == 0) preview_intro = true;
    preview_scene_us = 0;                       // set when it actually starts showing
}

static void handle_preview(void)
{
    int want = preview_want;
    if (want != preview_secs) {
        bool starting = !preview_secs;
        preview_secs = want;
        if (preview_secs && starting) preview_load(0);
        ESP_LOGI(TAG, "preview %d s", preview_secs);
    }
    if (preview_secs && preview_scene_us &&
        esp_timer_get_time() - preview_scene_us >= (int64_t)preview_secs * 1000000) {
        preview_load(preview_idx + 1);
    }
}

// Which scene should be on screen now.
static const scene_t *pick(bool *before_change)
{
    if (before_change) *before_change = false;
    if (preview_secs && preview_count > 0) return &preview_scene;
    if (test_idx >= 0) return &test_scene;
    if (!rot.ok) return NULL;
    struct tm tm;
    if (rot.has_prev && clock_now(&tm)) {
        char today[12];
        strftime(today, sizeof(today), "%Y-%m-%d", &tm);
        if (!strcmp(today, rot.date) && tm.tm_hour * 60 + tm.tm_min < rot.change_min) {
            if (before_change) *before_change = true;
            return &rot.prev;
        }
    }
    return &rot.today;
}

static void clock_line(char *buf, size_t len)
{
    struct tm tm;
    if (!clock_now(&tm)) {
        snprintf(buf, len, "Clock not set (shows today)");
        return;
    }
    char t[32];
    strftime(t, sizeof(t), "%a %H:%M %Z", &tm);
    snprintf(buf, len, "%s  rot %s @%s", t, rot.ok ? rot.date : "-", rot.ok ? rot.change_at : "-");
}

// ---------------------------------------------------------------- drawing

static void build_screen(void)
{
    scr_scene = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_scene, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr_scene, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_scene, LV_OBJ_FLAG_SCROLLABLE);

    img = lv_img_create(scr_scene);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    // caption: dark gradient strip (1 px wide image, tiled across) + shadow + text
    for (int y = 0; y < CAP_BOX_H; y++) {
        float t = (float)y / (CAP_BOX_H - 1);
        grad_px[y * 3 + 0] = 0;
        grad_px[y * 3 + 1] = 0;
        grad_px[y * 3 + 2] = (uint8_t)(200.0f * t * t);
    }
    grad_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    grad_dsc.header.w = 1;
    grad_dsc.header.h = CAP_BOX_H;
    grad_dsc.data_size = sizeof(grad_px);
    grad_dsc.data = grad_px;

    cap_box = lv_img_create(scr_scene);
    lv_img_set_src(cap_box, &grad_dsc);
    lv_obj_set_size(cap_box, MAX_W, CAP_BOX_H);
    lv_obj_align(cap_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(cap_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cap_box, LV_OBJ_FLAG_HIDDEN);

    cap_shadow = lv_label_create(cap_box);
    cap_text = lv_label_create(cap_box);
    lv_obj_t *labels[2] = { cap_shadow, cap_text };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_width(labels[i], 720);
        lv_label_set_long_mode(labels[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(labels[i], &font_caption, 0);
        lv_obj_set_style_text_align(labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(labels[i], 4, 0);
        lv_label_set_text(labels[i], "");
    }
    lv_obj_set_style_text_color(cap_shadow, lv_color_black(), 0);
    lv_obj_set_style_text_opa(cap_shadow, LV_OPA_80, 0);
    lv_obj_set_style_text_color(cap_text, lv_color_hex(CAPTION_COLOR), 0);
    lv_obj_align(cap_shadow, LV_ALIGN_BOTTOM_MID, 2, -16);
    lv_obj_align(cap_text, LV_ALIGN_BOTTOM_MID, 0, -18);

    diag_corner_hold(scr_scene, CORNER_W, CORNER_H, frame_show_diag);
    lv_scr_load(scr_scene);
}

static void fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

// Motion unlock (state.json "intro_motion"): until then nothing on screen may move
// except the presence-gated scene itself, so captions appear instantly.
static bool motion_unlocked(void)
{
    cJSON *st = read_json(SD "state.json");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(st, "intro_motion");
    bool on = cJSON_IsTrue(v);
    cJSON_Delete(st);
    return on;
}

// Scene motion switch (state.json "scene_motion"): false or missing = every scene holds
// still whatever the sensor says (keeps the secret the first week); true = normal.
// Re-read from the card every 10 s; flipped on GitHub, it arrives with the next sync.
static bool scene_motion_on(void)
{
    static bool on;
    static int64_t last_us = -20000000;
    int64_t now = esp_timer_get_time();
    if (now - last_us >= 10000000) {
        last_us = now;
        cJSON *st = read_json(SD "state.json");
        bool v = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "scene_motion"));
        cJSON_Delete(st);
        if (v != on) ESP_LOGI(TAG, "scene motion %s (state.json)", v ? "ON" : "OFF - scenes hold still");
        on = v;
    }
    return on;
}

static void caption_show(const char *text)
{
    bool fade = text && text[0] && motion_unlocked();
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    if (text && text[0] && !fade) {
        lv_anim_del(cap_box, fade_cb);
        lv_label_set_text(cap_shadow, text);
        lv_label_set_text(cap_text, text);
        lv_obj_set_style_opa(cap_box, LV_OPA_COVER, 0);
        lv_obj_clear_flag(cap_box, LV_OBJ_FLAG_HIDDEN);
    } else if (fade) {
        lv_label_set_text(cap_shadow, text);
        lv_label_set_text(cap_text, text);
        lv_obj_set_style_opa(cap_box, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(cap_box, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, cap_box);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, 600);
        lv_anim_set_delay(&a, 400);
        lv_anim_set_exec_cb(&a, fade_cb);
        lv_anim_start(&a);
    } else {
        lv_anim_del(cap_box, fade_cb);
        lv_obj_add_flag(cap_box, LV_OBJ_FLAG_HIDDEN);
    }
    esp_lv_adapter_unlock();
}

static void blank(void)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        esp_lv_adapter_unlock();
    }
}

// Playback timing, summed per clip frame and logged every 10 s (see play_scene).
static int64_t t_read_us, t_dec_us, t_lock_us;
static uint32_t t_frames, t_late;

// Decode one baseline JPEG into the back buffer and put it on screen.
static bool show_jpeg(const uint8_t *data, size_t len)
{
    int64_t ta = esp_timer_get_time();
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)data, .inbuf_len = (int)len, .outbuf = pixbuf[back] };
    jpeg_dec_header_info_t info;
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) return false;
    if (info.width > MAX_W || info.height > MAX_H) {
        ESP_LOGW(TAG, "image %dx%d too large", info.width, info.height);
        return false;
    }
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) return false;
    int64_t tb = esp_timer_get_time();
    t_dec_us += tb - ta;
    lv_img_dsc_t *d = &dsc[back];
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.w = info.width;
    d->header.h = info.height;
    d->data_size = (uint32_t)info.width * info.height * 2;
    d->data = pixbuf[back];
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        t_lock_us += esp_timer_get_time() - tb;
        lv_img_cache_invalidate_src(d);
        lv_img_set_src(img, d);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
        esp_lv_adapter_unlock();
    }
    back ^= 1;
    return true;
}

static bool show_jpeg_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && jbuf_reserve((size_t)n + 16) && fread(jbuf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (ok) ok = show_jpeg(jbuf, (size_t)n);
    if (!ok) ESP_LOGW(TAG, "could not show %s", path);
    return ok;
}

// ---------------------------------------------------------------- clip

typedef struct {
    FILE *f;
    int fps;
    uint32_t nframes, maxjpg, frame;
} clip_t;

static bool clip_open(clip_t *c, const char *path)
{
    memset(c, 0, sizeof(*c));
    c->f = fopen(path, "rb");
    if (!c->f) return false;
    uint8_t hdr[32];
    if (fread(hdr, 1, 32, c->f) != 32 || memcmp(hdr, "LPV1", 4)) {
        ESP_LOGW(TAG, "%s is not LPV1", path);
        fclose(c->f);
        c->f = NULL;
        return false;
    }
    c->fps = rd16(hdr + 8);
    if (c->fps < 1 || c->fps > 60) c->fps = 15;
    c->nframes = rd32(hdr + 12);
    c->maxjpg = rd32(hdr + 16);
    if (!jbuf_reserve(c->maxjpg + 16)) {
        fclose(c->f);
        c->f = NULL;
        return false;
    }
    setvbuf(c->f, NULL, _IOFBF, 32 * 1024);
    ESP_LOGI(TAG, "clip %s: %d fps, %lu frames", path, c->fps, (unsigned long)c->nframes);
    return true;
}

static void clip_close(clip_t *c)
{
    if (c->f) fclose(c->f);
    c->f = NULL;
}

static void clip_rewind(clip_t *c)
{
    fseek(c->f, 32, SEEK_SET);
    c->frame = 0;
}

// Show the next frame (looping). False only if the file is unusable.
static bool clip_next(clip_t *c)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        uint8_t lenb[4];
        int64_t tr = esp_timer_get_time();
        if (c->frame >= c->nframes || fread(lenb, 1, 4, c->f) != 4) {
            clip_rewind(c);
            continue;
        }
        uint32_t len = rd32(lenb);
        if (len == 0 || len > c->maxjpg || fread(jbuf, 1, len, c->f) != len) {
            ESP_LOGW(TAG, "bad frame %lu, restarting clip", (unsigned long)c->frame);
            clip_rewind(c);
            continue;
        }
        c->frame++;
        t_read_us += esp_timer_get_time() - tr;
        t_frames++;
        if (show_jpeg(jbuf, len)) return true;
        ESP_LOGW(TAG, "decode error on frame %lu", (unsigned long)c->frame);
        return true;   // skip a bad frame, keep going
    }
    return false;
}

// ---------------------------------------------------------------- playback

static bool same_scene(const scene_t *a, const scene_t *b)
{
    return !strcmp(a->key, b->key) && !strcmp(a->caption, b->caption) && a->rest == b->rest && a->once == b->once &&
           !strcmp(a->clip, b->clip) && !strcmp(a->still, b->still);
}

// Re-evaluate the schedule. True if a different scene should now be shown.
static bool schedule_changed(const scene_t *cur)
{
    check_rotation(false);
    handle_test_requests();
    handle_preview();
    const scene_t *want = pick(NULL);
    return want && !same_scene(want, cur);
}

static void play_intro(const char *path)
{
    set_in_use(NULL, NULL, path);
    caption_show(NULL);
    set_status("Intro", NULL);
    if (!path[0] || !show_jpeg_file(path)) {
        blank();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(INTRO_MS));
}

static volatile int64_t hb_us;      // frame task heartbeat (OTA health check)
int64_t frame_heartbeat_us(void) { return hb_us; }

// Easter egg: play `path` once from start to end over the current scene (caption hidden).
static void play_egg(const scene_t *sc, const char *path)
{
    clip_t e;
    if (!clip_open(&e, path)) { ESP_LOGW(TAG, "easter egg clip %s missing", path); return; }
    ESP_LOGI(TAG, "easter egg: playing %s", path);
    set_in_use(sc->clip, sc->still, path);
    caption_show(NULL);
    TickType_t period = pdMS_TO_TICKS(1000 / e.fps), wake = xTaskGetTickCount();
    while (e.frame < e.nframes && clip_next(&e)) {
        hb_us = esp_timer_get_time();
        vTaskDelayUntil(&wake, period);
    }
    clip_close(&e);
    set_in_use(sc->clip, sc->still, NULL);
}

// Plays `sc` until the schedule picks something else.
static void play_scene(const scene_t *sc)
{
    char l1[96], l2[96];
    clip_t clip = {0};
    set_in_use(sc->clip, sc->still, NULL);
    bool is_clip = sc->clip[0] && clip_open(&clip, sc->clip);
    bool have_frame = false, at_rest = false, still_ok = false;
    const char *mode = is_clip ? (sc->rest ? "clip-rest" : sc->once ? "clip-once" : "clip-freeze") : "still";
    char test[16] = "";
    if (preview_secs) snprintf(test, sizeof(test), "  PREVIEW %ds", preview_secs);
    else if (!scene_motion_on()) snprintf(test, sizeof(test), "  MOTION OFF");
    else if (test_idx >= 0) snprintf(test, sizeof(test), "  TEST");

    if (!is_clip) {
        still_ok = show_jpeg_file(sc->still);
        if (!still_ok) blank();
    }
    caption_show(sc->caption);

    TickType_t period = pdMS_TO_TICKS(is_clip ? 1000 / clip.fps : 200);
    TickType_t wake = xTaskGetTickCount();
    int64_t t0 = esp_timer_get_time();
    uint32_t frames = 0;

    while (1) {
        hb_us = esp_timer_get_time();
        char egg[96];
        if (!diag_open && !preview_secs && scene_motion_on() && egg_take(egg, sizeof(egg))) {
            play_egg(sc, egg);
            if (is_clip) have_frame = false, at_rest = false;   // redraw the scene's frame next
            else if (still_ok) show_jpeg_file(sc->still);
            caption_show(sc->caption);
            wake = xTaskGetTickCount();
        }
        if (!diag_open) {
            presence_t p;
            presence_get(&p);
            if (!scene_motion_on()) p.present = true; // motion switched off: behave as if watched
            else if (stare_active()) p.present = false; // reveal "stare": someone stared it to life
            if (preview_secs) p.present = false;      // preview: always moving
            if (is_clip) {
                if (!p.present) {
                    at_rest = false;
                    if (sc->once && clip.frame >= clip.nframes) {
                        // played through once: hold the last frame for the rest of the day
                    } else if (clip_next(&clip)) { have_frame = true; frames++; }
                } else if (sc->rest) {
                    if (!at_rest) {   // lights off: frame 0
                        clip_rewind(&clip);
                        if (clip_next(&clip)) { have_frame = true; frames++; }
                        at_rest = true;
                    }
                } else if (!have_frame) {
                    if (clip_next(&clip)) { have_frame = true; frames++; }
                }
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            float fps = frames * 1e6f / (float)(now - t0);
            if (preview_secs) snprintf(test, sizeof(test), "  PREVIEW %ds", preview_secs);
            else if (test_idx >= 0) snprintf(test, sizeof(test), "  TEST");
            else if (!scene_motion_on()) snprintf(test, sizeof(test), "  MOTION OFF");
            else test[0] = 0;
            if (is_clip) {
                snprintf(l1, sizeof(l1), "%s  %s  %.1f fps  %lu/%lu%s", sc->id, mode, fps,
                         (unsigned long)clip.frame, (unsigned long)clip.nframes, test);
            } else {
                snprintf(l1, sizeof(l1), "%s  still%s%s", sc->id, still_ok ? "" : "  MISSING", test);
            }
            clock_line(l2, sizeof(l2));
            set_status(l1, l2);
            t0 = now;
            frames = 0;
            static int64_t last_mem_us;
            if (now - last_mem_us > 30000000) {
                last_mem_us = now;
                ESP_LOGI(TAG, "memory: internal %u KB free (lowest %u KB), PSRAM %u KB free",
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                         (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            }
            static int64_t last_perf_us;
            if (now - last_perf_us >= 10000000) {
                if (t_frames) {
                    ESP_LOGI(TAG, "playback: %.1f fps target %d, per frame: read %d ms, decode %d ms, "
                             "wait for screen %d ms; %lu late",
                             t_frames * 1e6f / (float)(now - last_perf_us), is_clip ? clip.fps : 0,
                             (int)(t_read_us / t_frames / 1000), (int)(t_dec_us / t_frames / 1000),
                             (int)(t_lock_us / t_frames / 1000), (unsigned long)t_late);
                }
                last_perf_us = now;
                t_read_us = t_dec_us = t_lock_us = 0;
                t_frames = t_late = 0;
            }
            if (schedule_changed(sc)) break;
        }

        vTaskDelayUntil(&wake, diag_open ? pdMS_TO_TICKS(100) : period);
        if ((int32_t)(xTaskGetTickCount() - wake) > (int32_t)period) { wake = xTaskGetTickCount(); t_late++; }
    }
    clip_close(&clip);
    set_in_use(NULL, NULL, NULL);
}

static void frame_task(void *arg)
{
    (void)arg;
    sd_state_t sd;
    do {
        hb_us = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(200));
        sdcard_get(&sd);
    } while (!sd.done);
    if (!sd.mounted) set_status("No SD card", "Insert the card and restart");

    char l2[96];
    while (1) {
        check_rotation(true);
        handle_test_requests();
        handle_preview();
        const scene_t *want = pick(NULL);
        hb_us = esp_timer_get_time();
        if (!want) {
            blank();
            caption_show(NULL);
            clock_line(l2, sizeof(l2));
            set_status(sd.mounted ? "No rotation.json on SD" : "No SD card", l2);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        scene_t cur = *want;     // copy: rotation may be reloaded while playing
        ESP_LOGI(TAG, "showing %s%s: \"%s\"", cur.key, cur.rest ? " (rest)" : "", cur.caption);
        bool in_preview = preview_secs && want == &preview_scene;
        if (!in_preview || preview_intro) play_intro(rot.ok ? rot.intro : SD DEFAULT_INTRO);
        if (in_preview) {
            preview_intro = false;
            preview_scene_us = esp_timer_get_time();
        }
        play_scene(&cur);
    }
}

// ---------------------------------------------------------------- public

void frame_show_scene(void)
{
    if (scr_scene) lv_scr_load(scr_scene);
    diag_open = false;
}

void frame_show_other(lv_obj_t *scr)
{
    if (!scr) return;
    lv_scr_load(scr);
    lv_disp_trig_activity(NULL);
    diag_open = true;          // scene hidden: stop decoding
}

bool frame_scene_visible(void) { return scr_scene && lv_scr_act() == scr_scene; }

void frame_show_diag(void)
{
    lv_obj_t *d = diag_screen();
    if (!d) return;
    lv_scr_load(d);
    lv_disp_trig_activity(NULL);
    diag_open = true;
}

void frame_test_next(void) { test_requests++; }

int frame_preview_cycle(void)
{
    int i = 0;
    while (i < 3 && preview_steps[i] != preview_want) i++;
    preview_want = preview_steps[(i + 1) % 4];
    return preview_want;
}

int frame_preview_secs(void) { return preview_want; }

static void *json_malloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

void frame_start(void)
{
    // JSON parsing makes thousands of tiny allocations; keep them out of internal RAM
    // (step 1c log: internal RAM dipped to 7 KB while the manifest was parsed).
    cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&hooks);

    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);   // US Eastern
    tzset();

    size_t pix = (size_t)MAX_W * MAX_H * 2;
    pixbuf[0] = heap_caps_aligned_alloc(16, pix, MALLOC_CAP_SPIRAM);
    pixbuf[1] = heap_caps_aligned_alloc(16, pix, MALLOC_CAP_SPIRAM);
    jbuf_reserve(256 * 1024);
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;   // matches LVGL (no 16-bit swap)
    if (!pixbuf[0] || !pixbuf[1] || !jbuf || jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "out of memory for frame mode");
        set_status("Out of memory", "");
        return;
    }

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        build_screen();
        esp_lv_adapter_unlock();
    }
    // Stack in PSRAM: internal RAM is needed by Wi-Fi and the display (stage 2:
    // with Wi-Fi running, an 8 KB internal stack could not be allocated and the
    // frame stayed blank on "Starting").
    if (xTaskCreatePinnedToCoreWithCaps(frame_task, "frame", 8192, NULL, 3, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "could not start the frame task");
        set_status("Frame task failed to start", "");
    }
}
