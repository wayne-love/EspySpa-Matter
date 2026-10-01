#include "app.hpp"
#include "nvs_flash.h"
extern "C" void spa_worker_start();
extern "C" void app_main() {
    // Do not silently erase commissioning data on an NVS error.
    ESP_ERROR_CHECK(nvs_flash_init());
    spa_start();
    matter_start();
    diagnostics_start();
    spa_worker_start();
}
