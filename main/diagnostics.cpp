#include "app.hpp"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/thread.h"
#include "openthread/ip6.h"
#include "cJSON.h"
#include "freertos/task.h"
#include <array>
#include <cstring>
#include <cstdlib>
namespace {
void log_thread_addresses() {
    struct Address { char text[OT_IP6_ADDRESS_STRING_SIZE]{}; const char *scope = ""; bool usable = false; };
    std::array<Address, 16> addresses{};
    size_t count = 0;
    bool truncated = false, available = false;
    otDeviceRole role = OT_DEVICE_ROLE_DISABLED;
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        ESP_LOGW("spa_network", "IPv6 minute report: Thread lock busy; retry next minute");
        return;
    }
    auto ot = esp_openthread_get_instance();
    if (ot) {
        available = true; role = otThreadGetDeviceRole(ot);
        const auto prefix = otThreadGetMeshLocalPrefix(ot);
        const auto link_local_address = otThreadGetLinkLocalIp6Address(ot);
        for (auto a = otIp6GetUnicastAddresses(ot); a; a = a->mNext) {
            if (count == addresses.size()) { truncated = true; break; }
            auto &entry = addresses[count++];
            otIp6AddressToString(&a->mAddress, entry.text, sizeof(entry.text));
            const bool local = link_local_address && otIp6IsAddressEqual(&a->mAddress, link_local_address);
            const bool mesh = prefix && std::memcmp(a->mAddress.mFields.m8, prefix->m8, sizeof(prefix->m8)) == 0;
            entry.scope = local ? "link-local" : (mesh || a->mRloc ? "Thread mesh-local" : "routable candidate");
            entry.usable = role >= OT_DEVICE_ROLE_CHILD && !local && !mesh && !a->mRloc && a->mValid && a->mPreferred;
        }
    }
    esp_openthread_lock_release(); // Never hold the Thread lock during serial output.
    const char *role_name = "unknown";
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: role_name = "disabled"; break;
    case OT_DEVICE_ROLE_DETACHED: role_name = "detached"; break;
    case OT_DEVICE_ROLE_CHILD: role_name = "child"; break;
    case OT_DEVICE_ROLE_ROUTER: role_name = "router"; break;
    case OT_DEVICE_ROLE_LEADER: role_name = "leader"; break;
    }
    ESP_LOGI("spa_network", "IPv6 minute report: Thread=%s, addresses=%u%s", available ? role_name : "not initialized",
             unsigned(count), truncated ? " (list truncated)" : "");
    bool has_url = false;
    for (size_t i = 0; i < count; ++i) {
        ESP_LOGI("spa_network", "IPv6: %s [%s]", addresses[i].text, addresses[i].scope);
        if (addresses[i].usable) {
            has_url = true;
            ESP_LOGI("spa_network", "Diagnostics: http://[%s]/ ; Firmware: http://[%s]:8081/ (requires border-router IPv6 route)",
                     addresses[i].text, addresses[i].text);
        }
    }
    if (!has_url) ESP_LOGI("spa_network", "No preferred routable IPv6 address available for web access");
}
void network_log_worker(void *) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        log_thread_addresses();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(60000));
    }
}
const char page[] = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>EspySpa diagnostics</title>
<style>body{font:16px system-ui;max-width:1000px;margin:2rem auto;padding:0 1rem;background:#101c26;color:#e6f1f5}input,button{font:inherit;padding:.6rem}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#1b2b38;padding:1rem}label{display:block;margin:1rem 0}</style>
<h1>EspySpa</h1><nav><button id="diag-tab">Diagnostics</button><button id="config-tab">Configuration</button></nav><div id="diag-panel"><p>Read-only interface. State is reported by the spa controller. Requests are confirmed by readback.</p>
<p id="status">Connecting…</p><button id="download">Download snapshot</button><pre id="data"></pre></div><div id="config-panel" hidden><h2>Configuration</h2><label for="poll">Spa state polling interval (15–300 seconds)</label><input id="poll" type="number" min="15" max="300" step="1"><button id="save-poll">Save</button><p id="config-status"></p></div>
<script>let timer,latest;const el=id=>document.getElementById(id);async function refresh(){try{const r=await fetch('/api/diagnostics',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);latest=await r.json();el('data').textContent=JSON.stringify(latest,null,2);el('status').textContent=latest.fresh?'Live spa state':'STALE / unavailable — see errors';}catch(e){el('status').textContent='Connection failed: '+e.message;}}refresh();timer=setInterval(refresh,5000);el("diag-tab").onclick=()=>{el("diag-panel").hidden=false;el("config-panel").hidden=true;};el("config-tab").onclick=()=>{el("diag-panel").hidden=true;el("config-panel").hidden=false;loadConfig();};async function loadConfig(){try{const r=await fetch("/api/config",{cache:"no-store"});if(!r.ok)throw Error("HTTP "+r.status);const c=await r.json();el("poll").value=c.poll_seconds;el("config-status").textContent="Saved interval: "+c.poll_seconds+" seconds";}catch(e){el("config-status").textContent=e.message;}}el("save-poll").onclick=async()=>{const n=Number(el("poll").value);if(!Number.isInteger(n)||n<15||n>300){el("config-status").textContent="Enter 15 to 300 whole seconds";return;}try{const r=await fetch("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({poll_seconds:n})});if(!r.ok)throw Error(await r.text());await loadConfig();}catch(e){el("config-status").textContent=e.message;}};el('download').onclick=()=>{if(!latest)return;const a=document.createElement('a');const u=URL.createObjectURL(new Blob([JSON.stringify(latest,null,2)],{type:'application/json'}));a.href=u;a.download='spa-diagnostics.json';a.click();URL.revokeObjectURL(u);};</script></html>)HTML";
esp_err_t config_get(httpd_req_t *req) {
    char body[64];
    snprintf(body, sizeof(body), "{\"poll_seconds\":%lu}", static_cast<unsigned long>(spa_poll_interval_seconds()));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}
esp_err_t config_post(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 128) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload");
        return ESP_FAIL;
    }
    char data[129]{};
    const int length = httpd_req_recv(req, data, req->content_len);
    if (length != req->content_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete payload");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(data, length);
    cJSON *item = json ? cJSON_GetObjectItemCaseSensitive(json, "poll_seconds") : nullptr;
    if (!cJSON_IsNumber(item) || item->valuedouble < 15 || item->valuedouble > 300 || item->valuedouble != item->valueint) {
        if (json) cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "poll_seconds must be an integer from 15 to 300");
        return ESP_FAIL;
    }
    const auto seconds = static_cast<uint32_t>(item->valueint);
    cJSON_Delete(json);
    const esp_err_t err = spa_set_poll_interval_seconds(seconds);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return config_get(req);
}
esp_err_t dashboard(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}
esp_err_t diagnostic(httpd_req_t *req) {
    auto s = snapshot(); const auto now = now_ms();
    cJSON *root = cJSON_CreateObject(); if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddNumberToObject(root, "uptime_ms", now);
    cJSON_AddNumberToObject(root, "reset_reason", esp_reset_reason());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "poll_seconds", spa_poll_interval_seconds());
    cJSON_AddBoolToObject(root, "fresh", s.valid && now - s.last_valid_ms <= uint64_t(spa_poll_interval_seconds()) * 1000 + 10000);
    if (s.last_valid_ms) cJSON_AddNumberToObject(root, "state_age_ms", now - s.last_valid_ms);
    else cJSON_AddNullToObject(root, "state_age_ms");
    cJSON_AddStringToObject(root, "last_error", s.error.c_str());
    // Publish only live, validated electrical readings; never mislabel old RF as current data.
    auto electrical = cJSON_AddObjectToObject(root, "mains_electrical");
    const bool electrical_fresh = s.valid && now - s.last_valid_ms <= 15000 && s.state.mains_electrical_valid;
    cJSON_AddBoolToObject(electrical, "fresh", electrical_fresh);
    if (electrical_fresh) {
        cJSON_AddNumberToObject(electrical, "voltage_v", s.state.mains_voltage_volts);
        cJSON_AddNumberToObject(electrical, "current_a", s.state.mains_current_tenths / 10.0);
    } else {
        cJSON_AddNullToObject(electrical, "voltage_v");
        cJSON_AddNullToObject(electrical, "current_a");
    }
    auto state = cJSON_AddObjectToObject(root, "last_known_spa_state");
    cJSON_AddNumberToObject(state, "water_c", s.state.water_tenths / 10.0);
    cJSON_AddNumberToObject(state, "setpoint_c", s.state.setpoint_tenths / 10.0);
    cJSON_AddBoolToObject(state, "light", s.state.light);
    cJSON_AddBoolToObject(state, "heating", s.state.heating);
    cJSON_AddBoolToObject(state, "sleeping", s.state.sleeping);
    cJSON_AddNumberToObject(state, "blower_mode", s.state.blower);
    cJSON_AddNumberToObject(state, "variable_level", s.state.variable_level);
    auto pumps = cJSON_AddArrayToObject(state, "pumps");
    for (int p = 0; p < 5; ++p) {
        auto item = cJSON_CreateObject(); cJSON_AddItemToArray(pumps, item);
        cJSON_AddNumberToObject(item, "number", p + 1);
        cJSON_AddBoolToObject(item, "installed", s.state.pump_installed[p]);
        cJSON_AddBoolToObject(item, "unknown_modes", s.state.pump_unknown_modes[p]);
        cJSON_AddNumberToObject(item, "speed_type", s.state.pump_speed_type[p]);
        cJSON_AddNumberToObject(item, "mode", s.state.pumps[p]);
        cJSON_AddNumberToObject(item, "supported_modes_mask", s.state.pump_modes[p]);
        cJSON_AddBoolToObject(item, "ready", s.state.pump_ready[p]);
    }
    auto counters = cJSON_AddObjectToObject(root, "counters");
    cJSON_AddNumberToObject(counters, "queued_commands", s.queued_commands);
    cJSON_AddNumberToObject(counters, "polls_ok", s.polls_ok);
    cJSON_AddNumberToObject(counters, "polls_failed", s.polls_failed);
    cJSON_AddNumberToObject(counters, "commands_confirmed", s.commands_ok);
    cJSON_AddNumberToObject(counters, "commands_failed", s.commands_failed);
    auto led = cJSON_Parse(status_led_json().c_str());
    if (led) cJSON_AddItemToObject(root, "status_led", led);
    auto matter = cJSON_Parse(("{" + matter_status() + "}").c_str());
    if (matter) cJSON_AddItemToObject(root, "matter", matter);
    auto thread = cJSON_AddObjectToObject(root, "thread");
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        auto ot = esp_openthread_get_instance();
        if (ot) {
            cJSON_AddNumberToObject(thread, "role", otThreadGetDeviceRole(ot));
            auto addresses = cJSON_AddArrayToObject(thread, "ipv6_addresses");
            for (auto a = otIp6GetUnicastAddresses(ot); a; a = a->mNext) {
                char text[OT_IP6_ADDRESS_STRING_SIZE]; otIp6AddressToString(&a->mAddress, text, sizeof(text));
                cJSON_AddItemToArray(addresses, cJSON_CreateString(text));
            }
        }
        esp_openthread_lock_release();
    } else cJSON_AddStringToObject(thread, "error", "Thread lock busy");
    cJSON_AddStringToObject(root, "raw_rf", s.raw.c_str());
    auto history = cJSON_AddArrayToObject(root, "transactions");
    for (auto &t : s.transactions) {
        auto item = cJSON_CreateObject(); cJSON_AddItemToArray(history, item);
        cJSON_AddNumberToObject(item, "uptime_ms", t.ms);
        cJSON_AddStringToObject(item, "tx", t.tx.c_str()); cJSON_AddStringToObject(item, "rx", t.rx.c_str());
        cJSON_AddStringToObject(item, "result", t.result.c_str());
    }
    char *body = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    auto result = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN); cJSON_free(body); return result;
}
}
void diagnostics_start() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.server_port = 80;
    cfg.stack_size = 8192; cfg.max_open_sockets = 2; cfg.lru_purge_enable = true;
    httpd_handle_t server; ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    httpd_uri_t ui{}; ui.uri = "/"; ui.method = HTTP_GET; ui.handler = dashboard;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ui));
    httpd_uri_t api{}; api.uri = "/api/diagnostics"; api.method = HTTP_GET; api.handler = diagnostic;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api));
    httpd_uri_t config_read{}; config_read.uri = "/api/config"; config_read.method = HTTP_GET; config_read.handler = config_get;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &config_read));
    httpd_uri_t config_write{}; config_write.uri = "/api/config"; config_write.method = HTTP_POST; config_write.handler = config_post;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &config_write));
    ESP_ERROR_CHECK(xTaskCreate(network_log_worker, "network_log", 4096, nullptr, 1, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
