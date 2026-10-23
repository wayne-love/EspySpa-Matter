#include "app.hpp"
#include "ota_http.hpp"
#include "thread_dns.hpp"
#include "firmware_progress.hpp"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

#ifndef ESPYSPA_FIRMWARE_CHANNEL
#define ESPYSPA_FIRMWARE_CHANNEL "development"
#endif
#ifndef ESPYSPA_GIT_COMMIT
#define ESPYSPA_GIT_COMMIT "unknown"
#endif

namespace {
constexpr const char *TAG = "firmware";
constexpr const char *NVS_NAMESPACE = "firmware";
constexpr const char *NVS_CHANNEL_KEY = "channel";
constexpr size_t MANIFEST_LIMIT = 4096;
constexpr uint32_t VALIDATION_DELAY_MS = 30000;

SemaphoreHandle_t state_mutex;
bool update_busy = false;
std::string update_state = "idle";
std::string update_error;
std::string update_target_version;
FirmwareDownloadProgress download_progress;

struct UploadState {
    esp_ota_handle_t handle = 0;
    const esp_partition_t *target = nullptr;
    mbedtls_sha256_context sha{};
    size_t expected_length = 0;
    size_t written = 0;
    std::string expected_sha256;
    bool active = false;
};
UploadState upload_state_data;

struct Manifest {
    std::string version;
    std::string channel;
    std::string url;
    std::string sha256;
    std::string target;
};

void lock_state() {
    if (state_mutex) xSemaphoreTake(state_mutex, portMAX_DELAY);
}
void unlock_state() {
    if (state_mutex) xSemaphoreGive(state_mutex);
}
void set_state(const char *state, const std::string &error = {}, const std::string &version = {}) {
    lock_state();
    update_state = state;
    update_error = error;
    if (!version.empty()) update_target_version = version;
    unlock_state();
}

std::string json_escape(const std::string &value) {
    cJSON *item = cJSON_CreateString(value.c_str());
    if (!item) return "";
    char *encoded = cJSON_PrintUnformatted(item);
    std::string out = encoded ? encoded : """";
    if (encoded) cJSON_free(encoded);
    cJSON_Delete(item);
    return out;
}

std::string image_channel() {
    return ESPYSPA_FIRMWARE_CHANNEL;
}

std::string load_channel() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return image_channel();
    char value[16]{};
    size_t length = sizeof(value);
    esp_err_t err = nvs_get_str(handle, NVS_CHANNEL_KEY, value, &length);
    nvs_close(handle);
    if (err != ESP_OK) return image_channel();
    if (strcmp(value, "release") && strcmp(value, "development")) return image_channel();
    return value;
}

const char *manifest_url_for(const std::string &channel) {
    return channel == "release" ? CONFIG_SPA_RELEASE_MANIFEST_URL : CONFIG_SPA_DEVELOPMENT_MANIFEST_URL;
}

esp_err_t fetch_text(const char *url, std::string &body) {
    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    ota_http_configure(config);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    int64_t content_length = -1;
    esp_err_t err = ota_http_open_stream(client, content_length, "manifest");
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    if (content_length > static_cast<int64_t>(MANIFEST_LIMIT)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    std::array<char, 512> buffer{};
    body.clear();
    while (true) {
        int got = esp_http_client_read(client, buffer.data(), buffer.size());
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        if (got == 0) break;
        if (body.size() + static_cast<size_t>(got) > MANIFEST_LIMIT) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        body.append(buffer.data(), got);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

bool valid_hex_sha256(const std::string &value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); });
}

esp_err_t parse_manifest(const std::string &body, Manifest &manifest) {
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    auto get = [root](const char *name) -> const char * {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
        return cJSON_IsString(item) && item->valuestring ? item->valuestring : nullptr;
    };

    const char *version = get("version");
    const char *channel = get("channel");
    const char *url = get("url");
    const char *sha256 = get("sha256");
    const char *target = get("target");

    if (!version || !channel || !url || !sha256 || !target) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    manifest = {version, channel, url, sha256, target};
    cJSON_Delete(root);

    if ((manifest.channel != "release" && manifest.channel != "development") ||
        manifest.target != "esp32c6" || !valid_hex_sha256(manifest.sha256) ||
        manifest.url.rfind("https://", 0) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

std::string digest_hex(const unsigned char digest[32]) {
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return out;
}

esp_err_t download_to_inactive_slot(const Manifest &manifest) {
    lock_state();
    download_progress = {true, 0, 0};
    unlock_state();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (!target || target == running) return ESP_ERR_NOT_FOUND;

    esp_http_client_config_t config{};
    config.url = manifest.url.c_str();
    config.timeout_ms = 30000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    ota_http_configure(config);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    int64_t length = -1;
    esp_err_t err = ota_http_open_stream(client, length, "image");
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    if (length > 0 && length > static_cast<int64_t>(target->size)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    lock_state();
    download_progress.total_bytes = length > 0 ? static_cast<uint32_t>(length) : 0;
    unlock_state();

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        mbedtls_sha256_free(&sha);
        return ESP_FAIL;
    }

    std::array<char, 4096> buffer{};
    size_t written = 0;
    while (true) {
        int got = esp_http_client_read(client, buffer.data(), buffer.size());
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        if (got == 0) break;
        written += static_cast<size_t>(got);
        if (written > target->size) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char *>(buffer.data()), got) != 0 ||
            esp_ota_write(ota_handle, buffer.data(), got) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        lock_state();
        download_progress.received_bytes = static_cast<uint32_t>(written);
        unlock_state();
    }

    if (err == ESP_OK && length > 0 && written != static_cast<size_t>(length)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) set_state("verifying");
    unsigned char digest[32]{};
    if (err == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) err = ESP_FAIL;
    mbedtls_sha256_free(&sha);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && digest_hex(digest) != manifest.sha256) {
        ESP_LOGE(TAG, "Downloaded image SHA-256 does not match manifest");
        err = ESP_ERR_INVALID_CRC;
    }

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) return err;

    err = esp_ota_set_boot_partition(target);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image %s installed in %s; reboot required", manifest.version.c_str(), target->label);
    }
    return err;
}

void restart_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

esp_err_t schedule_restart() {
    return xTaskCreate(restart_task, "fw_restart", 2048, nullptr, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void clear_upload_state() {
    if (upload_state_data.active) {
        esp_ota_abort(upload_state_data.handle);
        mbedtls_sha256_free(&upload_state_data.sha);
    }
    upload_state_data = UploadState{};
}

void update_task(void *) {
    const std::string channel = load_channel();
    set_state("checking", {}, {});

    std::string body;
    const char *phase = "manifest fetch";
    esp_err_t err = fetch_text(manifest_url_for(channel), body);
    Manifest manifest;
    if (err == ESP_OK) {
        phase = "manifest validation";
        err = parse_manifest(body, manifest);
    }
    if (err == ESP_OK && manifest.channel != channel) err = ESP_ERR_INVALID_RESPONSE;

    if (err == ESP_OK) {
        phase = "firmware download or image verification";
        set_state("downloading", {}, manifest.version);
        err = download_to_inactive_slot(manifest);
    }

    if (err == ESP_OK) {
        set_state("rebooting", {}, manifest.version);
        phase = "restart scheduling";
        err = schedule_restart();
        if (err != ESP_OK) esp_ota_set_boot_partition(esp_ota_get_running_partition());
    }
    if (err != ESP_OK) {
        const std::string message = std::string(phase) + ": " + esp_err_to_name(err);
        ESP_LOGE(TAG, "Firmware update failed during %s: %s", phase, esp_err_to_name(err));
        set_state("failed", message, manifest.version);
    }

    lock_state();
    update_busy = false;
    unlock_state();
    vTaskDelete(nullptr);
}

void validation_task(void *) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "OTA image pending verification; validating for %lu seconds",
                 static_cast<unsigned long>(VALIDATION_DELAY_MS / 1000));
        vTaskDelay(pdMS_TO_TICKS(VALIDATION_DELAY_MS));

        if (matter_indicator().initialized) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) ESP_LOGI(TAG, "OTA image marked valid");
            else ESP_LOGE(TAG, "Unable to confirm OTA image: %s", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "Matter did not initialize; leaving OTA image unconfirmed for rollback");
        }
    }
    vTaskDelete(nullptr);
}

bool partition_is_bootable(const esp_partition_t *partition) {
    if (!partition) return false;

    esp_app_desc_t desc{};
    if (esp_ota_get_partition_description(partition, &desc) != ESP_OK) return false;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_err = esp_ota_get_state_partition(partition, &state);
    if (state_err == ESP_OK &&
        (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)) {
        return false;
    }
    return true;
}

const esp_partition_t *alternate_partition() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return nullptr;
    esp_partition_subtype_t other =
        running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0
            ? ESP_PARTITION_SUBTYPE_APP_OTA_1
            : ESP_PARTITION_SUBTYPE_APP_OTA_0;
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, other, nullptr);
    return partition_is_bootable(partition) ? partition : nullptr;
}
} // namespace

void firmware_update_start() {
    if (!state_mutex) state_mutex = xSemaphoreCreateMutex();
    if (!state_mutex) {
        ESP_LOGE(TAG, "Unable to create firmware state mutex");
        return;
    }
    if (xTaskCreate(validation_task, "ota_validate", 3072, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create OTA validation task");
    }
}

std::string firmware_update_channel() {
    return load_channel();
}

esp_err_t firmware_set_update_channel(const std::string &channel) {
    if (channel != "release" && channel != "development") return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, NVS_CHANNEL_KEY, channel.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t firmware_start_update() {
    if (!state_mutex) return ESP_ERR_INVALID_STATE;

    lock_state();
    if (update_busy) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    update_busy = true;
    update_state = "queued";
    download_progress = {};
    update_error.clear();
    update_target_version.clear();
    unlock_state();

    if (xTaskCreate(update_task, "ota_update", 8192, nullptr, 3, nullptr) != pdPASS) {
        lock_state();
        update_busy = false;
        update_state = "failed";
        update_error = "task allocation failed";
        unlock_state();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t firmware_upload_begin(size_t content_length, const std::string &expected_sha256) {
    if (!state_mutex || content_length == 0 || !valid_hex_sha256(expected_sha256)) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (!target || target == running) return ESP_ERR_NOT_FOUND;
    if (content_length > target->size) return ESP_ERR_INVALID_SIZE;

    lock_state();
    if (update_busy) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    update_busy = true;
    update_state = "uploading";
    download_progress = {};
    update_error.clear();
    update_target_version.clear();
    unlock_state();

    clear_upload_state();
    upload_state_data.expected_length = content_length;
    upload_state_data.expected_sha256 = expected_sha256;
    std::transform(upload_state_data.expected_sha256.begin(), upload_state_data.expected_sha256.end(),
                   upload_state_data.expected_sha256.begin(), [](unsigned char c) { return std::tolower(c); });
    upload_state_data.target = target;

    esp_err_t err = esp_ota_begin(target, content_length, &upload_state_data.handle);
    const bool ota_started = err == ESP_OK;
    if (err == ESP_OK) {
        mbedtls_sha256_init(&upload_state_data.sha);
        if (mbedtls_sha256_starts(&upload_state_data.sha, 0) != 0) err = ESP_FAIL;
        else upload_state_data.active = true;
    }
    if (err != ESP_OK) {
        if (ota_started) esp_ota_abort(upload_state_data.handle);
        mbedtls_sha256_free(&upload_state_data.sha);
        upload_state_data = UploadState{};
        lock_state();
        update_busy = false;
        update_state = "failed";
        update_error = esp_err_to_name(err);
        unlock_state();
    }
    return err;
}

esp_err_t firmware_upload_write(const void *data, size_t length) {
    if (!upload_state_data.active || !data || length == 0 ||
        length > upload_state_data.expected_length - upload_state_data.written) return ESP_ERR_INVALID_ARG;
    if (mbedtls_sha256_update(&upload_state_data.sha, static_cast<const unsigned char *>(data), length) != 0) {
        return ESP_FAIL;
    }
    const esp_err_t err = esp_ota_write(upload_state_data.handle, data, length);
    if (err == ESP_OK) upload_state_data.written += length;
    return err;
}

esp_err_t firmware_upload_finish() {
    if (!upload_state_data.active) return ESP_ERR_INVALID_STATE;
    if (upload_state_data.written != upload_state_data.expected_length) {
        firmware_upload_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    unsigned char digest[32]{};
    esp_err_t err = mbedtls_sha256_finish(&upload_state_data.sha, digest) == 0 ? ESP_OK : ESP_FAIL;
    mbedtls_sha256_free(&upload_state_data.sha);
    if (err == ESP_OK && digest_hex(digest) != upload_state_data.expected_sha256) err = ESP_ERR_INVALID_CRC;

    const esp_ota_handle_t handle = upload_state_data.handle;
    const esp_partition_t *target = upload_state_data.target;
    upload_state_data.active = false;
    upload_state_data = UploadState{};
    if (err == ESP_OK) err = esp_ota_end(handle);
    else esp_ota_abort(handle);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
    if (err == ESP_OK) {
        err = schedule_restart();
        if (err != ESP_OK && running) esp_ota_set_boot_partition(running);
    }

    lock_state();
    update_busy = false;
    update_state = err == ESP_OK ? "rebooting" : "failed";
    update_error = err == ESP_OK ? "" : esp_err_to_name(err);
    unlock_state();
    if (err == ESP_OK) ESP_LOGI(TAG, "Local firmware image installed in %s; reboot scheduled", target->label);
    return err;
}

void firmware_upload_abort() {
    if (upload_state_data.active) clear_upload_state();
    lock_state();
    update_busy = false;
    update_state = "failed";
    update_error = "upload interrupted";
    unlock_state();
}

esp_err_t firmware_reboot_alternate() {
    const esp_partition_t *other = alternate_partition();
    if (!other) return ESP_ERR_NOT_FOUND;

    esp_app_desc_t desc{};
    esp_err_t err = esp_ota_get_partition_description(other, &desc);
    if (err != ESP_OK) return err;

    err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) return err;

    ESP_LOGW(TAG, "Alternate boot requested: %s (%s)", other->label, desc.version);
    return schedule_restart();
}

esp_err_t firmware_reboot_current() {
    return schedule_restart();
}

std::string firmware_info_json() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = alternate_partition();
    esp_app_desc_t other_desc{};
    bool other_valid = other && esp_ota_get_partition_description(other, &other_desc) == ESP_OK;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);

    lock_state();
    std::string current_state = update_state;
    std::string current_error = update_error;
    std::string current_target = update_target_version;
    bool busy = update_busy;
    const auto progress = download_progress;
    unlock_state();

    const esp_app_desc_t *app = esp_app_get_description();
    std::string json = "{";
    json += "\"version\":" + json_escape(app ? app->version : "unknown");
    json += ",\"image_channel\":" + json_escape(image_channel());
    json += ",\"update_channel\":" + json_escape(load_channel());
    json += ",\"commit\":" + json_escape(ESPYSPA_GIT_COMMIT);
    json += ",\"partition\":" + json_escape(running ? running->label : "unknown");
    json += ",\"ota_state\":" + std::to_string(static_cast<int>(state));
    json += ",\"busy\":" + std::string(busy ? "true" : "false");
    json += ",\"download_progress\":" + progress.json();
    json += ",\"update_state\":" + json_escape(current_state);
    json += ",\"update_error\":" + json_escape(current_error);
    json += ",\"update_target_version\":" + json_escape(current_target);
    json += ",\"dns\":" + thread_dns_json();
    if (other_valid) {
        json += ",\"alternate\":{\"partition\":" + json_escape(other->label);
        json += ",\"version\":" + json_escape(other_desc.version) + "}";
    } else {
        json += ",\"alternate\":null";
    }
    json += "}";
    return json;
}
