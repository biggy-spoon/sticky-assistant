#pragma once

// Included by LVGL's lv_mem.c (see LV_MEM_POOL_INCLUDE in the top-level
// CMakeLists.txt) to allocate its heap in PSRAM. Internal RAM is scarce once
// WiFi is up, and the board has 8MB of PSRAM.
#include <stddef.h>

#include "esp_heap_caps.h"

static inline void* sticky_lv_pool_alloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
