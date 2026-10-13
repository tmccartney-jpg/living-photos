#pragma once
// Touchscreen Wi-Fi setup (stage 2): network list + on-screen keyboard.
// Opens itself when wifi_setup_wanted() says so (first boot, or the saved
// network missing for 1 hour), or by hand from diagnostics. Closes itself if
// the saved network comes back. Opened automatically it stays up until a network
// connects; "Not now" hides it for 1 h. Opened by hand it closes after 5 min untouched.
#include <stdbool.h>

void setup_start(void);        // after frame_start(); LVGL lock held
void setup_open(bool manual);  // LVGL lock held
bool setup_is_open(void);
