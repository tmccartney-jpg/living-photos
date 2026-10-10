#pragma once
// Scene player (step 7b): plays an .lpv clip from the SD card full screen.
// Moves only while presence says AWAY; freezes on the current frame when PRESENT.
//
// .lpv format (little endian):
//   0  "LPV1"   4  u16 width   6  u16 height   8  u16 fps   10 u16 flags (0)
//   12 u32 frame_count        16 u32 max_jpeg_bytes          20..31 reserved
//   then per frame: u32 jpeg_len, jpeg bytes

#define PLAYER_CLIP_PATH     "/sdcard/scenes/bus_stop.lpv"
#define PLAYER_CLIP_FALLBACK "/sdcard/bus_stop.lpv"   // card root also accepted

void player_start(void);   // call after diag_create(); starts on the diagnostics screen
void player_show(void);    // switch to the scene (call with LVGL lock held)
