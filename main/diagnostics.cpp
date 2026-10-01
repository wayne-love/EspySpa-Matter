#include "app.hpp"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/thread.h"
#include "openthread/ip6.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <cstring>
namespace {
const char page[] = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>EspySpa diagnostics</title>
<style>body{font:16px system-ui;max-width:1000px;margin:2rem auto;padding:0 1rem;background:#101c26;color:#e6f1f5}input,button{font:inherit;padding:.6rem}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#1b2b38;padding:1rem}label{display:block;margin:1rem 0}</style>
<h1>EspySpa diagnostics</h1><p>Read-only interface. State is reported by the spa controller. Requests are confirmed by readback.</p>
<label>Diagnostic token <input id="token" type="password" autocomplete="off"></label><button id="connect">Connect</button><p id="status">Disconnected</p><button id="download">Download snapshot</button><pre id="data"></pre>
<script>let timer,latest;const el=id=>document.getElementById(id);async function refresh(){try{const r=await fetch('/api/diagnostics',{headers:{Authorization:'Bearer '+el('token').value},cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);latest=await r.json();el('data').textContent=JSON.stringify(latest,null,2);el('status').textContent=latest.fresh?'Live spa state':'STALE / unavailable — see errors';}catch(e){el('status').textContent='Connection failed: '+e.message;}}el('connect').onclick=()=>{clearInterval(timer);refresh();timer=setInterval(refresh,5000);};el('download').onclick=()=>{if(!latest)return;const a=document.createElement('a');const u=URL.createObjectURL(new Blob([JSON.stringify(latest,null,2)],{type:'application/json'}));a.href=u;a.download='spa-diagnostics.json';a.click();URL.revokeObjectURL(u);};</script></html>)HTML";
bool authenticated(httpd_req_t *req) {
    const char *token = CONFIG_SPA_DIAGNOSTIC_TOKEN;
    if (strlen(token) < 24 || strlen(token) > 128) return false;
    char header[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) return false;
    std::string expected = std::string("Bearer ") + token;
    if (strlen(header) != expected.size()) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < expected.size(); ++i) difference |= static_cast<unsigned char>(header[i] ^ expected[i]);
    return difference == 0;
}
esp_err_t dashboard(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}
esp_err_t diagnostic(httpd_req_t *req) {
    if (!authenticated(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Diagnostic token required (at least 24 characters)");
    auto s = snapshot(); const auto now = now_ms();
    cJSON *root = cJSON_CreateObject(); if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddNumberToObject(root, "uptime_ms", now);
    cJSON_AddNumberToObject(root, "reset_reason", esp_reset_reason());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddBoolToObject(root, "fresh", s.valid && now - s.last_valid_ms <= 15000);
    if (s.last_valid_ms) cJSON_AddNumberToObject(root, "state_age_ms", now - s.last_valid_ms);
    else cJSON_AddNullToObject(root, "state_age_ms");
    cJSON_AddStringToObject(root, "last_error", s.error.c_str());
    auto state = cJSON_AddObjectToObject(root, "last_known_spa_state");
    cJSON_AddNumberToObject(state, "water_c", s.state.water_tenths / 10.0);
    cJSON_AddNumberToObject(state, "setpoint_c", s.state.setpoint_tenths / 10.0);
    cJSON_AddBoolToObject(state, "light", s.state.light);
    cJSON_AddBoolToObject(state, "heating", s.state.heating);
    cJSON_AddBoolToObject(state, "sleeping", s.state.sleeping);
    cJSON_AddNumberToObject(state, "blower_mode", s.state.blower);
    auto pumps = cJSON_AddArrayToObject(state, "pumps");
    for (int p = 0; p < 5; ++p) {
        auto item = cJSON_CreateObject(); cJSON_AddItemToArray(pumps, item);
        cJSON_AddNumberToObject(item, "number", p + 1);
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
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.server_port = 8080;
    cfg.stack_size = 8192; cfg.max_open_sockets = 2; cfg.lru_purge_enable = true;
    httpd_handle_t server; ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    httpd_uri_t ui{}; ui.uri = "/"; ui.method = HTTP_GET; ui.handler = dashboard;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ui));
    httpd_uri_t api{}; api.uri = "/api/diagnostics"; api.method = HTTP_GET; api.handler = diagnostic;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api));
}
