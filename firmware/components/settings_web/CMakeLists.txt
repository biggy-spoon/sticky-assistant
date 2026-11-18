idf_component_register(
    SRCS "settings_web.cpp"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "settings.html"
    REQUIRES esp_http_server esp_netif esp_system json settings_service power_service
)
