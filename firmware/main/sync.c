// GitHub -> SD card mirror (stage 3). See sync.h.
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "sdcard.h"
#include "wifi.h"
#include "frame.h"
#include "sync.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "sync";

#define SD            SD_MOUNT_POINT "/"
#define MAX_FILES     160
#define MAX_SMALL     (512 * 1024)      // largest JSON we fetch into memory
#define CHUNK         8192

typedef struct {
    char path[96];       // repo-relative, e.g. twilight-zone/device/twz_001.lpv
    char sha[65];
    long bytes;
} entry_t;

// Files verified on the card this boot (path + sha), so we don't re-hash every 5 minutes.
static entry_t *known;
static int n_known;

static volatile bool kick;
static portMUX_TYPE st_lock = portMUX_INITIALIZER_UNLOCKED;
static char st_line[96] = "Sync: waiting for Wi-Fi";

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status(const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&st_lock);
    memcpy(st_line, b, sizeof(st_line));
    portEXIT_CRITICAL(&st_lock);
}

void sync_status(char *buf, size_t len)
{
    portENTER_CRITICAL(&st_lock);
    snprintf(buf, len, "%s", st_line);
    portEXIT_CRITICAL(&st_lock);
}

void sync_now(void) { kick = true; }

static void *pmalloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static void hex(const unsigned char *d, char *out)
{
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
    out[64] = 0;
}

// ---------------------------------------------------------------- HTTP

