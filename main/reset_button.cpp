#include "app.hpp"
#include "reset_gesture.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_matter.h"
#include "platform/CHIPDeviceLayer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
namespace {
const char *TAG = "factory_reset";
static_assert(CONFIG_SPA_RESET_GPIO != CONFIG_SPA_TX_GPIO && CONFIG_SPA_RESET_GPIO != CONFIG_SPA_RX_GPIO &&
              CONFIG_SPA_RESET_GPIO != CONFIG_SPA_LED_GPIO, "Reset button must have a dedicated GPIO");
void perform_reset(intptr_t) {
    ESP_LOGW(TAG, "Clearing Matter/Thread configuration and restarting");
    auto err = esp_matter::factory_reset();
    if (err != ESP_OK) ESP_LOGE(TAG, "Factory reset returned: %s", esp_err_to_name(err));
}
void worker(void *) {
    ResetGesture gesture;
    while (true) {
        if (gesture.sample(pressed, now_ms())) {
            ESP_LOGW(TAG, "Five BOOT presses confirmed; factory reset requested");
            status_led_reset_pending(true);
            // Allow the LED's independent task to show its purple confirmation.
            vTaskDelay(pdMS_TO_TICKS(1500));
            // GPIO9 is a boot strap. Wait for release if it was pressed again during confirmation.
            unsigned released = 0;
            while (released < 5) {
                released = gpio_get_level(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO)) ? released + 1 : 0;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            auto err = chip::DeviceLayer::PlatformMgr().ScheduleWork(perform_reset, 0);
            if (err != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Unable to schedule factory reset");
                status_led_reset_pending(false);
            }
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}
void reset_button_start() {
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << CONFIG_SPA_RESET_GPIO;
    cfg.mode = GPIO_MODE_INPUT; cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE; cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(xTaskCreate(worker, "reset_button", 3072, nullptr, 2, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
