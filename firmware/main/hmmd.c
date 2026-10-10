// HMMD mmWave sensor reader.
// Reads the saved settings back once at boot, then streams presence,
// distance and per-gate energy to the log once a second.
//
// Wiring (header labels): TXD -> sensor RX, RXD -> sensor TX, UART switch on UART2.
// The chip's ROM boot text on GPIO43 used to hang the sensor at power-up;
// fixed 2026-10-10 by burning eFuse UART_PRINT_CONTROL = 3 (ROM UART print off).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hmmd.h"

#define HMMD_UART   UART_NUM_0
#define HMMD_TX_PIN 43
#define HMMD_RX_PIN 44
#define HMMD_BAUD   115200

#define CMD_READ_VERSION 0x0000
#define CMD_READ_PARAM   0x0008
#define CMD_SET_MODE     0x0012
#define CMD_DISABLE_CONF 0x00FE
#define CMD_ENABLE_CONF  0x00FF
#define MODE_REPORT      0x04

#define P_MIN_GATE 0x0000
#define P_MAX_GATE 0x0001
#define P_DELAY    0x0004
#define P_TRIGGER(g) (0x0010 + (g))
#define P_HOLD(g)    (0x0020 + (g))

static const char *TAG = "hmmd";
static const uint8_t HDR[4]  = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t FTR[4]  = {0x04, 0x03, 0x02, 0x01};
static const uint8_t RHDR[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t RFTR[4] = {0xF8, 0xF7, 0xF6, 0xF5};

static uint8_t rx[512];

static hmmd_state_t st;
static portMUX_TYPE st_lock = portMUX_INITIALIZER_UNLOCKED;

void hmmd_get_state(hmmd_state_t *out)
{
    portENTER_CRITICAL(&st_lock);
    *out = st;
    portEXIT_CRITICAL(&st_lock);
}
static int have;

static int find4(const uint8_t *b, int n, const uint8_t *p)
{
    for (int i = 0; i + 4 <= n; i++) {
        if (!memcmp(b + i, p, 4)) return i;
    }
    return -1;
}

static void drop(int n)
{
    if (n >= have) { have = 0; return; }
    memmove(rx, rx + n, have - n);
    have -= n;
}

static void fill(int wait_ms)
{
    if (have >= (int)sizeof(rx)) have = 0;   // overflow: start over
    int r = uart_read_bytes(HMMD_UART, rx + have, sizeof(rx) - have, pdMS_TO_TICKS(wait_ms));
    if (r > 0) have += r;
}

// Send a command; copy the ACK payload (after the status word) to out.
// Returns payload length, or -1 on timeout / error status.
static int hmmd_cmd(uint16_t cmd, const uint8_t *data, int dlen, uint8_t *out, int outmax)
{
    uint8_t f[64];
    uint16_t blen = 2 + dlen;
    int n = 0;
    memcpy(f, HDR, 4); n = 4;
    f[n++] = blen & 0xFF; f[n++] = blen >> 8;
    f[n++] = cmd & 0xFF;  f[n++] = cmd >> 8;
    if (dlen) { memcpy(f + n, data, dlen); n += dlen; }
    memcpy(f + n, FTR, 4); n += 4;

    uart_flush_input(HMMD_UART);
    have = 0;
    uart_write_bytes(HMMD_UART, f, n);

    int64_t end = esp_timer_get_time() + 1500000;
    while (esp_timer_get_time() < end) {
        fill(50);
        int i = find4(rx, have, HDR);
        if (i < 0) { if (have > 3) drop(have - 3); continue; }
        if (have < i + 6) continue;
        int ln = rx[i + 4] | (rx[i + 5] << 8);
        if (ln < 4 || ln > (int)sizeof(rx) - 10) { drop(i + 4); continue; }
        if (have < i + 6 + ln + 4) continue;
        uint8_t *body = rx + i + 6;
        if (memcmp(body + ln, FTR, 4)) { drop(i + 4); continue; }
        uint16_t ack = body[0] | (body[1] << 8);
        uint16_t st  = body[2] | (body[3] << 8);
        if (ack != (cmd | 0x0100)) { drop(i + 6 + ln + 4); continue; }
        if (st != 0) {
            ESP_LOGW(TAG, "cmd 0x%04X failed, status %u", cmd, st);
            return -1;
        }
        int pl = ln - 4;
        if (pl > outmax) pl = outmax;
        if (out && pl > 0) memcpy(out, body + 4, pl);
        return pl;
    }
    ESP_LOGW(TAG, "no reply to cmd 0x%04X", cmd);
    return -1;
}

// Read up to 8 params per request.
static bool read_params(const uint16_t *ids, int count, uint32_t *vals)
{
    for (int k = 0; k < count; k += 8) {
        int c = count - k < 8 ? count - k : 8;
        uint8_t req[16], resp[64];
        for (int j = 0; j < c; j++) {
            req[2 * j] = ids[k + j] & 0xFF;
            req[2 * j + 1] = ids[k + j] >> 8;
        }
        int pl = hmmd_cmd(CMD_READ_PARAM, req, 2 * c, resp, sizeof(resp));
        if (pl < 4 * c) return false;
        for (int j = 0; j < c; j++) {
            const uint8_t *p = resp + 4 * j;
            vals[k + j] = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
        }
    }
    return true;
}

static int max_gate = 15;

static bool read_config_and_start_reports(void)
{
    const uint8_t en[2] = {0x02, 0x00};
    if (hmmd_cmd(CMD_ENABLE_CONF, en, 2, NULL, 0) < 0) {
        return false;
    }

    uint8_t v[64] = {0};
    int pl = hmmd_cmd(CMD_READ_VERSION, NULL, 0, v, sizeof(v) - 1);
    if (pl >= 2) {
        int n = v[0] | (v[1] << 8);
        if (n > pl - 2) n = pl - 2;
        char s[64] = {0};
        memcpy(s, v + 2, n);
        ESP_LOGI(TAG, "firmware: %s", s);
        portENTER_CRITICAL(&st_lock);
        size_t fl = strnlen(s, sizeof(st.firmware) - 1);
        memcpy(st.firmware, s, fl);
        st.firmware[fl] = 0;
        portEXIT_CRITICAL(&st_lock);
    }

    uint16_t ids[35];
    uint32_t vals[35];
    int c = 0;
    ids[c++] = P_MIN_GATE; ids[c++] = P_MAX_GATE; ids[c++] = P_DELAY;
    for (int g = 0; g < 16; g++) ids[c++] = P_TRIGGER(g);
    for (int g = 0; g < 16; g++) ids[c++] = P_HOLD(g);
    if (read_params(ids, c, vals)) {
        max_gate = vals[1] > 15 ? 15 : (int)vals[1];
        portENTER_CRITICAL(&st_lock);
        st.min_gate = vals[0];
        st.max_gate = max_gate;
        st.delay_s = vals[2];
        for (int g = 0; g < HMMD_GATES; g++) {
            st.trigger[g] = vals[3 + g];
            st.hold[g] = vals[19 + g];
        }
        st.config_valid = true;
        portEXIT_CRITICAL(&st_lock);
        ESP_LOGI(TAG, "min gate %lu, max gate %lu (~%.1f m), delay %lu s",
                 (unsigned long)vals[0], (unsigned long)vals[1], (vals[1] + 1) * 0.7,
                 (unsigned long)vals[2]);
        for (int g = 0; g <= max_gate; g++) {
            ESP_LOGI(TAG, "gate %2d  %4.1f-%4.1f m  trigger %6lu  hold %6lu", g, g * 0.7, (g + 1) * 0.7,
                     (unsigned long)vals[3 + g], (unsigned long)vals[19 + g]);
        }
    } else {
        ESP_LOGW(TAG, "could not read settings");
    }

    const uint8_t mode[6] = {0x00, 0x00, MODE_REPORT, 0x00, 0x00, 0x00};
    if (hmmd_cmd(CMD_SET_MODE, mode, 6, NULL, 0) < 0) {
        ESP_LOGW(TAG, "could not switch to report mode");
    }
    hmmd_cmd(CMD_DISABLE_CONF, NULL, 0, NULL, 0);
    return true;
}

static void hmmd_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));   // let the sensor finish booting

    // Retry until the sensor answers (safety net; normally first try).
    int attempt = 0;
    while (!read_config_and_start_reports()) {
        attempt++;
        portENTER_CRITICAL(&st_lock);
        st.retries = attempt;
        portEXIT_CRITICAL(&st_lock);
        ESP_LOGW(TAG, "sensor not answering, retry %d", attempt);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "sensor connected (%d retries)", attempt);
    portENTER_CRITICAL(&st_lock);
    st.connected = true;
    portEXIT_CRITICAL(&st_lock);

    have = 0;
    int64_t last = 0;
    uint32_t frames = 0;
    while (1) {
        fill(100);
        while (1) {
            int i = find4(rx, have, RHDR);
            if (i < 0) { if (have > 3) drop(have - 3); break; }
            if (have < i + 6) { drop(i); break; }
            int ln = rx[i + 4] | (rx[i + 5] << 8);
            if (ln < 35 || ln > 200) { drop(i + 4); continue; }
            if (have < i + 6 + ln + 4) { drop(i); break; }
            const uint8_t *b = rx + i + 6;
            if (memcmp(b + ln, RFTR, 4)) { drop(i + 4); continue; }

            frames++;
            int64_t now = esp_timer_get_time();
            portENTER_CRITICAL(&st_lock);
            st.present = b[0] != 0;
            st.distance_cm = b[1] | (b[2] << 8);
            for (int g = 0; g < HMMD_GATES; g++) st.energy[g] = b[3 + 2 * g] | (b[4 + 2 * g] << 8);
            st.frames = frames;
            st.last_frame_us = now;
            portEXIT_CRITICAL(&st_lock);
            if (now - last >= 1000000) {
                last = now;
                int present = b[0];
                int dist = b[1] | (b[2] << 8);
                char line[160];
                int n = 0;
                for (int g = 0; g <= max_gate && n < (int)sizeof(line) - 8; g++) {
                    int e = b[3 + 2 * g] | (b[4 + 2 * g] << 8);
                    n += snprintf(line + n, sizeof(line) - n, "%6d", e);
                }
                ESP_LOGI(TAG, "%s %4dcm |%s  (%lu frames)", present ? "PRESENT" : "absent ",
                         dist, line, (unsigned long)frames);
            }
            drop(i + 6 + ln + 4);
        }
    }
}

void hmmd_start(void)
{
    uart_config_t cfg = {
        .baud_rate = HMMD_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(HMMD_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HMMD_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(HMMD_UART, HMMD_TX_PIN, HMMD_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreatePinnedToCore(hmmd_task, "hmmd", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "reader started on UART0 (TX%d / RX%d)", HMMD_TX_PIN, HMMD_RX_PIN);
}
