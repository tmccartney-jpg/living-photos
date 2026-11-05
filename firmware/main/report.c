// Frame health reports + GitHub device-flow linking. See report.h.
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "sdcard.h"
#include "wifi.h"
#include "frame.h"
#include "sync.h"
#include "ota.h"
#include "presence.h"
#include "hmmd.h"
#include "tuning.h"
#include "report.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "report";

#define STATE_PATH    SD_MOUNT_POINT "/state.json"
#define NVS_NS        "report"
#define MAX_RESP      (192 * 1024)
#define MAX_LOG       (96 * 1024)          // a day's file larger than this starts over
#define UA            "living-photos-frame"
#define DIST_BUCKETS  60                   // 10 cm each, 0 .. 600 cm

// ---------------------------------------------------------------- shared state

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static rep_info_t info = { .state = REP_NOT_SET_UP, .msg = "Reports: not set up yet" };
static volatile bool want_link, want_cancel, want_unlink, want_now;
static char token[256];

typedef struct {
    bool enabled;
    char client_id[48];
    char repo[96];
    int every_min;
} rcfg_t;

static void set_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_msg(const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&lock);
    memcpy(info.msg, b, sizeof(info.msg));
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "%s", b);
}

static void set_state(rep_state_t s)
{
    portENTER_CRITICAL(&lock);
    info.state = s;
    if (s != REP_CODE) { info.code[0] = 0; info.expires_s = 0; }
    portEXIT_CRITICAL(&lock);
}

static rep_state_t get_state(void)
{
    portENTER_CRITICAL(&lock);
    rep_state_t s = info.state;
    portEXIT_CRITICAL(&lock);
    return s;
}

void report_get(rep_info_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = info;
    portEXIT_CRITICAL(&lock);
}

void report_link(void)   { want_cancel = false; want_link = true; }
void report_cancel(void) { want_cancel = true; }
void report_unlink(void) { want_unlink = true; }
void report_now(void)    { want_now = true; }

bool report_busy(void)
{
    rep_state_t s = get_state();
    return s == REP_ASKING || s == REP_CODE;
}

static void *pmalloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

// ---------------------------------------------------------------- config + token

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 256 * 1024) { fclose(f); return NULL; }
    char *b = pmalloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    return b;
}

static void load_cfg(rcfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->every_min = 60;
    char *txt = read_file(STATE_PATH);
    if (!txt) return;
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) return;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "reports");
    if (cJSON_IsObject(r)) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "client_id");
        const cJSON *rp = cJSON_GetObjectItemCaseSensitive(r, "repo");
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(r, "enabled");
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(r, "every_min");
        if (cJSON_IsString(id)) snprintf(c->client_id, sizeof(c->client_id), "%s", id->valuestring);
        if (cJSON_IsString(rp)) snprintf(c->repo, sizeof(c->repo), "%s", rp->valuestring);
        c->enabled = !cJSON_IsFalse(en);
        if (cJSON_IsNumber(ev)) c->every_min = ev->valueint;
    }
    cJSON_Delete(root);
    if (c->every_min < 10) c->every_min = 10;
    if (c->every_min > 1440) c->every_min = 1440;
    for (char *p = c->client_id; *p; p++)               // client ids are [A-Za-z0-9.]
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.')) {
            c->client_id[0] = 0;
            break;
        }
    if (!strchr(c->repo, '/') || strstr(c->repo, "..") || strchr(c->repo, ' ')) c->repo[0] = 0;
}

