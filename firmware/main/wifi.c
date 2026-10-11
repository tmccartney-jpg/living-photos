// Wi-Fi + internet time (stage 2). See wifi.h.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "wifi.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "wifi";

#define EV_GOT_IP     (1u << 0)
#define EV_DISC       (1u << 1)
#define EV_SCAN_DONE  (1u << 2)
#define CONNECT_TIMEOUT_US  (15 * 1000000LL)
#define TRY_TIMEOUT_US      (25 * 1000000LL)
#define SCAN_TIMEOUT_US     (10 * 1000000LL)

typedef enum { CMD_SCAN, CMD_TRY } cmd_t;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_status_t st;
static wifi_ap_t aps[WIFI_MAX_APS];
static int n_aps;
static char saved_ssid[33], saved_pass[65];
static char try_ssid[33], try_pass[65];
static QueueHandle_t cmdq;
static TaskHandle_t task;
static volatile uint8_t last_reason;
static char got_ip[16];
static int64_t missing_since_us;   // 0 = connected
static int64_t snooze_until_us;
static volatile bool radio_off;

// ---------------------------------------------------------------- NVS

static void creds_load(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t a = sizeof(saved_ssid), b = sizeof(saved_pass);
    if (nvs_get_str(h, "ssid", saved_ssid, &a) != ESP_OK) saved_ssid[0] = 0;
    if (nvs_get_str(h, "pass", saved_pass, &b) != ESP_OK) saved_pass[0] = 0;
    nvs_close(h);
}

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
    snprintf(saved_ssid, sizeof(saved_ssid), "%s", ssid);
    snprintf(saved_pass, sizeof(saved_pass), "%s", pass);
    ESP_LOGI(TAG, "saved network \"%s\"", ssid);
}

// ---------------------------------------------------------------- events

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        last_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        xTaskNotify(task, EV_DISC, eSetBits);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xTaskNotify(task, EV_SCAN_DONE, eSetBits);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(got_ip, sizeof(got_ip), IPSTR, IP2STR(&e->ip_info.ip));
        xTaskNotify(task, EV_GOT_IP, eSetBits);
    }
}

static void on_time(struct timeval *tv)
{
    (void)tv;
    portENTER_CRITICAL(&lock);
    st.time_ok = true;
    portEXIT_CRITICAL(&lock);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[40];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S %Z", &tm);
    ESP_LOGI(TAG, "internet time: %s", b);
}

// ---------------------------------------------------------------- helpers

static void connect_to(const char *ssid, const char *pass)
{
    wifi_config_t c = {0};
    snprintf((char *)c.sta.ssid, sizeof(c.sta.ssid), "%s", ssid);
    snprintf((char *)c.sta.password, sizeof(c.sta.password), "%s", pass);
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    c.sta.threshold.authmode = WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_err_t e = esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to \"%s\"%s", ssid, e == ESP_OK ? "" : " (connect call failed)");
}

static const char *reason_text(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_CONNECTION_FAIL:
        return "Wrong password?";
    case WIFI_REASON_NO_AP_FOUND:
        return "Network not found";
    default:
        return NULL;
    }
}

static void read_scan(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *rec = n ? calloc(n, sizeof(*rec)) : NULL;
    if (n && !rec) { esp_wifi_clear_ap_list(); return; }
    if (n) esp_wifi_scan_get_ap_records(&n, rec);
    wifi_ap_t tmp[WIFI_MAX_APS];
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *s = (const char *)rec[i].ssid;
        if (!s[0]) continue;                       // hidden
        int j;
        for (j = 0; j < k; j++) if (!strcmp(tmp[j].ssid, s)) break;
        if (j < k) {                               // same name: keep the strongest
            if (rec[i].rssi > tmp[j].rssi) tmp[j].rssi = rec[i].rssi;
            continue;
        }
        if (k >= WIFI_MAX_APS) continue;
        snprintf(tmp[k].ssid, sizeof(tmp[k].ssid), "%s", s);
        tmp[k].rssi = rec[i].rssi;
        tmp[k].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    free(rec);
    for (int a = 0; a < k; a++)                    // strongest first
        for (int b = a + 1; b < k; b++)
            if (tmp[b].rssi > tmp[a].rssi) { wifi_ap_t t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
    portENTER_CRITICAL(&lock);
    memcpy(aps, tmp, sizeof(tmp));
    n_aps = k;
    st.scan_gen++;
    st.scanning = false;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "scan: %d networks", k);
}

