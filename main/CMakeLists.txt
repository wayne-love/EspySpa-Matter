idf_component_register(SRCS "app_main.cpp" "spa_service.cpp" "matter.cpp" "diagnostics.cpp" "status_led.cpp"
    INCLUDE_DIRS "." REQUIRES spa_protocol esp_matter nvs_flash esp_driver_uart esp_timer esp_http_server json openthread led_strip)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
