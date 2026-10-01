#pragma once
#include "spa_protocol.hpp"
#include "esp_err.h"
#include <string>
#include <vector>
struct Transaction { uint64_t ms; std::string tx, rx, result; };
struct Snapshot {
    spa::State state;
    bool valid = false;
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
void matter_start();
void matter_publish();
void diagnostics_start();
std::string matter_status();
