idf_component_register(
    SRCS "ui.cpp"
         "fonts/sticky_sans_16.c" "fonts/sticky_sans_18.c" "fonts/sticky_sans_24.c"
         "fonts/sticky_bold_18.c" "fonts/sticky_bold_24.c" "fonts/sticky_bold_34.c"
         "fonts/sticky_bold_52.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl board epaper_display esp_timer esp_wifi heap freertos power_service
)
