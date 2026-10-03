#include "app.hpp"
#include "status_indicator.hpp"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/thread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <atomic>
namespace {
const char *TAG = "status_led";
led_strip_handle_t strip;
std::atomic<indicator::Mode> mode{indicator::Mode::Starting};
std::atomic<int> driver_error{ESP_OK};
std::atomic<bool> reset_pending{false};
static_assert(CONFIG_SPA_TX_GPIO != CONFIG_SPA_RX_GPIO, "Spa TX and RX must differ");
static_assert(CONFIG_SPA_LED_GPIO != CONFIG_SPA_TX_GPIO && CONFIG_SPA_LED_GPIO != CONFIG_SPA_RX_GPIO,
              "Status LED must not share a spa UART pin");
void worker(void *) {
    bool attached = false, thread_seen = false, last_lit = false;
    auto previous = indicator::Mode::Starting;
    uint64_t next_sample = 0, changed_at = now_ms();
    bool first = true;
    while (true) {
        const auto now = now_ms();
        if (now >= next_sample) {
            next_sample = now + 1000;
            auto m = matter_indicator();
            if (m.initialized && esp_openthread_lock_acquire(pdMS_TO_TICKS(50))) {
                auto ot = esp_openthread_get_instance();
                auto role = ot ? otThreadGetDeviceRole(ot) : OT_DEVICE_ROLE_DISABLED;
                attached = role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
                esp_openthread_lock_release();
            }
            thread_seen = thread_seen || attached;
            auto spa = spa_indicator();
            mode = indicator::select({m.initialized, m.paired, m.window_open, m.pairing,
                m.failed_at_ms != 0 && now - m.failed_at_ms < 10000,
                attached, thread_seen, spa.fresh, spa.seen});
        }
        auto current = reset_pending.load() ? indicator::Mode::FactoryReset : mode.load();
        const bool changed = current != previous;
        if (changed) {
            changed_at = now;
            ESP_LOGI(TAG, "Status: %s", indicator::pattern(current).name);
        }
        auto p = indicator::pattern(current);
        const bool on = indicator::lit(p, now - changed_at);
        if (first || changed || on != last_lit) {
            auto err = led_strip_set_pixel(strip, 0, on ? p.red * CONFIG_SPA_LED_BRIGHTNESS : 0,
                on ? p.green * CONFIG_SPA_LED_BRIGHTNESS : 0, on ? p.blue * CONFIG_SPA_LED_BRIGHTNESS : 0);
            if (err == ESP_OK) err = led_strip_refresh(strip);
            driver_error = err;
            if (err != ESP_OK) { ESP_LOGE(TAG, "LED output failed: %s", esp_err_to_name(err)); vTaskDelete(nullptr); }
        }
        first = false; previous = current; last_lit = on;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}
void status_led_reset_pending(bool pending) { reset_pending = pending; }
std::string status_led_json() {
    auto p = indicator::pattern(reset_pending.load() ? indicator::Mode::FactoryReset : mode.load());
    return "{\"gpio\":" + std::to_string(CONFIG_SPA_LED_GPIO) + ",\"status\":\"" + p.name +
        "\",\"flashes\":" + std::to_string(p.flashes) + ",\"driver_error\":" + std::to_string(driver_error.load()) + "}";
}
void status_led_start() {
    led_strip_config_t cfg{};
    cfg.strip_gpio_num = CONFIG_SPA_LED_GPIO; cfg.max_leds = 1;
    cfg.led_pixel_format = LED_PIXEL_FORMAT_GRB; cfg.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rmt{};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT; rmt.resolution_hz = 10000000;
    auto err = led_strip_new_rmt_device(&cfg, &rmt, &strip);
    if (err == ESP_OK && xTaskCreate(worker, "status_led", 3072, nullptr, 2, nullptr) != pdPASS) err = ESP_ERR_NO_MEM;
    // Do not overwrite a render error if the new worker already ran.
    // Indicator faults must not stop spa control or network diagnostics.
    if (err != ESP_OK) {
        driver_error = err;
        ESP_LOGE(TAG, "LED initialization failed: %s", esp_err_to_name(err));
    }
}
