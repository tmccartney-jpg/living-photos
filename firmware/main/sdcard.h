#pragma once
#include <stdbool.h>
#include <stdint.h>

// microSD on the Waveshare 7" board: SPI2, MOSI 11 / MISO 13 / CLK 12.
// Card CS is CH422G EXIO4 (held low), so no CS GPIO.
#define SD_MOUNT_POINT "/sdcard"

typedef struct {
    bool done;            // mount attempt finished
    bool mounted;
    char error[48];       // why it failed, if it did
    char card_name[12];
    uint32_t size_mb;
    uint32_t free_mb;
    int file_count;       // entries in the card's root folder
    char listing[96];     // first few root entries, comma separated
} sd_state_t;

void sdcard_start(void);              // mounts in the background
void sdcard_get(sd_state_t *out);
