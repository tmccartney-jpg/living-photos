#pragma once
// GitHub -> SD card mirror (stage 3).
//
// Every 5 minutes (on the clock: :00, :05, ... once internet time is set) and
// ~15 s after Wi-Fi first connects:
//   1. download device_index.json from the repo (raw.githubusercontent.com)
//   2. for each listed file, compare its SHA-256 with the card; download only
//      what differs, to <path>.tmp, verify the hash, then replace the old file
//   3. refresh the manifests and state.json, then rotation.json last
//   4. delete files in the device folders that the index no longer lists
// The file currently on screen is never replaced or deleted; it waits for the
// next pass. The frame always plays from the card (docs/Authority.md).
#include <stdbool.h>
#include <stddef.h>

#define SYNC_PERIOD_S   300
#define SYNC_REPO_RAW   "https://raw.githubusercontent.com/tmccartney-jpg/living-photos/main/"

void sync_start(void);                    // after wifi_start() and frame_start()
void sync_now(void);                      // diagnostics button
void sync_status(char *buf, size_t len);  // one short line
