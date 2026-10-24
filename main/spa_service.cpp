#include "app.hpp"
#include "control_pipeline.hpp"
#include "serial_log.hpp"
#include "uart_trace.hpp"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "nvs.h"
#include <atomic>
#include <algorithm>
namespace {
SemaphoreHandle_t mutex;
QueueHandle_t requests;
Snapshot current;
constexpr uart_port_t port = UART_NUM_1;
const char *TAG = "spa_state";
std::atomic<uint32_t> poll_seconds{15};
std::atomic<uint64_t> rejected_update_poll_due_ms{0};
constexpr uint32_t MIN_POLL_SECONDS = 15, MAX_POLL_SECONDS = 300;
constexpr uint32_t SPA_COMMAND_GAP_MS = 100;
constexpr uint32_t SPA_FIRST_BYTE_TIMEOUT_MS = 5000;
constexpr uint32_t SPA_RESPONSE_MAX_MS = 6000;
uint64_t freshness_limit_ms() { return uint64_t(poll_seconds.load()) * 1000 + 10000; }
constexpr size_t UART_TRACE_TX_LIMIT = 64;
constexpr size_t UART_TRACE_RX_LIMIT = 512;
constexpr size_t UART_LOG_RESPONSE_PREVIEW_BYTES = 96;
void record(const std::string &tx, const std::string &rx, const std::string &result,
            const std::string &tx_bytes = {}, const std::string &rx_bytes = {}) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (current.transactions.size() == 16) current.transactions.erase(current.transactions.begin());
    const auto tx_hex = spa_trace::hex(tx_bytes, UART_TRACE_TX_LIMIT);
    const auto rx_hex = spa_trace::hex(rx_bytes, UART_TRACE_RX_LIMIT);
    current.transactions.push_back({now_ms(), tx, rx.substr(0, 1024), result,
                                    tx_hex.value, rx_hex.value, tx_hex.truncated, rx_hex.truncated});
    xSemaphoreGive(mutex);
}
struct ExchangeResult { std::string response, transmitted; };
ExchangeResult exchange(const std::string &command) {
    const uint64_t exchange_start = now_ms();
    ESP_LOGI("spa_uart", "exchange start command=%s", command.c_str());
    uart_flush_input(port);
    ExchangeResult result;
    result.transmitted = "\n";
    uart_write_bytes(port, result.transmitted.data(), result.transmitted.size());
    uart_wait_tx_done(port, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(50));
    const auto wire = command + "\n";
    uart_write_bytes(port, wire.data(), wire.size());
    result.transmitted += wire;
    result.response.reserve(4096);
    const uint64_t start = now_ms(); uint64_t last = start;
    while (now_ms() - start < SPA_RESPONSE_MAX_MS) {
        char buffer[256]; int count = uart_read_bytes(port, buffer, sizeof(buffer), pdMS_TO_TICKS(30));
        if (count > 0) {
            result.response.append(buffer, count); last = now_ms();
            if (result.response.size() > 8192) break;
        } else if ((!result.response.empty() && now_ms() - last >= 250) ||
                   (result.response.empty() && now_ms() - start >= SPA_FIRST_BYTE_TIMEOUT_MS)) break;
    }
    const auto preview = spa_trace::hex(result.response, UART_LOG_RESPONSE_PREVIEW_BYTES);
    ESP_LOGI("spa_uart", "exchange finish command=%s response_bytes=%u elapsed_ms=%llu response_prefix_hex=%s%s",
             command.c_str(), static_cast<unsigned>(result.response.size()),
             static_cast<unsigned long long>(now_ms() - exchange_start), preview.value.c_str(),
             preview.truncated ? " [truncated]" : "");
    // Give SpaNET time to settle before any next request, including another RF.
    vTaskDelay(pdMS_TO_TICKS(SPA_COMMAND_GAP_MS));
    return result;
}
bool poll() {
    auto exchange_result = exchange("RF");
    auto &raw = exchange_result.response;
    spa::State state; std::string error;
    bool ok = spa::parse(raw, state, error);
    xSemaphoreTake(mutex, portMAX_DELAY);
    current.raw = raw.substr(0, 8192); current.error = error;
    current.valid = ok; // Retain last known state but explicitly mark the failed poll.
    if (ok) {
        current.state = state; current.command_acknowledged = false;
        current.last_valid_ms = now_ms(); ++current.polls_ok;
    }
    else ++current.polls_failed;
    xSemaphoreGive(mutex);
    record("RF", raw, ok ? "valid snapshot" : error, exchange_result.transmitted, raw);
    if (ok) {
        ESP_LOGI(TAG, "RF response accepted: response_bytes=%u",
                 static_cast<unsigned>(raw.size()));
    } else {
        // Parser errors are fixed, bounded reason strings; log every failed
        // response so intermittent malformed RF data is visible even while
        // the higher-level availability warning is being throttled.
        ESP_LOGE(TAG, "RF response rejected: reason=%s response_bytes=%u",
                 error.empty() ? "unspecified parser error" : error.c_str(),
                 static_cast<unsigned>(raw.size()));
    }
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
    uint64_t next_poll = 0;
    spa::State working;
    bool failure_poll_pending = false;
    for (;;) {
        const uint64_t rejected_due = rejected_update_poll_due_ms.exchange(0);
        if (rejected_due != 0) {
            next_poll = failure_poll_pending ? std::max(next_poll, rejected_due) : rejected_due;
            failure_poll_pending = true;
        }
        auto run_periodic_poll = [&] {
            poll();
            working = snapshot().state;
            next_poll = now_ms() + uint64_t(poll_seconds.load()) * 1000;
            failure_poll_pending = false;
        };
        if (failure_poll_pending && now_ms() >= next_poll) {
            run_periodic_poll();
            continue;
        }
        spa::Request r;
        // Queued attribute updates take priority when a periodic poll becomes due,
        // so a scheduled RF never acts as an update pre-read.
        if (xQueueReceive(requests, &r, 0) != pdTRUE) {
            const uint64_t now = now_ms();
            if (now >= next_poll) {
                run_periodic_poll();
                continue;
            }
            const uint64_t remaining = next_poll - now;
            if (xQueueReceive(requests, &r,
                    pdMS_TO_TICKS(static_cast<uint32_t>(std::min<uint64_t>(remaining, 1000)))) != pdTRUE)
                continue;
        }
        const auto description = spa_log::request(r);
        ESP_LOGI("spa_command", "Executing %s", description.c_str());
        bool command_acked = false;
        auto send = [&](const spa::Command &cmd) {
            ESP_LOGI("spa_command", "Sending %s", cmd.wire.c_str());
            auto exchange_result = exchange(cmd.wire);
            auto &rx = exchange_result.response;
            const bool ack = trimmed(rx) == cmd.acknowledgement;
            command_acked = ack;
            record(cmd.wire, rx, ack ? "acknowledged" : "acknowledgement mismatch; no RF requested",
                   exchange_result.transmitted, rx);
            return ack;
        };
        auto execute = [&](spa::Request stage, std::string &why) {
            command_acked = false;
            const bool completed = controls::execute_stage(stage, working, send, why);
            if (completed && command_acked) {
                // Matter publishes this cached value on the CHIP task immediately.
                // The next valid RF replaces it and clears the ACK marker.
                xSemaphoreTake(mutex, portMAX_DELAY);
                current.state = working;
                current.command_acknowledged = true;
                xSemaphoreGive(mutex);
                matter_publish();
            }
            return completed;
        };
        std::string error;
        const auto evidence = snapshot();
        bool ok = false;
        if (!evidence.valid || now_ms() - evidence.last_valid_ms > freshness_limit_ms())
            error = "spa state stale or unavailable";
        else ok = controls::execute_request(r, working, execute, error);
        if (!ok) {
            // Do not issue RF inside the failed update. Advance the next normal
            // periodic poll to one second after the failure instead.
            next_poll = controls::next_poll_after_failure(now_ms());
            failure_poll_pending = true;
        }
        xSemaphoreTake(mutex, portMAX_DELAY);
        if (ok) ++current.commands_ok;
        else { ++current.commands_failed; current.error = error; }
        xSemaphoreGive(mutex);
        if (ok) ESP_LOGI("spa_command", "Completed (ACK or cached no-op): %s", description.c_str());
        else ESP_LOGW("spa_command", "Failed %s: %s; next scheduled RF poll in 1000 ms", description.c_str(), error.c_str());
        record("request:" + std::to_string(int(r.control)) + ":" + std::to_string(r.value) + ":method=" + std::to_string(int(r.method)) + ":level=" + std::to_string(r.level), "", ok ? "completed; periodic RF reconciliation pending" : error);
        matter_publish();
    }
}
}
uint64_t now_ms() { return esp_timer_get_time() / 1000; }
SpaIndicator spa_indicator() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    SpaIndicator result{current.valid && now_ms() - current.last_valid_ms <= freshness_limit_ms(), current.last_valid_ms != 0};
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
    if (!s.valid || now_ms() - s.last_valid_ms > freshness_limit_ms()) {
        rejected_update_poll_due_ms.store(controls::next_poll_after_failure(now_ms()));
        return rejected(ESP_ERR_INVALID_STATE, "spa state stale or unavailable");
    }
    spa::Command c; std::string error;
    if (!spa::command(s.state, r, c, error)) return rejected(ESP_ERR_INVALID_ARG, error);
    // Queue all requests, including apparent no-ops: an earlier queued write may change state.
    if (xQueueSend(requests, &r, 0) != pdTRUE) return rejected(ESP_ERR_NO_MEM, "command queue full");
    record("queued:" + std::to_string(int(r.control)) + ":" + std::to_string(r.value) + ":method=" + std::to_string(int(r.method)) + ":level=" + std::to_string(r.level), "", "accepted; not yet confirmed");
    ESP_LOGI("spa_command", "Queued %s; awaiting spa acknowledgement", spa_log::request(r).c_str());
    return ESP_OK;
}
uint32_t spa_poll_interval_seconds() { return poll_seconds.load(); }
esp_err_t spa_set_poll_interval_seconds(uint32_t seconds) {
    if (seconds < MIN_POLL_SECONDS || seconds > MAX_POLL_SECONDS) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("spa_config", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, "poll_sec", seconds);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) poll_seconds.store(seconds);
    return err;
}
void spa_start() {
    nvs_handle_t handle;
    if (nvs_open("spa_config", NVS_READONLY, &handle) == ESP_OK) {
        uint32_t saved = 15;
        if (nvs_get_u32(handle, "poll_sec", &saved) == ESP_OK && saved >= MIN_POLL_SECONDS && saved <= MAX_POLL_SECONDS)
            poll_seconds.store(saved);
        nvs_close(handle);
    }
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
