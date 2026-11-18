cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL allocates its heap through this hook, which puts it in PSRAM. The hook
# is a static inline function in a header so LVGL needs no link dependency.
idf_build_set_property(COMPILE_DEFINITIONS
    "LV_MEM_POOL_INCLUDE=\"${CMAKE_CURRENT_LIST_DIR}/components/ui/lv_psram_pool.h\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_POOL_ALLOC=sticky_lv_pool_alloc" APPEND)
project(sticky_claude)