// ---------------------------------------------------------------- task

static void wifi_task(void *arg)
{
    (void)arg;
    bool connected = false, connecting = false;
    bool trying = false, try_pending = false;
    bool scan_pending = false, scanning = false;
    int try_fails = 0;
    int64_t next_retry = 0, connect_deadline = 0, try_deadline = 0, scan_deadline = 0;
    int backoff_s = 5;
    int64_t last_rssi_us = 0;
    int rssi = 0;

    missing_since_us = esp_timer_get_time();        // counts from boot until the first connect

    while (1) {
        uint32_t ev = 0;
        xTaskNotifyWait(0, UINT32_MAX, &ev, pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();

        if (ev & EV_GOT_IP) {
            connected = true;
            connecting = false;
            backoff_s = 5;
            missing_since_us = 0;
            if (trying) {
                creds_save(try_ssid, try_pass);
                trying = false;
                portENTER_CRITICAL(&lock);
                st.try_state = WIFI_TRY_OK;
                st.try_msg[0] = 0;
                portEXIT_CRITICAL(&lock);
            }
            ESP_LOGI(TAG, "connected, IP %s", got_ip);
        }

        if (ev & EV_DISC) {
            uint8_t r = last_reason;
            bool was = connected;
            connected = false;
            connecting = false;
            if (was) ESP_LOGW(TAG, "disconnected (reason %u)", r);
            if (try_pending) {
                // our own disconnect before a trial connection: ignore
            } else if (trying) {
                try_fails++;
                const char *why = reason_text(r);
                if (try_fails >= 2 || r == WIFI_REASON_NO_AP_FOUND) {
                    trying = false;
                    portENTER_CRITICAL(&lock);
                    st.try_state = WIFI_TRY_FAIL;
                    if (why) snprintf(st.try_msg, sizeof(st.try_msg), "%s", why);
                    else snprintf(st.try_msg, sizeof(st.try_msg), "Couldn't connect (code %u)", r);
                    portEXIT_CRITICAL(&lock);
                    ESP_LOGW(TAG, "trial connection failed (reason %u)", r);
                    next_retry = now + 1000000;     // back to the saved network
                } else {
                    next_retry = now + 500000;
                }
            } else {
                if (missing_since_us == 0) missing_since_us = now;
                next_retry = now + backoff_s * 1000000LL;
                if (backoff_s < 60) backoff_s = backoff_s * 2 > 60 ? 60 : backoff_s * 2;
            }
        }

        if (ev & EV_SCAN_DONE) {
            read_scan();
            scanning = false;
        }

        cmd_t cmd;
        while (xQueueReceive(cmdq, &cmd, 0) == pdTRUE) {
            if (cmd == CMD_SCAN) {
                if (!scanning) scan_pending = true;
            } else if (cmd == CMD_TRY) {
                trying = true;
                try_pending = true;
                try_fails = 0;
                try_deadline = now + TRY_TIMEOUT_US;
                esp_wifi_disconnect();
                connecting = false;
                connected = false;
                next_retry = now + 400000;
                portENTER_CRITICAL(&lock);
                st.try_state = WIFI_TRY_BUSY;
                st.try_msg[0] = 0;
                portEXIT_CRITICAL(&lock);
            }
        }

        // scanning (a connection attempt in progress blocks scans, so stop it first)
        if (radio_off) { scan_pending = false; connecting = false; }
        if (scan_pending) {
            if (connecting) { esp_wifi_disconnect(); connecting = false; }
            if (esp_wifi_scan_start(NULL, false) == ESP_OK) {
                scan_pending = false;
                scanning = true;
                scan_deadline = now + SCAN_TIMEOUT_US;
                portENTER_CRITICAL(&lock);
                st.scanning = true;
                portEXIT_CRITICAL(&lock);
            }
        }
        if (scanning && now > scan_deadline) {
            esp_wifi_scan_stop();
            scanning = false;
            portENTER_CRITICAL(&lock);
            st.scanning = false;
            portEXIT_CRITICAL(&lock);
        }

        if (trying && now > try_deadline) {
            trying = false;
            try_pending = false;
            esp_wifi_disconnect();
            connecting = false;
            portENTER_CRITICAL(&lock);
            st.try_state = WIFI_TRY_FAIL;
            snprintf(st.try_msg, sizeof(st.try_msg), "No answer from the network");
            portEXIT_CRITICAL(&lock);
            next_retry = now + 1000000;
        }

        if (connecting && now > connect_deadline) {
            esp_wifi_disconnect();                  // the disconnect event schedules the retry
            connecting = false;
        }

        if (!radio_off && !connected && !connecting && !scanning && !scan_pending && now >= next_retry) {
            if (trying) {
                try_pending = false;
                connect_to(try_ssid, try_pass);
                connecting = true;
                connect_deadline = now + CONNECT_TIMEOUT_US;
            } else if (saved_ssid[0]) {
                connect_to(saved_ssid, saved_pass);
                connecting = true;
                connect_deadline = now + CONNECT_TIMEOUT_US;
                if (missing_since_us == 0) missing_since_us = now;
            }
        }

        if (connected && now - last_rssi_us > 5000000) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
            last_rssi_us = now;
        }

        portENTER_CRITICAL(&lock);
        st.radio_off = radio_off;
        st.has_creds = saved_ssid[0] != 0;
        snprintf(st.ssid, sizeof(st.ssid), "%s", saved_ssid);
        st.state = connected ? WIFI_ST_CONNECTED
                 : !st.has_creds && !trying ? WIFI_ST_NO_CREDS
                 : connecting ? WIFI_ST_CONNECTING : WIFI_ST_WAITING;
        st.rssi = connected ? rssi : 0;
        snprintf(st.ip, sizeof(st.ip), "%s", connected ? got_ip : "");
        st.missing_s = (connected || !missing_since_us) ? 0 : (uint32_t)((now - missing_since_us) / 1000000);
        portEXIT_CRITICAL(&lock);
    }
}

