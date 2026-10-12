#pragma once
// Remote tuning from state.json (synced from GitHub), re-read every 30 s:
//
//   "sensor": {
//     "mode": "sensor" | "frame",   sensor = the sensor's own present/absent (default)
//                                   frame  = the frame compares gate energies with the lists below
//     "max_gate": 4,                gates 0..max_gate count (0.7 m each)
//     "trigger": [..],              energy that makes a gate count as someone there
//     "hold": [..],                 lower energy that keeps it counting (optional; default = trigger)
//     "away_after_s": 10            clear this long before the scene may move
//   },
//   "reveal": "hidden" | "stare",   hidden = scenes move only while nobody is near (default)
//                                   stare  = also come alive when someone stands close and still
//                                            for "stare_s" seconds, until they walk away
//   "stare_s": 15,
//   "easter_egg": {                 (its max_cm / jitter_cm / still_max also define "close and still"
//                                    for the stare)
//     "enabled": false,
//     "max_cm": 80,                 someone this close ...
//     "jitter_cm": 15,              ... whose distance wanders less than this over 5 s ...
//     "still_max": 65535,           ... with no gate energy above this ...
//     "hold_s": 30,                 ... for this long fires the egg
//     "every_h": 24,                at most once per this many hours
//     "clip": "intros/device/tz_egg.lpv"   played once over the scene (needs scene_motion on)
//   }
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hmmd.h"

typedef struct {
    bool frame_decides;
    int max_gate;
    uint32_t trigger[HMMD_GATES];
    uint32_t hold[HMMD_GATES];
    uint32_t away_ms;

    bool egg_enabled;
    int egg_max_cm, egg_jitter_cm;
    uint32_t egg_still_max, egg_hold_s, egg_every_h;
    char egg_clip[96];                // full SD path

    bool stare_mode;                  // reveal == "stare"
    uint32_t stare_s;
} tuning_t;

typedef struct {
    bool enabled;
    int still_s, need_s;              // progress toward firing
    int dist_cm, range_cm;
    uint32_t peak;
    int cooldown_min;                 // minutes until it may fire again (0 = ready)
    bool stare_mode, stare_active;    // reveal "stare"; someone is watching it come alive
    int stare_need_s;
} egg_status_t;

bool stare_active(void);              // frame: scene should move although someone is present

void tuning_start(void);              // after sdcard_start()
void tuning_get(tuning_t *out);
void egg_get_status(egg_status_t *out);
// Frame: true once when the egg has just fired (within 2 s); copies the clip path.
bool egg_take(char *clip, size_t len);