static esp_http_client_handle_t http_open(const char *rel, bool fresh, int *status_out, long *len_out)
{
    char url[256];
    if (fresh) snprintf(url, sizeof(url), SYNC_REPO_RAW "%s?t=%lld", rel, (long long)time(NULL));
    else snprintf(url, sizeof(url), SYNC_REPO_RAW "%s", rel);
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    long n = esp_http_client_fetch_headers(c);
    int code = esp_http_client_get_status_code(c);
    if (status_out) *status_out = code;
    if (len_out) *len_out = n;
    if (code != 200) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

static void http_done(esp_http_client_handle_t c)
{
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

// Small file into PSRAM (NUL-terminated). NULL on failure; *code gets the HTTP status.
static char *fetch_small(const char *rel, bool fresh, size_t *len_out, int *code)
{
    long clen = 0;
    esp_http_client_handle_t c = http_open(rel, fresh, code, &clen);
    if (!c) return NULL;
    size_t cap = clen > 0 ? (size_t)clen + 1 : 64 * 1024, used = 0;
    if (cap > MAX_SMALL) cap = MAX_SMALL;
    char *b = pmalloc(cap);
    while (b) {
        if (used + 1 >= cap) {
            if (cap >= MAX_SMALL) { free(b); b = NULL; break; }
            size_t ncap = cap * 2 > MAX_SMALL ? MAX_SMALL : cap * 2;
            char *nb = heap_caps_realloc_prefer(b, ncap, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
            if (!nb) { free(b); b = NULL; break; }
            b = nb; cap = ncap;
        }
        int r = esp_http_client_read(c, b + used, cap - 1 - used);
        if (r < 0) { free(b); b = NULL; break; }
        if (r == 0) break;
        used += r;
    }
    http_done(c);
    if (b) { b[used] = 0; if (len_out) *len_out = used; }
    return b;
}

// Stream a file to `tmp`, hashing as it goes. True if complete; sha_out gets the hex digest.
static bool fetch_file(const char *rel, const char *tmp, char *sha_out, long *bytes_out)
{
    int code = 0;
    esp_http_client_handle_t c = http_open(rel, false, &code, NULL);
    if (!c) {
        ESP_LOGW(TAG, "GET %s failed (HTTP %d)", rel, code);
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    uint8_t *buf = pmalloc(CHUNK);
    bool ok = f && buf;
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    long total = 0;
    while (ok) {
        int r = esp_http_client_read(c, (char *)buf, CHUNK);
        if (r < 0) { ok = false; break; }
        if (r == 0) break;
        if (fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
        mbedtls_sha256_update(&h, buf, r);
        total += r;
    }
    ok = ok && esp_http_client_is_complete_data_received(c);
    unsigned char d[32];
    mbedtls_sha256_finish(&h, d);
    mbedtls_sha256_free(&h);
    hex(d, sha_out);
    if (bytes_out) *bytes_out = total;
    if (f) fclose(f);
    free(buf);
    http_done(c);
    if (!ok) unlink(tmp);
    return ok;
}

// ---------------------------------------------------------------- card helpers

static void sd_path(char *out, size_t len, const char *rel) { snprintf(out, len, SD "%s", rel); }

static void mkdirs(const char *full)   // creates the folders above a file path
{
    char p[160];
    snprintf(p, sizeof(p), "%s", full);
    for (char *s = p + strlen(SD); *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0775);
            *s = '/';
        }
    }
}

static bool hash_file(const char *full, char *sha_out)
{
    FILE *f = fopen(full, "rb");
    if (!f) return false;
    uint8_t *buf = pmalloc(CHUNK);
    if (!buf) { fclose(f); return false; }
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    size_t r;
    while ((r = fread(buf, 1, CHUNK, f)) > 0) {
        mbedtls_sha256_update(&h, buf, r);
        taskYIELD();                      // share the card with the player
    }
    unsigned char d[32];
    mbedtls_sha256_finish(&h, d);
    mbedtls_sha256_free(&h);
    fclose(f);
    free(buf);
    hex(d, sha_out);
    return true;
}

static long file_size(const char *full)
{
    struct stat st;
    return stat(full, &st) == 0 ? (long)st.st_size : -1;
}

static bool replace(const char *tmp, const char *full)
{
    unlink(full);
    if (rename(tmp, full) != 0) {
        ESP_LOGW(TAG, "rename %s failed (%d)", full, errno);
        unlink(tmp);
        return false;
    }
    return true;
}

static int known_find(const char *path)
{
    for (int i = 0; i < n_known; i++) if (!strcmp(known[i].path, path)) return i;
    return -1;
}

static void known_set(const char *path, const char *sha)
{
    int i = known_find(path);
    if (i < 0) {
        if (n_known >= MAX_FILES) return;
        i = n_known++;
    }
    snprintf(known[i].path, sizeof(known[i].path), "%s", path);
    snprintf(known[i].sha, sizeof(known[i].sha), "%s", sha);
}

// Small text file: write only if the content differs from the card.
static bool update_small(const char *rel, const char *data, size_t len, bool *changed)
{
    char full[160], tmp[168];
    sd_path(full, sizeof(full), rel);
    *changed = false;
    FILE *f = fopen(full, "rb");
    if (f) {
        bool same = true;
        char buf[512];
        size_t off = 0, r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (off + r > len || memcmp(buf, data + off, r)) { same = false; break; }
            off += r;
        }
        fclose(f);
        if (same && off == len) return true;
    }
    mkdirs(full);
    snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    if (!ok) { unlink(tmp); return false; }
    if (!replace(tmp, full)) return false;
    *changed = true;
    ESP_LOGI(TAG, "updated %s", rel);
    return true;
}

// ---------------------------------------------------------------- one pass

static int run_pass(void)
{
    int64_t t0 = esp_timer_get_time();
    int code = 0;
    size_t len = 0;
    status("Sync: checking GitHub ...");
    char *idx_txt = fetch_small("device_index.json", true, &len, &code);
    if (!idx_txt) {
        status("Sync: GitHub unreachable (HTTP %d)", code);
        ESP_LOGW(TAG, "device_index.json: HTTP %d", code);
        return -1;
    }
    cJSON *idx = cJSON_Parse(idx_txt);
    free(idx_txt);
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(idx, "files");
    if (!cJSON_IsArray(files)) {
        cJSON_Delete(idx);
        status("Sync: bad device_index.json");
        return -1;
    }

    entry_t *want = pmalloc(sizeof(entry_t) * MAX_FILES);
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, files) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "path");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(e, "sha256");
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(e, "bytes");
        if (!want || n >= MAX_FILES || !cJSON_IsString(p) || !cJSON_IsString(s)) continue;
        snprintf(want[n].path, sizeof(want[n].path), "%s", p->valuestring);
        snprintf(want[n].sha, sizeof(want[n].sha), "%s", s->valuestring);
        want[n].bytes = cJSON_IsNumber(b) ? (long)b->valuedouble : -1;
        n++;
    }
    cJSON_Delete(idx);
    if (!want) return -1;

    int fetched = 0, deferred = 0, failed = 0, removed = 0;
    char full[160], tmp[168], sha[65];

    // 1. device files
    for (int i = 0; i < n; i++) {
        int k = known_find(want[i].path);
        if (k >= 0 && !strcmp(known[k].sha, want[i].sha)) continue;
        sd_path(full, sizeof(full), want[i].path);
        if (k < 0 && file_size(full) == want[i].bytes) {         // first look this boot: verify
            status("Sync: checking card %d/%d", i + 1, n);
            if (hash_file(full, sha) && !strcmp(sha, want[i].sha)) {
                known_set(want[i].path, sha);
                continue;
            }
        }
        if (frame_file_in_use(full)) { deferred++; continue; }
        status("Sync: downloading %s", strrchr(want[i].path, '/') ? strrchr(want[i].path, '/') + 1 : want[i].path);
        mkdirs(full);
        snprintf(tmp, sizeof(tmp), "%s.tmp", full);
        long got = 0;
        if (!fetch_file(want[i].path, tmp, sha, &got)) { failed++; continue; }
        if (strcmp(sha, want[i].sha)) {
            ESP_LOGW(TAG, "%s: hash mismatch, discarded", want[i].path);
            unlink(tmp);
            failed++;
            continue;
        }
        if (frame_file_in_use(full)) { unlink(tmp); deferred++; continue; }
        if (replace(tmp, full)) {
            known_set(want[i].path, sha);
            fetched++;
            ESP_LOGI(TAG, "downloaded %s (%ld bytes)", want[i].path, got);
        } else {
            failed++;
        }
    }

    // 2. remove files the index no longer lists (device folders only)
    for (int i = 0; i < n; i++) {
        char dir[128];
        snprintf(dir, sizeof(dir), "%s", want[i].path);
        char *slash = strrchr(dir, '/');
        if (!slash) continue;
        *slash = 0;
        bool seen = false;                       // each folder once
        for (int j = 0; j < i && !seen; j++) {
            char d2[128];
            snprintf(d2, sizeof(d2), "%s", want[j].path);
            char *s2 = strrchr(d2, '/');
            if (s2) { *s2 = 0; seen = !strcmp(d2, dir); }
        }
        if (seen || !strstr(dir, "device")) continue;
        char dfull[160];
        sd_path(dfull, sizeof(dfull), dir);
        DIR *d = opendir(dfull);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_type == DT_DIR || de->d_name[0] == '.') continue;
            char rel[160];
            snprintf(rel, sizeof(rel), "%s/%s", dir, de->d_name);
            bool listed = false;
            for (int j = 0; j < n && !listed; j++) listed = !strcmp(want[j].path, rel);
            if (listed) continue;
            snprintf(full, sizeof(full), SD "%s", rel);
            if (frame_file_in_use(full)) continue;
            if (unlink(full) == 0) {
                removed++;
                ESP_LOGI(TAG, "removed %s", rel);
            }
        }
        closedir(d);
    }

    // 3. manifests and state.json, then rotation.json last (it points at the files above)
    char themes[4][40];
    int nt = 0;
    for (int i = 0; i < n; i++) {
        char th[40];
        snprintf(th, sizeof(th), "%s", want[i].path);
        char *s = strchr(th, '/');
        if (!s) continue;
        *s = 0;
        if (!strcmp(th, "intros")) continue;
        bool dup = false;
        for (int j = 0; j < nt; j++) dup |= !strcmp(themes[j], th);
        if (!dup && nt < 4) snprintf(themes[nt++], sizeof(themes[0]), "%s", th);
    }
    free(want);
    int small_changed = 0;
    const char *extra[6];
    char mpaths[4][64];
    int ne = 0;
    for (int j = 0; j < nt; j++) {
        snprintf(mpaths[j], sizeof(mpaths[j]), "%s/manifest.json", themes[j]);
        extra[ne++] = mpaths[j];
    }
    extra[ne++] = "state.json";
    extra[ne++] = "rotation.json";
    for (int j = 0; j < ne; j++) {
        char *txt = fetch_small(extra[j], true, &len, &code);
        if (!txt) { if (code != 404) failed++; continue; }
        bool ch = false;
        if (update_small(extra[j], txt, len, &ch)) small_changed += ch;
        else failed++;
        free(txt);
    }

    int secs = (int)((esp_timer_get_time() - t0) / 1000000);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8];
    strftime(hm, sizeof(hm), "%H:%M", &tm);
    int changed = fetched + small_changed + removed;
    if (failed) status("Sync %s: %d updated, %d failed", hm, changed, failed);
    else if (deferred) status("Sync %s: %d updated, %d waiting", hm, changed, deferred);
    else status("Sync %s: up to date%s", hm, changed ? " (updated)" : "");
    ESP_LOGI(TAG, "pass done in %d s: %d downloaded, %d small files updated, %d removed, %d deferred, %d failed; "
             "internal RAM %u KB free",
             secs, fetched, small_changed, removed, deferred, failed,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return failed ? -1 : 0;
}

