#pragma once
// On-screen diagnostics: sensor presence, settings, per-gate energy, system info.
// Call once with the LVGL lock held.
void diag_create(void);