static void token_load(void)
{
    token[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(token);
    if (nvs_get_str(h, "tok", token, &n) != ESP_OK) token[0] = 0;
    nvs_close(h);
}

static void token_save(const char *t)
{
    snprintf(token, sizeof(token), "%s", t ? t : "");
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (token[0]) nvs_set_str(h, "tok", token);
    else nvs_erase_key(h, "tok");
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- HTTP

// One request. Returns the HTTP status (or -1); *resp gets a NUL-terminated PSRAM copy of the body.
static int http(const char *method, const char *url, const char *ctype, const char *accept,
                bool auth, const char *body, char **resp)
{
    *resp = NULL;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
        .user_agent = UA,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (!strcmp(method, "POST")) esp_http_client_set_method(c, HTTP_METHOD_POST);
    else if (!strcmp(method, "PUT")) esp_http_client_set_method(c, HTTP_METHOD_PUT);
    else esp_http_client_set_method(c, HTTP_METHOD_GET);
    if (accept) esp_http_client_set_header(c, "Accept", accept);
    if (ctype) esp_http_client_set_header(c, "Content-Type", ctype);
    esp_http_client_set_header(c, "X-GitHub-Api-Version", "2022-11-28");
    char *ah = NULL;
    if (auth && token[0]) {
        size_t an = strlen(token) + 16;
        ah = pmalloc(an);
        if (ah) {
            snprintf(ah, an, "Bearer %s", token);
            esp_http_client_set_header(c, "Authorization", ah);
        }
    }
    int blen = body ? (int)strlen(body) : 0;
    int code = -1;
    if (esp_http_client_open(c, blen) == ESP_OK) {
        bool sent = true;
        int off = 0;
        while (off < blen) {
            int w = esp_http_client_write(c, body + off, blen - off);
            if (w <= 0) { sent = false; break; }
            off += w;
        }
        if (sent && esp_http_client_fetch_headers(c) >= 0) {
            code = esp_http_client_get_status_code(c);
            size_t cap = 8192, used = 0;
            char *b = pmalloc(cap);
            while (b) {
                if (used + 4096 + 1 > cap) {
                    if (cap >= MAX_RESP) break;
                    char *nb = heap_caps_realloc(b, cap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (!nb) break;
                    b = nb;
                    cap *= 2;
                }
                int r = esp_http_client_read(c, b + used, (int)(cap - 1 - used));
                if (r <= 0) break;
                used += (size_t)r;
            }
            if (b) { b[used] = 0; *resp = b; }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    free(ah);
    return code;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int jnum(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static bool online(void)
{
    wifi_status_t w;
    wifi_get_status(&w);
    return w.state == WIFI_ST_CONNECTED;
}

// ---------------------------------------------------------------- linking (OAuth device flow)

static void do_link(const rcfg_t *c)
{
    if (!c->client_id[0]) { set_state(REP_FAILED); set_msg("No client_id in state.json yet"); return; }
    if (!online()) { set_state(REP_FAILED); set_msg("No Wi-Fi"); return; }
    set_state(REP_ASKING);
    set_msg("Asking GitHub for a code...");
    char body[128];
    snprintf(body, sizeof(body), "client_id=%s", c->client_id);
    char *resp = NULL;
    int code = http("POST", "https://github.com/login/device/code", "application/x-www-form-urlencoded",
                    "application/json", false, body, &resp);
    cJSON *j = resp ? cJSON_Parse(resp) : NULL;
    free(resp);
    const char *dev = j ? jstr(j, "device_code") : NULL;
    const char *usr = j ? jstr(j, "user_code") : NULL;
    if (code != 200 || !dev || !usr) {
        const char *e = j ? jstr(j, "error") : NULL;
        set_state(REP_FAILED);
        set_msg("GitHub said no (%d %s)", code, e ? e : "");
        cJSON_Delete(j);
        return;
    }
    char device_code[128];
    snprintf(device_code, sizeof(device_code), "%s", dev);
    int interval = jnum(j, "interval", 5);
    int expires = jnum(j, "expires_in", 900);
    portENTER_CRITICAL(&lock);
    info.state = REP_CODE;
    snprintf(info.code, sizeof(info.code), "%s", usr);
    snprintf(info.uri, sizeof(info.uri), "%s", jstr(j, "verification_uri") ? jstr(j, "verification_uri")
                                                                             : "https://github.com/login/device");
    info.expires_s = expires;
    portEXIT_CRITICAL(&lock);
    cJSON_Delete(j);
    set_msg("Enter the code on your phone");

    int64_t t_end = esp_timer_get_time() + (int64_t)expires * 1000000;
    if (interval < 5) interval = 5;
    while (1) {
        for (int i = 0; i < interval * 5; i++) {          // wait, but react to Cancel quickly
            vTaskDelay(pdMS_TO_TICKS(200));
            if (want_cancel) break;
        }
        int left = (int)((t_end - esp_timer_get_time()) / 1000000);
        portENTER_CRITICAL(&lock);
        info.expires_s = left > 0 ? left : 0;
        portEXIT_CRITICAL(&lock);
        if (want_cancel) { want_cancel = false; set_state(token[0] ? REP_LINKED : REP_UNLINKED); set_msg("Linking cancelled"); return; }
        if (left <= 0) { set_state(REP_FAILED); set_msg("The code ran out - tap Get code again"); return; }

        snprintf(body, sizeof(body), "client_id=%s&device_code=", c->client_id);
        char poll[320];
        snprintf(poll, sizeof(poll), "%s%s&grant_type=urn:ietf:params:oauth:grant-type:device_code", body, device_code);
        code = http("POST", "https://github.com/login/oauth/access_token", "application/x-www-form-urlencoded",
                    "application/json", false, poll, &resp);
        j = resp ? cJSON_Parse(resp) : NULL;
        free(resp);
        const char *tok = j ? jstr(j, "access_token") : NULL;
        const char *err = j ? jstr(j, "error") : NULL;
        if (tok && tok[0]) {
            bool expiring = cJSON_GetObjectItemCaseSensitive(j, "refresh_token") != NULL;
            token_save(tok);
            cJSON_Delete(j);
            set_state(REP_LINKED);
            if (expiring) set_msg("Linked - but the token expires: turn off token expiry in the App");
            else set_msg("Linked");
            want_now = true;                               // first report straight away
            return;
        }
        if (err && !strcmp(err, "authorization_pending")) { cJSON_Delete(j); continue; }
        if (err && !strcmp(err, "slow_down")) { interval += 5; cJSON_Delete(j); continue; }
        if (code < 0) { cJSON_Delete(j); continue; }      // network blip: keep waiting
        set_state(REP_FAILED);
        set_msg("Not linked (%s)", err ? err : "unexpected answer");
        cJSON_Delete(j);
        return;
    }
}

// ---------------------------------------------------------------- statistics (sampled once a second)

typedef struct {
    uint32_t n, present, raw_present, sensor_ok, stare_alive;
    uint32_t changes0;
    uint32_t dist_n;
    uint16_t dist[DIST_BUCKETS];
    uint32_t near65, near50;                 // seconds someone was within 65 / 50 cm
    int max_still;                           // longest close-and-still run seen (egg/stare clock)
    uint32_t peak_max;                       // highest gate energy while someone was there
    int rssi_min, rssi_max;
} stats_t;

static stats_t st;

static void stats_reset(void)
{
    presence_t p;
    presence_get(&p);
    memset(&st, 0, sizeof(st));
    st.changes0 = p.changes;
    st.rssi_min = 0; st.rssi_max = -200;
}

static void stats_sample(void)
{
    presence_t p;
    presence_get(&p);
    hmmd_state_t s;
    hmmd_get_state(&s);
    egg_status_t e;
    egg_get_status(&e);
    st.n++;
    if (p.present) st.present++;
    if (p.raw_present) st.raw_present++;
    if (p.sensor_ok) st.sensor_ok++;
    if (e.stare_active) st.stare_alive++;
    if (p.raw_present && s.distance_cm > 0) {
        int b = s.distance_cm / 10;
        if (b >= DIST_BUCKETS) b = DIST_BUCKETS - 1;
        if (st.dist[b] < 65535) st.dist[b]++;
        st.dist_n++;
        if (s.distance_cm <= 65) st.near65++;
        if (s.distance_cm <= 50) st.near50++;
        uint32_t peak = 0;
        for (int g = 0; g < HMMD_GATES; g++) if (s.energy[g] > peak) peak = s.energy[g];
        if (peak > st.peak_max) st.peak_max = peak;
    }
    if (e.still_s > st.max_still) st.max_still = e.still_s;
    wifi_status_t w;
    wifi_get_status(&w);
    if (w.state == WIFI_ST_CONNECTED && w.rssi) {
        if (w.rssi < st.rssi_min) st.rssi_min = w.rssi;
        if (w.rssi > st.rssi_max) st.rssi_max = w.rssi;
    }
}

static int dist_pct(int pct)                  // percentile of the distance readings, in cm
{
    if (!st.dist_n) return -1;
    uint32_t want = (st.dist_n * (uint32_t)pct + 99) / 100, acc = 0;
    if (want < 1) want = 1;
    for (int b = 0; b < DIST_BUCKETS; b++) {
        acc += st.dist[b];
        if (acc >= want) return b * 10 + 5;
    }
    return DIST_BUCKETS * 10;
}

static const char *reset_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "sleep";
    default:               return "other";
    }
}

static char *build_line(int every_min)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &lt);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", ts);
    cJSON_AddNumberToObject(o, "fw", ota_running_build());
    cJSON_AddNumberToObject(o, "up_h", (double)(esp_timer_get_time() / 360000000LL) / 10.0);
    cJSON_AddStringToObject(o, "boot", reset_name());
    cJSON_AddNumberToObject(o, "ram_k", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    cJSON_AddNumberToObject(o, "ram_min_k", (double)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));

    wifi_status_t w;
    wifi_get_status(&w);
    cJSON *wf = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(wf, "ssid", w.ssid);
    cJSON_AddNumberToObject(wf, "rssi", w.rssi);
    if (st.rssi_max > -200) { cJSON_AddNumberToObject(wf, "rssi_min", st.rssi_min); cJSON_AddNumberToObject(wf, "rssi_max", st.rssi_max); }

    char b[200];
    frame_status(b, sizeof(b));
    for (char *p = b; *p; p++) if (*p == '\n') *p = ';';
    cJSON_AddStringToObject(o, "frame", b);
    sync_status(b, sizeof(b));
    cJSON_AddStringToObject(o, "sync", b);
    ota_status(b, sizeof(b));
    if (b[0]) cJSON_AddStringToObject(o, "ota", b);

    presence_t p;
    presence_get(&p);
    cJSON *pr = cJSON_AddObjectToObject(o, "presence");
    cJSON_AddNumberToObject(pr, "minutes", (double)st.n / 60.0);
    cJSON_AddNumberToObject(pr, "present_pct", st.n ? (double)(st.present * 100 / st.n) : 0);
    cJSON_AddNumberToObject(pr, "sensor_seen_pct", st.n ? (double)(st.raw_present * 100 / st.n) : 0);
    cJSON_AddNumberToObject(pr, "sensor_ok_pct", st.n ? (double)(st.sensor_ok * 100 / st.n) : 0);
    cJSON_AddNumberToObject(pr, "changes", (double)(p.changes - st.changes0));
    cJSON_AddStringToObject(pr, "now", p.present ? "present" : "away");
    cJSON_AddStringToObject(pr, "source", presence_source_name(p.source));

    cJSON *d = cJSON_AddObjectToObject(o, "distance_cm");
    cJSON_AddNumberToObject(d, "readings", st.dist_n);
    if (st.dist_n) {
        cJSON_AddNumberToObject(d, "p10", dist_pct(10));
        cJSON_AddNumberToObject(d, "p50", dist_pct(50));
        cJSON_AddNumberToObject(d, "p90", dist_pct(90));
        cJSON_AddNumberToObject(d, "within_65_s", st.near65);
        cJSON_AddNumberToObject(d, "within_50_s", st.near50);
        cJSON_AddNumberToObject(d, "peak_energy", st.peak_max);
    }
    egg_status_t e;
    egg_get_status(&e);
    cJSON *eg = cJSON_AddObjectToObject(o, "stare_egg");
    cJSON_AddBoolToObject(eg, "egg_on", e.enabled);
    cJSON_AddBoolToObject(eg, "stare_on", e.stare_mode);
    cJSON_AddNumberToObject(eg, "longest_still_s", st.max_still);
    cJSON_AddNumberToObject(eg, "stare_alive_s", st.stare_alive);
    cJSON_AddNumberToObject(o, "every_min", every_min);

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

// ---------------------------------------------------------------- sending

static char *b64_encode(const char *src, size_t n)
{
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, (const unsigned char *)src, n);
    char *out = pmalloc(olen + 1);
    if (!out) return NULL;
    if (mbedtls_base64_encode((unsigned char *)out, olen + 1, &olen, (const unsigned char *)src, n) != 0) { free(out); return NULL; }
    out[olen] = 0;
    return out;
}

static char *b64_decode(const char *src)
{
    size_t n = strlen(src);
    char *clean = pmalloc(n + 1);
    if (!clean) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (src[i] != '\n' && src[i] != '\r') clean[k++] = src[i];
    clean[k] = 0;
    size_t olen = 0;
    mbedtls_base64_decode(NULL, 0, &olen, (const unsigned char *)clean, k);
    char *out = pmalloc(olen + 2);
    if (!out) { free(clean); return NULL; }
    if (mbedtls_base64_decode((unsigned char *)out, olen + 1, &olen, (const unsigned char *)clean, k) != 0) olen = 0;
    out[olen] = 0;
    free(clean);
    return out;
}

// Append one line to log/<date>.jsonl in the reports repo. true on success.
static bool send_report(const rcfg_t *c)
{
    char *line = build_line(c->every_min);
    if (!line) return false;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char day[16];
    strftime(day, sizeof(day), "%Y-%m-%d", &lt);
    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/contents/log/%s.jsonl", c->repo, day);

    // current file (if any): its sha + text
    char *resp = NULL;
    int code = http("GET", url, NULL, "application/vnd.github+json", true, NULL, &resp);
    char sha[64] = "";
    char *old = NULL;
    if (code == 200 && resp) {
        cJSON *j = cJSON_Parse(resp);
        const char *s = j ? jstr(j, "sha") : NULL;
        const char *ct = j ? jstr(j, "content") : NULL;
        if (s) snprintf(sha, sizeof(sha), "%s", s);
        if (ct) old = b64_decode(ct);
        cJSON_Delete(j);
    }
    free(resp);
    if (code == 401 || code == 403 || code == 404) {
        if (code == 404 && sha[0] == 0) { /* no file yet today: fine, unless the repo itself is missing */ }
        if (code == 401) {
            free(line); free(old);
            token_save(NULL);
            set_state(REP_UNLINKED);
            set_msg("GitHub refused the key (401) - link again");
            return false;
        }
        if (code == 403) { free(line); free(old); set_msg("Report refused (403) - check the App's permissions"); return false; }
    } else if (code != 200) {
        free(line); free(old);
        set_msg("Report: could not read today's file (%d)", code);
        return false;
    }

    size_t oldn = old ? strlen(old) : 0;
    if (oldn > MAX_LOG) oldn = 0;                        // runaway file: start the day over
    size_t ln = strlen(line);
    char *text = pmalloc(oldn + ln + 2);
    if (!text) { free(line); free(old); return false; }
    if (oldn) memcpy(text, old, oldn);
    memcpy(text + oldn, line, ln);
    text[oldn + ln] = '\n';
    text[oldn + ln + 1] = 0;
    free(old);
    free(line);

    char *b64 = b64_encode(text, oldn + ln + 1);
    free(text);
    if (!b64) return false;
    char msg[48];
    strftime(msg, sizeof(msg), "Frame report %H:%M", &lt);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "message", msg);
    cJSON_AddStringToObject(body, "content", b64);
    if (sha[0]) cJSON_AddStringToObject(body, "sha", sha);
    free(b64);
    char *bs = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!bs) return false;
    code = http("PUT", url, "application/json", "application/vnd.github+json", true, bs, &resp);
    free(bs);
    free(resp);
    if (code == 200 || code == 201) {
        char hm[8];
        strftime(hm, sizeof(hm), "%H:%M", &lt);
        set_msg("Linked - last report %s OK", hm);
        return true;
    }
    if (code == 401) { token_save(NULL); set_state(REP_UNLINKED); set_msg("GitHub refused the key (401) - link again"); return false; }
    set_msg("Report failed (%d)%s", code, code == 404 ? " - is the App installed on the repo?" : "");
    return false;
}

// ---------------------------------------------------------------- task

static void report_task(void *arg)
{
    (void)arg;
    sd_state_t sd;
    do { vTaskDelay(pdMS_TO_TICKS(500)); sdcard_get(&sd); } while (!sd.done);

    token_load();
    stats_reset();
    rcfg_t c;
    load_cfg(&c);
    int64_t last_cfg = esp_timer_get_time();
    int64_t next_report = esp_timer_get_time() + 3LL * 60 * 1000000;    // first one ~3 min after boot

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        stats_sample();
        int64_t now = esp_timer_get_time();
        if (now - last_cfg > 60LL * 1000000) { last_cfg = now; load_cfg(&c); }

        rep_state_t s = get_state();
        bool set_up = c.client_id[0] && c.repo[0];
        if (s != REP_ASKING && s != REP_CODE && s != REP_FAILED) {
            rep_state_t want = !set_up ? REP_NOT_SET_UP : token[0] ? REP_LINKED : REP_UNLINKED;
            if (want != s) {
                set_state(want);
                if (want == REP_NOT_SET_UP) set_msg("Reports: not set up yet");
                else if (want == REP_UNLINKED) set_msg("Not linked - tap Get code");
                else set_msg("Linked");
            }
        }

        if (want_unlink) {
            want_unlink = false;
            token_save(NULL);
            set_state(set_up ? REP_UNLINKED : REP_NOT_SET_UP);
            set_msg("Unlinked");
        }
        if (want_link) {
            want_link = false;
            load_cfg(&c);
            do_link(&c);                                   // blocks until linked, cancelled or failed
            continue;
        }

        bool due = want_now || now >= next_report;
        if (!due) continue;
        if (!token[0] || !set_up) { want_now = false; next_report = now + 5LL * 60 * 1000000; continue; }
        if (!c.enabled) { want_now = false; set_msg("Linked - reports paused (state.json)"); next_report = now + 10LL * 60 * 1000000; continue; }
        wifi_status_t w;
        wifi_get_status(&w);
        if (w.state != WIFI_ST_CONNECTED || !w.time_ok || sync_busy()) continue;   // try again in a second
        want_now = false;
        bool ok = send_report(&c);
        if (ok) stats_reset();
        next_report = esp_timer_get_time() + (ok ? (int64_t)c.every_min * 60 * 1000000 : 10LL * 60 * 1000000);
    }
}

void report_start(void)
{
    if (xTaskCreatePinnedToCoreWithCaps(report_task, "report", 10240, NULL, 2, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE(TAG, "could not start");
}
