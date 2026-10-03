#include "app.hpp"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/thread.h"
#include "openthread/ip6.h"
#include "cJSON.h"
namespace {
const char page[] = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>EspySpa diagnostics</title>
<style>
body{font:16px system-ui;max-width:1000px;margin:2rem auto;padding:0 1rem;background:#101c26;color:#e6f1f5}
button,select{font:inherit;padding:.6rem;margin:.25rem .25rem .25rem 0}
button.danger{background:#8e2d2d;color:white;border:0}
section{background:#162633;padding:1rem;margin:1rem 0;border-radius:.5rem}
pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#1b2b38;padding:1rem}
.grid{display:grid;grid-template-columns:max-content 1fr;gap:.35rem 1rem}
.warn{color:#ffd27a;font-weight:700}
</style>
<h1>EspySpa diagnostics</h1>
<p>State is reported by the spa controller. Control requests are confirmed by readback.</p>
<section>
<h2>Firmware</h2>
<p id="fw-banner"></p>
<div class="grid">
<span>Running</span><strong id="fw-running">Loading…</strong>
<span>Partition</span><span id="fw-partition">—</span>
<span>Update channel</span><span><select id="fw-channel"><option value="release">Release</option><option value="development">Development</option></select></span>
<span>Alternate image</span><span id="fw-alternate">—</span>
<span>Update status</span><span id="fw-state">—</span>
</div>
<p>
<button id="fw-update">Check and install update</button>
<button id="fw-alt">Reboot into alternate image</button>
<button id="fw-reboot">Reboot</button>
</p>
</section>
<p id="status">Connecting…</p><button id="download">Download snapshot</button><pre id="data"></pre>
<script>
let timer,latest;
const el=id=>document.getElementById(id);
async function action(path,body){
  const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-EspySpa-Action':'1'},body:JSON.stringify(body||{})});
  if(!r.ok)throw Error((await r.text())||('HTTP '+r.status));
  return r;
}
async function refreshFirmware(){
  try{
    const r=await fetch('/api/firmware',{cache:'no-store'}); if(!r.ok)throw Error('HTTP '+r.status);
    const f=await r.json();
    el('fw-running').textContent=f.version+' ('+f.image_channel+', '+f.commit+')';
    el('fw-partition').textContent=f.partition;
    el('fw-channel').value=f.update_channel;
    el('fw-alternate').textContent=f.alternate?f.alternate.version+' ('+f.alternate.partition+')':'No valid alternate image';
    el('fw-state').textContent=f.update_state+(f.update_target_version?' → '+f.update_target_version:'')+(f.update_error?' — '+f.update_error:'');
    el('fw-banner').textContent=f.image_channel==='development'?'⚠ Development firmware':'Release firmware';
    el('fw-banner').className=f.image_channel==='development'?'warn':'';
    el('fw-update').disabled=f.busy;
    el('fw-alt').disabled=f.busy||!f.alternate;
  }catch(e){el('fw-state').textContent='Firmware status failed: '+e.message;}
}
async function refresh(){
  try{
    const r=await fetch('/api/diagnostics',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);
    latest=await r.json();el('data').textContent=JSON.stringify(latest,null,2);
    el('status').textContent=latest.fresh?'Live spa state':'STALE / unavailable — see errors';
  }catch(e){el('status').textContent='Connection failed: '+e.message;}
  await refreshFirmware();
}
el('fw-channel').onchange=async()=>{try{await action('/api/firmware/channel',{channel:el('fw-channel').value});await refreshFirmware();}catch(e){alert(e.message);}};
el('fw-update').onclick=async()=>{if(!confirm('Install the newest image from the selected channel and reboot?'))return;try{await action('/api/firmware/update');await refreshFirmware();}catch(e){alert(e.message);}};
el('fw-alt').onclick=async()=>{if(!confirm('Reboot into the alternate firmware image?'))return;try{await action('/api/firmware/alternate');}catch(e){alert(e.message);}};
el('fw-reboot').onclick=async()=>{if(!confirm('Reboot EspySpa?'))return;try{await action('/api/reboot');}catch(e){alert(e.message);}};
el('download').onclick=()=>{if(!latest)return;const a=document.createElement('a');const u=URL.createObjectURL(new Blob([JSON.stringify(latest,null,2)],{type:'application/json'}));a.href=u;a.download='spa-diagnostics.json';a.click();URL.revokeObjectURL(u);};
refresh();timer=setInterval(refresh,5000);
</script></html>)HTML";
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

bool authorized_action(httpd_req_t *req) {
    char value[8]{};
    if (httpd_req_get_hdr_value_str(req, "X-EspySpa-Action", value, sizeof(value)) != ESP_OK) return false;
    return strcmp(value, "1") == 0;
}

esp_err_t firmware_info(httpd_req_t *req) {
    const auto body = firmware_info_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t firmware_channel(httpd_req_t *req) {
    if (!authorized_action(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing action header");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 128) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }
    char body[129]{};
    int got = httpd_req_recv(req, body, req->content_len);
    if (got <= 0) return ESP_FAIL;

    cJSON *root = cJSON_ParseWithLength(body, got);
    cJSON *item = root ? cJSON_GetObjectItemCaseSensitive(root, "channel") : nullptr;
    if (!cJSON_IsString(item) || !item->valuestring) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "channel must be release or development");
        return ESP_FAIL;
    }
    esp_err_t err = firmware_set_update_channel(item->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return err;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t firmware_update(httpd_req_t *req) {
    if (!authorized_action(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing action header");
        return ESP_FAIL;
    }
    esp_err_t err = firmware_start_update();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_409_CONFLICT, esp_err_to_name(err));
        return err;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

esp_err_t firmware_alternate(httpd_req_t *req) {
    if (!authorized_action(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing action header");
        return ESP_FAIL;
    }
    esp_err_t err = firmware_reboot_alternate();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_409_CONFLICT, esp_err_to_name(err));
        return err;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"rebooting\":true}");
}

esp_err_t reboot(httpd_req_t *req) {
    if (!authorized_action(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing action header");
        return ESP_FAIL;
    }
    esp_err_t err = firmware_reboot_current();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"rebooting\":true}");
}
}
void diagnostics_start() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.server_port = 8080;
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192; cfg.max_open_sockets = 2; cfg.lru_purge_enable = true;
    httpd_handle_t server; ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    httpd_uri_t ui{}; ui.uri = "/"; ui.method = HTTP_GET; ui.handler = dashboard;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ui));
    httpd_uri_t api{}; api.uri = "/api/diagnostics"; api.method = HTTP_GET; api.handler = diagnostic;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api));

    httpd_uri_t fw{}; fw.uri = "/api/firmware"; fw.method = HTTP_GET; fw.handler = firmware_info;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &fw));

    httpd_uri_t channel{}; channel.uri = "/api/firmware/channel"; channel.method = HTTP_POST; channel.handler = firmware_channel;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &channel));

    httpd_uri_t update{}; update.uri = "/api/firmware/update"; update.method = HTTP_POST; update.handler = firmware_update;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update));

    httpd_uri_t alternate{}; alternate.uri = "/api/firmware/alternate"; alternate.method = HTTP_POST; alternate.handler = firmware_alternate;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &alternate));

    httpd_uri_t reboot_uri{}; reboot_uri.uri = "/api/reboot"; reboot_uri.method = HTTP_POST; reboot_uri.handler = reboot;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reboot_uri));
}
