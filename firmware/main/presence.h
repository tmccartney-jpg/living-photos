#pragma once
#include <stdbool.h>
#include <stdint.h>

// Presence decision (step 6): turns the sensor's raw output into the
// present/away state that drives scene motion.
//   - Anyone detected      -> PRESENT immediately (scene freezes)
//   - Clear for hold time  -> AWAY (scene may move)
//   - Sensor silent/stale  -> PRESENT (fail safe: never move by mistake)

#define PRESENCE_AWAY_HOLD_MS 10000

typedef enum {
    PRES_SRC_REAL = 0,
    PRES_SRC_FORCE_PRESENT,
    PRES_SRC_FORCE_AWAY,
    PRES_SRC_COUNT
} pres_source_t;

typedef struct {
    bool present;          // the decision
    bool raw_present;      // what the sensor itself says
    bool sensor_ok;        // sensor connected and data fresh
    pres_source_t source;  // real or forced (test switch)
    uint32_t clear_ms;     // how long the sensor has been clear
    uint32_t hold_ms;      // clear time needed before AWAY
    uint32_t changes;      // decision changes since boot
    bool frame_decides;    // raw_present comes from state.json limits, not the sensor
} presence_t;

void presence_start(void);
void presence_get(presence_t *out);
void presence_cycle_source(void);
const char *presence_source_name(pres_source_t src);
