#pragma once
// Frame mode (stage 1): plays the day's scene from the SD card.
//
// SD card layout mirrors the repo:
//   /sdcard/rotation.json                    today's scene, yesterday's scene, intro, change_at
//   /sdcard/<theme>/manifest.json            used for presence / test cycling when rotation.json lacks it
//   /sdcard/<theme>/device/<id>.lpv | .jpg   clip (LPV1) and still (frame 0)
//   /sdcard/intros/device/<name>.jpg         intro
//
// Behaviour:
//   - Intro at boot and whenever the shown scene changes, then the scene with its caption.
//   - Clip scenes move only while presence is AWAY. While PRESENT: presence "freeze" holds the
//     current frame; presence "rest" (light-only scenes) shows frame 0 (lights off).
//   - Still-only scenes show the still. Clips with motion "once" play one time, then hold
//     their last frame.
//   - Before change_at (09:30) on rotation.date the frame keeps showing previous_scene; from
//     change_at it shows today's scene. Without a valid clock it shows today's scene.
//   - rotation.json is re-read when it changes on the card.
//   - Diagnostics: hold the top-left corner for 2 s (same gesture or 60 s idle to leave).

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

void frame_start(void);                       // after diag_create(); takes over the display
void frame_show_scene(void);                  // LVGL lock held
void frame_show_diag(void);                   // LVGL lock held
void frame_show_other(lv_obj_t *scr);         // another full screen (Wi-Fi setup); scene pauses
bool frame_scene_visible(void);               // LVGL lock held
void frame_test_next(void);
void frame_preview_cycle(void);               // diagnostics: Preview off -> 10 s -> 20 s -> 30 s -> off
int  frame_preview_secs(void);                 // 0 = off                   // diagnostics: cycle scenes from the manifest, then back to schedule
void frame_status(char *buf, size_t len);     // one or two lines for the diagnostics screen
bool frame_file_in_use(const char *full_path); // sync: never replace the file on screen