// ---------------------------------------------------------------- task

static void clean_tmp(const char *dir)   // leftovers from an interrupted download
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char p[200];
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        size_t l = strlen(de->d_name);
        if (de->d_type == DT_DIR) {
            if (strlen(p) < 150) clean_tmp(p);
        } else if (l > 4 && !strcmp(de->d_name + l - 4, ".tmp")) {
            unlink(p);
        }
    }
    closedir(d);
}

static bool clock_ok(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_year + 1900 >= 2025;
}

static void sync_task(void *arg)
{
    (void)arg;
    sd_state_t sd;
    do { vTaskDelay(pdMS_TO_TICKS(500)); sdcard_get(&sd); } while (!sd.done);
    if (!sd.mounted) {
        status("Sync: no SD card");
        vTaskDelete(NULL);
    }
    clean_tmp(SD_MOUNT_POINT);

    int64_t next_us = 0;          // 0 = as soon as Wi-Fi is up (+15 s)
    int64_t connected_since = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wifi_status_t w;
        wifi_get_status(&w);
        int64_t now = esp_timer_get_time();
        if (w.state != WIFI_ST_CONNECTED) {
            connected_since = 0;
            if (!w.has_creds) status("Sync: Wi-Fi not set up");
            else status("Sync: waiting for Wi-Fi");
            continue;
        }
        if (!connected_since) connected_since = now;
        bool due = kick || (next_us ? now >= next_us : now - connected_since >= 15000000LL);
        if (!due) continue;
        kick = false;

        run_pass();

        // next pass: on the next 5-minute mark (+20 s), or 5 minutes from now without a clock
        if (clock_ok()) {
            time_t t = time(NULL);
            long wait = SYNC_PERIOD_S - (long)(t % SYNC_PERIOD_S) + 20;
            if (wait < 60) wait += SYNC_PERIOD_S;
            next_us = esp_timer_get_time() + (int64_t)wait * 1000000;
        } else {
            next_us = esp_timer_get_time() + (int64_t)SYNC_PERIOD_S * 1000000;
        }
    }
}

void sync_start(void)
{
    known = heap_caps_calloc(MAX_FILES, sizeof(entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!known) {
        status("Sync: out of memory");
        return;
    }
    // PSRAM stack: internal RAM stays free for Wi-Fi, TLS buffers and the display.
    if (xTaskCreatePinnedToCoreWithCaps(sync_task, "sync", 10240, NULL, 2, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        status("Sync: could not start");
        ESP_LOGE(TAG, "could not start the sync task");
    }
}
