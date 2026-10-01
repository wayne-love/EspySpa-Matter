#include "app.hpp"
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
        // A fresh read is essential for W14 (toggle), also validates current capabilities.
        bool ok = poll(); std::string error = "fresh poll failed"; spa::Command cmd;
        if (ok) ok = spa::command(snapshot().state, r, cmd, error);
        if (ok && !cmd.wire.empty()) {
            auto rx = exchange(cmd.wire);
            bool ack = trimmed(rx) == cmd.acknowledgement;
            record(cmd.wire, rx, ack ? "acknowledged; awaiting readback" : "acknowledgement mismatch; awaiting readback");
            // Never retry a toggle; even a lost ACK can mean the command took effect.
            ok = poll() && spa::matches(snapshot().state, r);
            if (!ok) error = "controller readback did not confirm request";
        }
        xSemaphoreTake(mutex, portMAX_DELAY);
        if (ok) ++current.commands_ok;
        else { ++current.commands_failed; current.error = error; }
        xSemaphoreGive(mutex);
        record("request:" + std::to_string(int(r.control)) + ":" + std::to_string(r.value), "", ok ? "confirmed" : error);
        matter_publish();
    }
}
}
uint64_t now_ms() { return esp_timer_get_time() / 1000; }
Snapshot snapshot() {
    xSemaphoreTake(mutex, portMAX_DELAY); auto s = current; xSemaphoreGive(mutex); return s;
}
esp_err_t submit(spa::Request r) {
    auto s = snapshot();
    if (!s.valid || now_ms() - s.last_valid_ms > 15000) return ESP_ERR_INVALID_STATE;
    spa::Command c; std::string error;
    if (!spa::command(s.state, r, c, error)) return ESP_ERR_INVALID_ARG;
    // Queue all requests, including apparent no-ops: an earlier queued write may change state.
    return xQueueSend(requests, &r, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
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
extern "C" void spa_worker_start() { configASSERT(xTaskCreate(worker, "spa_uart", 12288, nullptr, 5, nullptr) == pdPASS); }
