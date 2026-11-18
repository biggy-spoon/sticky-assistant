# The UI's fonts: Liberation Sans always, plus your own if
# firmware/tools/make_fonts.sh has written them to fonts/custom. With both, the
# device's Settings page switches between them.
set(font_sizes sans_16 sans_18 sans_24 bold_18 bold_24 bold_34 bold_52)
set(font_srcs "")
foreach(size ${font_sizes})
    list(APPEND font_srcs "fonts/sticky_${size}.c")
endforeach()
set(has_custom 0)
set(custom_name "Your own font")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/fonts/custom/custom_sans_24.c")
    set(has_custom 1)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/fonts/custom/name.txt")
        file(READ "${CMAKE_CURRENT_LIST_DIR}/fonts/custom/name.txt" custom_name)
        string(STRIP "${custom_name}" custom_name)
    endif()
    foreach(size ${font_sizes})
        list(APPEND font_srcs "fonts/custom/custom_${size}.c")
    endforeach()
    message(STATUS "ui: building in the custom fonts from fonts/custom")
endif()

idf_component_register(
    SRCS "ui.cpp" ${font_srcs}
    INCLUDE_DIRS "include"
    REQUIRES lvgl board epaper_display esp_timer esp_wifi heap freertos power_service
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC STICKY_HAS_CUSTOM_FONT=${has_custom}
                           "STICKY_CUSTOM_FONT_NAME=\"${custom_name}\"")
