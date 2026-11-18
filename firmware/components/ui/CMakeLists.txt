# The UI's fonts: Liberation Sans by default, or your own if
# firmware/tools/make_fonts.sh has written them to fonts/custom.
set(font_names sticky_sans_16 sticky_sans_18 sticky_sans_24
               sticky_bold_18 sticky_bold_24 sticky_bold_34 sticky_bold_52)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/fonts/custom/sticky_sans_24.c")
    set(font_dir "fonts/custom")
    message(STATUS "ui: using the custom fonts in fonts/custom")
else()
    set(font_dir "fonts")
endif()
set(font_srcs "")
foreach(name ${font_names})
    list(APPEND font_srcs "${font_dir}/${name}.c")
endforeach()

idf_component_register(
    SRCS "ui.cpp" ${font_srcs}
    INCLUDE_DIRS "include"
    REQUIRES lvgl board epaper_display esp_timer esp_wifi heap freertos power_service
)
