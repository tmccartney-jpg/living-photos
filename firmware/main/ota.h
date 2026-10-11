#pragma once
// Firmware updates over Wi-Fi (stage 4).
//
// Releases live in the repo: ota/firmware.json {"build": N, "bin": "ota/aura_fw.bin", "bytes": ...}
// and the image itself. The build number is firmware/version.txt (also the app version).
//
//   - After every sync pass the frame reads ota/firmware.json. A higher build than the one
//     running (and not one that already failed here) becomes "waiting".
//   - A waiting update installs between 03:00 and 04:00 with the backlight off, or right away
//     from the diagnostics Update button. It goes into the other firmware slot, then restarts.
//   - A new build must prove itself: the frame task has to be running 60 s after boot. If it
//     isn't, or the build crashes before then, the chip goes back to the previous build and
//     remembers not to try that build again.
#include <stdbool.h>
#include <stddef.h>

#define OTA_JSON        "ota/firmware.json"
#define OTA_HOUR        3          // install window 03:00-03:59 local time
#define OTA_HEALTHY_S   60         // a new build is kept after this long running well
#define OTA_GIVEUP_S    180        // ... or rolled back if it isn't healthy by then

void ota_start(void);                  // end of app_main
void ota_check(const char *json_text);  // sync task, after each pass: ota/firmware.json or NULL
int  ota_running_build(void);
bool ota_waiting(void);                // an update is ready to install
void ota_install_now(void);            // diagnostics button
void ota_status(char *buf, size_t len); // short line, empty if nothing to say
