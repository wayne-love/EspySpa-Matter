#pragma once
#include "esp_http_client.h"
#include <cstdint>

// GitHub signed release-asset redirect paths/queries exceed IDF's 512-byte
// default. Fixed heap allocation per client; never log signed URLs.
constexpr int OTA_HTTP_TX_BUFFER_SIZE = 8192;
void ota_http_configure(esp_http_client_config_t &config);

// Shared by manifest and image downloads; every hop must remain HTTPS.
esp_err_t ota_http_open_stream(esp_http_client_handle_t client, int64_t &length, const char *stage);
