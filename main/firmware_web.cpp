#include "app.hpp"

#include "cJSON.h"
#include "esp_log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace {
const char *TAG = "firmware_web";

esp_err_t info(httpd_req_t *req) {
    const auto body = firmware_info_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t channel(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 128) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }
    char body[129]{};
    int got = httpd_req_recv(req, body, req->content_len);
    if (got != req->content_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete request body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_ParseWithLength(body, got);
    cJSON *item = root ? cJSON_GetObjectItemCaseSensitive(root, "channel") : nullptr;
    if (!cJSON_IsString(item) || !item->valuestring) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channel must be release or development");
        return ESP_FAIL;
    }
    const esp_err_t err = firmware_set_update_channel(item->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t update(httpd_req_t *req) {
    const esp_err_t err = firmware_start_update();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

esp_err_t upload(httpd_req_t *req) {
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware file is empty");
        return ESP_FAIL;
    }
    char expected_sha256[65]{};
    if (httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256") != 64 ||
        httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", expected_sha256, sizeof(expected_sha256)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Firmware-SHA256 must contain 64 hex characters");
        return ESP_FAIL;
    }
    const esp_err_t begin_err = firmware_upload_begin(static_cast<size_t>(req->content_len), expected_sha256);
    if (begin_err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, esp_err_to_name(begin_err));
        return ESP_FAIL;
    }

    std::array<uint8_t, 2048> buffer{};
    size_t remaining = static_cast<size_t>(req->content_len);
    while (remaining) {
        const int requested = static_cast<int>(std::min(remaining, buffer.size()));
        const int received = httpd_req_recv(req, reinterpret_cast<char *>(buffer.data()), requested);
        if (received <= 0) {
            firmware_upload_abort();
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware upload interrupted");
            return ESP_FAIL;
        }
        const esp_err_t write_err = firmware_upload_write(buffer.data(), static_cast<size_t>(received));
        if (write_err != ESP_OK) {
            firmware_upload_abort();
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, esp_err_to_name(write_err));
            return ESP_FAIL;
        }
        remaining -= static_cast<size_t>(received);
    }
    const esp_err_t finish_err = firmware_upload_finish();
    if (finish_err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, esp_err_to_name(finish_err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Local firmware image accepted; reboot scheduled");
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true,\"source\":\"local-upload\"}");
}

esp_err_t alternate(httpd_req_t *req) {
    const esp_err_t err = firmware_reboot_alternate();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"rebooting\":true}");
}

esp_err_t reboot(httpd_req_t *req) {
    const esp_err_t err = firmware_reboot_current();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"rebooting\":true}");
}
} // namespace

void firmware_web_register(httpd_handle_t server) {
    httpd_uri_t fw{}; fw.uri = "/api/firmware"; fw.method = HTTP_GET; fw.handler = info;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &fw));
    httpd_uri_t ch{}; ch.uri = "/api/firmware/channel"; ch.method = HTTP_POST; ch.handler = channel;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ch));
    httpd_uri_t up{}; up.uri = "/api/firmware/update"; up.method = HTTP_POST; up.handler = update;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &up));
    httpd_uri_t file{}; file.uri = "/api/firmware/upload"; file.method = HTTP_POST; file.handler = upload;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &file));
    httpd_uri_t alt{}; alt.uri = "/api/firmware/alternate"; alt.method = HTTP_POST; alt.handler = alternate;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &alt));
    httpd_uri_t rb{}; rb.uri = "/api/reboot"; rb.method = HTTP_POST; rb.handler = reboot;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rb));
}
