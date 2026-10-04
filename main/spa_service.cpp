#include "app.hpp"
#include "control_pipeline.hpp"
#include "serial_log.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <algorithm>
namespace {
SemaphoreHandle_t mutex;
QueueHandle_t requests;
Snapshot current;
constexpr uart_port_t port = UART_NUM_1;
const char *TAG = "spa_state";
void record(const std::string &tx, const std::string &rx, const std::string &result) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (current.transactions.size() == 16) current.transactions.erase(current.transactions.begin());
    current.transactions.push_back({now_ms(), tx, rx.substr(0, 1024), result});
    xSemaphoreGive(mutex);
}
std::string exchange(const std::string &command) {
    uart_flush_input(port);
    uart_write_bytes(port, "\n", 1);
    uart_wait_tx_done(port, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(50));
    const auto wire = command + "\n";
    uart_write_bytes(port, wire.data(), wire.size());
    std::string response; response.reserve(4096);
    const uint64_t start = now_ms(); uint64_t last = start;
    while (now_ms() - start < 4000) {
        char buffer[256]; int count = uart_read_bytes(port, buffer, sizeof(buffer), pdMS_TO_TICKS(30));
        if (count > 0) {
            response.append(buffer, count); last = now_ms();
            if (response.size() > 8192) break;
        } else if ((!response.empty() && now_ms() - last >= 250) || (response.empty() && now_ms() - start >= 1500)) break;
    }
    return response;
}
bool poll() {
    auto raw = exchange("RF"); spa::State state; std::string error;
    bool ok = spa::parse(raw, state, error);
    xSemaphoreTake(mutex, portMAX_DELAY);
    current.raw = raw.substr(0, 8192); current.error = error;
    current.valid = ok; // Retain last known state but explicitly mark the failed poll.
    if (ok) { current.state = state; current.last_valid_ms = now_ms(); ++current.polls_ok; }
    else ++current.polls_failed;
    xSemaphoreGive(mutex);
    record("RF", raw, ok ? "valid snapshot" : error);
    // UART worker owns these caches; never print placeholder state as live data.
    static bool seen = false, previously_valid = false;
    static std::string last_report;
    static spa_log::Periodic heartbeat;
    const bool periodic = heartbeat.due(now_ms());
    if (ok) {
        const auto report = spa_log::state(state);
        if (!seen || !previously_valid || report != last_report || periodic)
            ESP_LOGI(TAG, "RF readback valid: %s", report.c_str());
        last_report = report; seen = true;
    } else if (previously_valid || !seen || periodic) {
        // Error text is bounded and contains no network credentials.
        ESP_LOGW(TAG, "Spa unavailable: RF read failed (%s); controls unreachable, commands rejected", error.c_str());
        seen = true;
    }
    previously_valid = ok;
    matter_publish(); return ok;
}
std::string trimmed(std::string r) {
    auto first = r.find_first_not_of("\r\n ");
    if (first == r.npos) return {};
    auto last = r.find_last_not_of("\r\n "); return r.substr(first, last - first + 1);
}
void worker(void *) {
    for (;;) {
        poll();
        spa::Request r;
        if (xQueueReceive(requests, &r, pdMS_TO_TICKS(5000)) != pdTRUE) continue;
        const auto description = spa_log::request(r);
        ESP_LOGI("spa_command", "Executing %s", description.c_str());
        // Each stage has its own fresh RF and exact readback. No automatic retries.
        auto execute = [&](spa::Request stage, std::string &error) {
            if (!poll()) { error = "fresh poll failed"; return false; }
            spa::Command cmd;
            if (!spa::command(snapshot().state, stage, cmd, error)) return false;
            if (cmd.wire.empty()) {
                ESP_LOGI("spa_command", "%s already matches fresh RF; no write", spa_log::request(stage).c_str());
                return true;
            }
            ESP_LOGI("spa_command", "%s: sending %s", spa_log::request(stage).c_str(), cmd.wire.c_str());
            auto rx = exchange(cmd.wire);
            bool ack = trimmed(rx) == cmd.acknowledgement;
            if (!ack) ESP_LOGW("spa_command", "%s: ACK mismatch; checking RF, write will not be retried", spa_log::request(stage).c_str());
            record(cmd.wire, rx, ack ? "acknowledged; awaiting readback" : "acknowledgement mismatch; awaiting readback");
            // A lost ACK is ambiguous; always check the real state and never repeat the write.
            if (!poll() || !spa::matches(snapshot().state, stage)) {
                error = "controller readback did not confirm request"; return false;
            }
            return true;
        };
        std::string error;
        bool ok = controls::execute_request(r, execute, [] { return snapshot().state; }, error);
        xSemaphoreTake(mutex, portMAX_DELAY);
        if (ok) ++current.commands_ok;
        else { ++current.commands_failed; current.error = error; }
        xSemaphoreGive(mutex);
        if (ok) ESP_LOGI("spa_command", "Confirmed by RF: %s", description.c_str());
        else ESP_LOGW("spa_command", "Failed %s: %s", description.c_str(), error.c_str());
        record("request:" + std::to_string(int(r.control)) + ":" + std::to_string(r.value) + ":method=" + std::to_string(int(r.method)) + ":level=" + std::to_string(r.level), "", ok ? "confirmed" : error);
        matter_publish();
    }
}
}
uint64_t now_ms() { return esp_timer_get_time() / 1000; }
SpaIndicator spa_indicator() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    SpaIndicator result{current.valid && now_ms() - current.last_valid_ms <= 15000, current.last_valid_ms != 0};
    xSemaphoreGive(mutex);
    return result;
}
Snapshot snapshot() {
    xSemaphoreTake(mutex, portMAX_DELAY); auto s = current; xSemaphoreGive(mutex);
    s.queued_commands = uxQueueMessagesWaiting(requests);
    return s;
}
esp_err_t submit(spa::Request r) {
    auto s = snapshot();
    auto rejected = [&](esp_err_t result, const std::string &why) {
        ESP_LOGW("spa_command", "Rejected %s: %s", spa_log::request(r).c_str(), why.c_str());
        record("rejected:" + std::to_string(int(r.control)) + ":" + std::to_string(r.value) + ":method=" + std::to_string(int(r.method)) + ":level=" + std::to_string(r.level), "", why);
        return result;
    };
    if (!s.valid || now_ms() - s.last_valid_ms > 15000) return rejected(ESP_ERR_INVALID_STATE, "spa state stale or unavailable");
    spa::Command c; std::string error;
    if (!spa::command(s.state, r, c, error)) return rejected(ESP_ERR_INVALID_ARG, error);
    // Queue all requests, including apparent no-ops: an earlier queued write may change state.
    if (xQueueSend(requests, &r, 0) != pdTRUE) return rejected(ESP_ERR_NO_MEM, "command queue full");
    record("queued:" + std::to_string(int(r.control)) + ":" + std::to_string(r.value) + ":method=" + std::to_string(int(r.method)) + ":level=" + std::to_string(r.level), "", "accepted; not yet confirmed");
    ESP_LOGI("spa_command", "Queued %s; awaiting spa readback", spa_log::request(r).c_str());
    return ESP_OK;
}
void spa_start() {
    mutex = xSemaphoreCreateMutex(); requests = xQueueCreate(8, sizeof(spa::Request));
    configASSERT(mutex && requests);
    uart_config_t cfg{}; cfg.baud_rate = 38400; cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE; cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE; cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(port, CONFIG_SPA_TX_GPIO, CONFIG_SPA_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(port, 4096, 0, 0, nullptr, 0));
    // Worker starts only after Matter initialization; see app_main.
}
extern "C" void spa_worker_start() { ESP_ERROR_CHECK(xTaskCreate(worker, "spa_uart", 12288, nullptr, 5, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM); }
