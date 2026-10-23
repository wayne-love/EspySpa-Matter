#include "ota_http.hpp"
#include "thread_dns.hpp"
#include "esp_log.h"

void ota_http_configure(esp_http_client_config_t &config) {
    config.buffer_size_tx = OTA_HTTP_TX_BUFFER_SIZE;
    config.max_redirection_count = 5;
    config.disable_auto_redirect = true;
}

esp_err_t ota_http_open_stream(esp_http_client_handle_t client, int64_t &length, const char *stage) {
    constexpr unsigned MAX_REDIRECTS = 5;
    for (unsigned hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        // Check AFTER set_redirection too: never connect to an HTTP downgrade.
        // Keep the real URL hostname for TLS SNI and certificate verification.
        if (esp_http_client_get_transport_type(client) != HTTP_TRANSPORT_OVER_SSL) {
            ESP_LOGE("firmware", "%s hop=%u rejected non-HTTPS transport", stage, hop);
            return ESP_ERR_INVALID_ARG;
        }
        const auto error = esp_http_client_open(client, 0);
        if (error != ESP_OK) {
            ESP_LOGE("firmware", "%s hop=%u open/request failed: %s; last_dns=%s tx_buffer=%d",
                     stage, hop, esp_err_to_name(error), thread_dns_last_result(), OTA_HTTP_TX_BUFFER_SIZE);
            return error;
        }
        length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (length >= 0 && status >= 200 && status < 300) return ESP_OK;
        if (length < 0 || status < 300 || status >= 400 || hop == MAX_REDIRECTS) {
            esp_http_client_close(client);
            return ESP_ERR_INVALID_RESPONSE;
        }
        const auto redirect_error = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (redirect_error != ESP_OK) return redirect_error;
    }
    return ESP_ERR_INVALID_RESPONSE;
}
