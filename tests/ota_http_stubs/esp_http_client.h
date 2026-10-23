#pragma once
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_FAIL = -1, ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_INVALID_RESPONSE = 2,
                    ESP_ERR_HTTP_CONNECT = 3;
struct esp_http_client_config_t {
    int buffer_size_tx = 0;
    int max_redirection_count = 0;
    bool disable_auto_redirect = false;
};
enum esp_http_client_transport_t { HTTP_TRANSPORT_OVER_SSL, HTTP_TRANSPORT_OVER_TCP };
struct FakeHttp;
using esp_http_client_handle_t = FakeHttp *;
esp_http_client_transport_t esp_http_client_get_transport_type(esp_http_client_handle_t);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t);
const char *esp_err_to_name(esp_err_t);
