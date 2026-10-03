#include "app.hpp"
#include "nvs_flash.h"
#include "esp_log.h"
extern "C" void spa_worker_start();
extern "C" void app_main() {
    static const char *TAG = "espyspa";
    ESP_LOGI(TAG, "Starting EspySpa-Matter on ESP32-C6");
    // Do not silently erase commissioning data on an NVS error.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "NVS ready; initializing spa interface");
    spa_start();
    status_led_start();
    ESP_LOGI(TAG, "Starting Matter over Thread");
    matter_start();
    reset_button_start();
    ESP_LOGI(TAG, "Starting IPv6 diagnostics on port 8080");
    diagnostics_start();
    spa_worker_start();
    ESP_LOGI(TAG, "Startup complete; spa worker running");
}