// ---------------------------------------------------------------- public

void wifi_get_status(wifi_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = st;
    portEXIT_CRITICAL(&lock);
}

int wifi_scan_results(wifi_ap_t *out, int max)
{
    portENTER_CRITICAL(&lock);
    int n = n_aps < max ? n_aps : max;
    memcpy(out, aps, n * sizeof(*out));
    portEXIT_CRITICAL(&lock);
    return n;
}

void wifi_scan(void)
{
    cmd_t c = CMD_SCAN;
    if (cmdq) xQueueSend(cmdq, &c, 0);
}

void wifi_try(const char *ssid, const char *pass)
{
    snprintf(try_ssid, sizeof(try_ssid), "%s", ssid);
    snprintf(try_pass, sizeof(try_pass), "%s", pass ? pass : "");
    portENTER_CRITICAL(&lock);
    st.try_state = WIFI_TRY_BUSY;      // visible immediately, before the task picks it up
    st.try_msg[0] = 0;
    portEXIT_CRITICAL(&lock);
    cmd_t c = CMD_TRY;
    if (cmdq) xQueueSend(cmdq, &c, 0);
}

bool wifi_setup_wanted(void)
{
    int64_t now = esp_timer_get_time();
    wifi_status_t s;
    wifi_get_status(&s);
    if (now < snooze_until_us) return false;
    if (s.try_state == WIFI_TRY_BUSY) return false;
    if (!s.has_creds) return true;
    return s.state != WIFI_ST_CONNECTED && s.missing_s >= WIFI_MISSING_S;
}

void wifi_snooze(void)
{
    snooze_until_us = esp_timer_get_time() + (int64_t)WIFI_SNOOZE_S * 1000000;
    ESP_LOGI(TAG, "setup hidden for %d min", WIFI_SNOOZE_S / 60);
}

bool wifi_radio_on(void) { return !radio_off; }

void wifi_radio_set(bool on)
{
    if (on == !radio_off) return;
    if (on) {
        radio_off = false;
        esp_wifi_start();
        ESP_LOGI(TAG, "radio ON (test)");
    } else {
        radio_off = true;
        esp_wifi_stop();
        ESP_LOGI(TAG, "radio OFF (test)");
    }
}

void wifi_start(void)
{
    creds_load();
    st.has_creds = saved_ssid[0] != 0;
    snprintf(st.ssid, sizeof(st.ssid), "%s", saved_ssid);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, "living-photos");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // credentials live in our own NVS keys
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    cmdq = xQueueCreate(4, sizeof(cmd_t));
    xTaskCreatePinnedToCore(wifi_task, "wifi_ctl", 4096, NULL, 4, &task, 0);
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                               ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    sc.sync_cb = on_time;
    esp_netif_sntp_init(&sc);

    ESP_LOGI(TAG, "started; saved network: %s", saved_ssid[0] ? saved_ssid : "(none)");
}
