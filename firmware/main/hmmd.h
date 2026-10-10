#pragma once
#include <stdbool.h>
#include <stdint.h>

// Waveshare HMMD mmWave sensor (S3KM1110) on UART0, GPIO43 TX / GPIO44 RX.
// Board UART switch must be on UART2 (sensor header).

#define HMMD_GATES 16

typedef struct {
    // link
    bool connected;            // sensor answered config and is streaming
    int retries;               // connection attempts that failed so far
    // settings read back at boot
    bool config_valid;
    char firmware[24];
    uint32_t min_gate, max_gate, delay_s;
    uint32_t trigger[HMMD_GATES];
    uint32_t hold[HMMD_GATES];
    // latest report frame
    bool present;
    uint16_t distance_cm;
    uint16_t energy[HMMD_GATES];
    uint32_t frames;
    int64_t last_frame_us;     // esp_timer time of the latest frame, 0 = none yet
} hmmd_state_t;

void hmmd_start(void);
void hmmd_get_state(hmmd_state_t *out);   // thread-safe snapshot
