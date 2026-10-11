#pragma once
// LVGL's memory functions (LV_MEM_CUSTOM): put screen objects in PSRAM.
// Internal RAM is kept for what needs it (Wi-Fi, display line buffers, SD card).
// Included by LVGL itself via LV_MEM_CUSTOM_INCLUDE (see main/CMakeLists.txt).
#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *lp_lv_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static inline void *lp_lv_realloc(void *p, size_t size)
{
    return heap_caps_realloc_prefer(p, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static inline void lp_lv_free(void *p)
{
    heap_caps_free(p);
}
