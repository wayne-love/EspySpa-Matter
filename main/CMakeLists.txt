idf_component_register(
    SRCS
        "app_main.cpp"
        "spa_service.cpp"
        "matter.cpp"
        "diagnostics.cpp"
        "status_led.cpp"
        "reset_button.cpp"
        "firmware_update.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        spa_protocol
        esp_matter
        nvs_flash
        esp_driver_uart
        esp_timer
        esp_http_server
        esp_http_client
        esp_https_ota
        app_update
        json
        openthread
        led_strip
        esp_driver_gpio
        mbedtls
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

if(DEFINED ENV{ESPYSPA_CHANNEL} AND NOT "$ENV{ESPYSPA_CHANNEL}" STREQUAL "")
    set(ESPYSPA_CHANNEL "$ENV{ESPYSPA_CHANNEL}")
else()
    set(ESPYSPA_CHANNEL "development")
endif()

if(DEFINED ENV{ESPYSPA_GIT_COMMIT} AND NOT "$ENV{ESPYSPA_GIT_COMMIT}" STREQUAL "")
    set(ESPYSPA_GIT_COMMIT "$ENV{ESPYSPA_GIT_COMMIT}")
else()
    set(ESPYSPA_GIT_COMMIT "unknown")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESPYSPA_FIRMWARE_CHANNEL="${ESPYSPA_CHANNEL}"
    ESPYSPA_GIT_COMMIT="${ESPYSPA_GIT_COMMIT}"
)
