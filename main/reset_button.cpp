#include "app.hpp"
#include "reset_gesture.hpp"
#include "alternate_boot_gesture.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_matter.h"
#include "platform/CHIPDeviceLayer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include <atomic>

namespace {
const char *TAG = "factory_reset";
struct ButtonEdge { bool pressed; uint64_t at_ms; };
QueueHandle_t button_edges = nullptr;
std::atomic<uint32_t> edge_overflow{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "Button ISR needs lock-free overflow flag");

void button_edge(void *) {
    const ButtonEdge edge{gpio_get_level(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO)) == 0, now_ms()};
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(button_edges, &edge, &wake) != pdTRUE) edge_overflow.store(1);
    if (wake) portYIELD_FROM_ISR();
}

static_assert(CONFIG_SPA_RESET_GPIO != CONFIG_SPA_TX_GPIO &&
              CONFIG_SPA_RESET_GPIO != CONFIG_SPA_RX_GPIO &&
              CONFIG_SPA_RESET_GPIO != CONFIG_SPA_LED_GPIO,
              "Reset button must have a dedicated GPIO");

void perform_reset(intptr_t) {
    ESP_LOGW(TAG, "Clearing Matter/Thread configuration and restarting");
    auto err = esp_matter::factory_reset();
    if (err != ESP_OK) ESP_LOGE(TAG, "Factory reset returned: %s", esp_err_to_name(err));
}

void worker(void *) {
    ResetGesture reset;
    AlternateBootGesture alternate;
    bool management_armed = false;
    bool previous_pressed = true;
    bool pressed = gpio_get_level(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO)) == 0;
    unsigned previous_count = 0;
    uint64_t last_sample_ms = 0;
    ESP_LOGI(TAG, "BOOT gesture handler ready on GPIO%d (level=%d); five press/releases within five seconds",
             CONFIG_SPA_RESET_GPIO, pressed ? 0 : 1);

    while (true) {
        ButtonEdge edge{};
        const bool received = xQueueReceive(button_edges, &edge, pdMS_TO_TICKS(10)) == pdTRUE;
        uint64_t now = received ? edge.at_ms : now_ms();
        if (received) {
            pressed = edge.pressed;
            ESP_LOGI(TAG, "BOOT input %s on GPIO%d at %llu ms",
                     pressed ? "pressed" : "released", CONFIG_SPA_RESET_GPIO,
                     static_cast<unsigned long long>(edge.at_ms));
        }
        // An ISR may enqueue at the timeout boundary; never move the clock back.
        if (now < last_sample_ms) now = last_sample_ms;
        last_sample_ms = now;
        if (edge_overflow.exchange(0)) {
            xQueueReset(button_edges);
            reset = ResetGesture{}; alternate = AlternateBootGesture{};
            pressed = gpio_get_level(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO)) == 0;
            management_armed = false; previous_pressed = true; previous_count = 0;
            ESP_LOGW(TAG, "BOOT edge buffer overflow; gesture discarded, release and retry");
            continue;
        }

        if (!management_armed) {
            if (!pressed) management_armed = true;
        } else if (previous_pressed && !pressed) {
            firmware_management_unlock();
        }
        previous_pressed = pressed;

        if (alternate.sample(pressed, now)) {
            ESP_LOGW(TAG, "Three-second BOOT hold confirmed; alternate firmware requested");
            esp_err_t err = firmware_reboot_alternate();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Alternate firmware boot failed: %s", esp_err_to_name(err));
            } else {
                gpio_isr_handler_remove(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO));
                vTaskDelete(nullptr);
                return;
            }
        }

        const bool reset_requested = reset.sample(pressed, now);
        if (reset.count() != previous_count) {
            if (reset.count()) ESP_LOGI(TAG, "BOOT factory-reset press %u/5", reset.count());
            else ESP_LOGI(TAG, "BOOT factory-reset sequence expired; start again");
            previous_count = reset.count();
        }
        if (reset_requested) {
            ESP_LOGW(TAG, "Five BOOT presses confirmed; factory reset requested");
            status_led_reset_pending(true);
            gpio_isr_handler_remove(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO));

            // Allow the LED's independent task to show its purple confirmation.
            vTaskDelay(pdMS_TO_TICKS(1500));

            // GPIO9 is a boot strap. Wait for release if it was pressed again
            // during confirmation.
            unsigned released = 0;
            while (released < 5) {
                released = gpio_get_level(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO))
                    ? released + 1
                    : 0;
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            auto err = chip::DeviceLayer::PlatformMgr().ScheduleWork(perform_reset, 0);
            if (err != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Unable to schedule factory reset");
                status_led_reset_pending(false);
                reset = ResetGesture{}; alternate = AlternateBootGesture{};
                previous_count = 0;
                xQueueReset(button_edges);
                edge_overflow.store(0);
                pressed = false;
                ESP_ERROR_CHECK(gpio_isr_handler_add(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO), button_edge, nullptr));
                continue;
            }
            vTaskDelete(nullptr);
            return;
        }

    }
}
} // namespace

void reset_button_start() {
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << CONFIG_SPA_RESET_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    button_edges = xQueueCreate(32, sizeof(ButtonEdge));
    ESP_ERROR_CHECK(button_edges ? ESP_OK : ESP_ERR_NO_MEM);
    auto err = gpio_install_isr_service(0);
    ESP_ERROR_CHECK(err == ESP_ERR_INVALID_STATE ? ESP_OK : err);
    ESP_ERROR_CHECK(gpio_isr_handler_add(static_cast<gpio_num_t>(CONFIG_SPA_RESET_GPIO), button_edge, nullptr));
    ESP_ERROR_CHECK(
        xTaskCreate(worker, "reset_button", 3072, nullptr, 2, nullptr) == pdPASS
            ? ESP_OK
            : ESP_ERR_NO_MEM);
}
