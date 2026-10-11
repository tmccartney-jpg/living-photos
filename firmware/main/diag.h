#pragma once
// On-screen diagnostics: sensor presence, settings, per-gate energy, frame status.
// Hidden: opened by a 2 s hold in the top-left corner of the scene (see frame.c).
#include "lvgl.h"

void diag_create(void);                 // builds the (not yet shown) screen; LVGL lock held
lv_obj_t *diag_screen(void);
// Invisible top-left hold zone: calls cb once after a 2 s press.
void diag_corner_hold(lv_obj_t *parent, int w, int h, void (*cb)(void));
