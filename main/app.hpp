#pragma once
#include "spa_protocol.hpp"
#include "esp_err.h"
#include "esp_http_server.h"
#include <cstddef>
#include <string>
#include <vector>

struct Transaction {
    uint64_t ms;
    std::string tx, rx, result;
    std::string tx_hex, rx_hex;
    bool tx_truncated = false, rx_truncated = false;
};
struct Snapshot {
    spa::State state;
    bool valid = false;
    // True while state reflects a successful spa ACK newer than the last RF read.
    bool command_acknowledged = false;
    uint64_t last_valid_ms = 0;
    uint32_t queued_commands = 0;
    uint32_t polls_ok = 0, polls_failed = 0, commands_ok = 0, commands_failed = 0;
    std::string raw, error;
    std::vector<Transaction> transactions;
};

uint64_t now_ms();
Snapshot snapshot();
esp_err_t submit(spa::Request request);
void spa_start();
uint32_t spa_poll_interval_seconds();
esp_err_t spa_set_poll_interval_seconds(uint32_t seconds);
void matter_start();
void matter_publish();
void diagnostics_start();
void firmware_web_register(httpd_handle_t server);
std::string matter_status();

struct MatterIndicator { bool initialized, paired, window_open, pairing; uint64_t failed_at_ms; };
struct SpaIndicator { bool fresh, seen; };
MatterIndicator matter_indicator();
SpaIndicator spa_indicator();
void status_led_start();
std::string status_led_json();

void reset_button_start();
void status_led_reset_pending(bool pending);

void firmware_update_start();
std::string firmware_info_json();
std::string firmware_update_channel();
esp_err_t firmware_set_update_channel(const std::string &channel);
esp_err_t firmware_start_update();
esp_err_t firmware_upload_begin(size_t content_length, const std::string &expected_sha256);
esp_err_t firmware_upload_write(const void *data, size_t length);
esp_err_t firmware_upload_finish();
void firmware_upload_abort();
esp_err_t firmware_reboot_alternate();
esp_err_t firmware_reboot_current();
