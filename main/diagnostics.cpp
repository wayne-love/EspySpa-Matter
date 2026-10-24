#include "app.hpp"
#include "product_url.hpp"
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
            ESP_LOGI("spa_network", "Diagnostics and firmware: http://[%s]/ (requires border-router IPv6 route)",
                     addresses[i].text);
        }
    }
    if (!has_url) ESP_LOGI("spa_network", "No preferred routable IPv6 address available for web access");
}
void network_log_worker(void *) {
    TickType_t last = xTaskGetTickCount();
    unsigned reports = 0;
    for (;;) {
        product_url_refresh();
        if (reports++ % 12 == 0) log_thread_addresses();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(5000));
    }
}
const char page[] = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>EspySpa</title>
<style>body{font:16px system-ui;max-width:1000px;margin:2rem auto;padding:0 1rem;background:#101c26;color:#e6f1f5}input,button,select{font:inherit;padding:.6rem;margin:.25rem .25rem .25rem 0}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#1b2b38;padding:1rem}label{display:block;margin:1rem 0}section{background:#162633;padding:1rem;margin:1rem 0;border-radius:.5rem}.warn{color:#ffd27a;font-weight:700}</style>
<h1>EspySpa</h1><nav><button id="diag-tab">Diagnostics</button><button id="config-tab">Configuration</button><button id="firmware-tab">Firmware</button></nav>
<div id="diag-panel"><p>Read-only interface. Successful command acknowledgements update cached control state immediately; the next RF poll confirms it. State source identifies the latest evidence.</p><p id="status">Connecting…</p><button id="download">Download snapshot</button><pre id="data"></pre></div>
<div id="config-panel" hidden><h2>Configuration</h2><label for="poll">Spa state polling interval (15–300 seconds)</label><input id="poll" type="number" min="15" max="300" step="1"><button id="save-poll">Save</button><p id="config-status"></p></div>
<div id="firmware-panel" hidden><h2>Firmware update</h2><p id="fw-banner"></p><section><p>Firmware update and reboot controls are available from this page. Use only trusted ESP32-C6 release images.</p><div>Running: <strong id="running">Loading…</strong></div><div>Partition: <span id="partition">—</span></div><label>Update channel <select id="channel"><option value="release">Release</option><option value="development">Development</option></select></label><div>Alternate image: <span id="alternate">—</span></div><div>Update status: <span id="fw-state">—</span></div><div id="fw-download" hidden><label for="fw-progress">Firmware download</label><progress id="fw-progress" max="100" style="width:100%"></progress><p id="fw-progress-text" role="status" aria-live="polite"></p></div><p><button id="update">Check and install update</button><button id="alt">Reboot into alternate image</button><button id="reboot">Reboot current image</button></p></section><section><h3>Install a firmware file</h3><p>Select the app-only <code>espyspa-matter.bin</code> image from a successful ESP32-C6 release artifact. Do not upload a merged full-flash image. It is written to the inactive OTA slot and checked by ESP-IDF before reboot.</p><input id="firmware-file" type="file" accept=".bin,application/octet-stream"><button id="upload">Upload and install</button><p id="upload-status"></p></section></div>
<script>
let timer,latest;const el=id=>document.getElementById(id);let panels=['diag','config','firmware'];function show(name){for(const p of panels)el(p+'-panel').hidden=p!==name;}
async function refresh(){try{const r=await fetch('/api/diagnostics',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);latest=await r.json();el('data').textContent=JSON.stringify(latest,null,2);el('status').textContent=latest.fresh?'Live spa state':'STALE / unavailable — see errors';}catch(e){el('status').textContent='Connection failed: '+e.message;}}
async function loadConfig(){try{const r=await fetch('/api/config',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const c=await r.json();el('poll').value=c.poll_seconds;el('config-status').textContent='Saved interval: '+c.poll_seconds+' seconds';}catch(e){el('config-status').textContent=e.message;}}
async function action(path,body){const headers={},options={method:'POST',headers};if(body!==undefined){headers['Content-Type']='application/json';options.body=JSON.stringify(body);}const r=await fetch(path,options);if(!r.ok)throw Error((await r.text())||('HTTP '+r.status));return r;}
function renderDownloadProgress(f){
 const p=f.download_progress,box=el('fw-download'),bar=el('fw-progress'),text=el('fw-progress-text');
 box.hidden=!p||!p.started;
 if(box.hidden)return;
 const received=Number(p.received_bytes)||0,total=Number(p.total_bytes)||0;
 if(total>0){const percent=Math.min(100,Math.max(0,Math.floor(received*100/total)));bar.value=percent;text.textContent=percent+'% — '+received.toLocaleString()+' / '+total.toLocaleString()+' bytes';}
 else{bar.removeAttribute('value');text.textContent=received.toLocaleString()+' bytes downloaded — total size unknown';}
 if(f.update_state==='verifying')text.textContent+=' — Verifying image…';
 if(f.update_state==='rebooting')text.textContent+=' — Verified; rebooting…';
 if(f.update_state==='failed')text.textContent+=' — Download/update failed; see update status';
}
async function refreshFirmware(){try{const r=await fetch('/api/firmware',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const f=await r.json();renderDownloadProgress(f);el('running').textContent=f.version+' ('+f.image_channel+', '+f.commit+')';el('partition').textContent=f.partition;el('channel').value=f.update_channel;el('alternate').textContent=f.alternate?f.alternate.version+' ('+f.alternate.partition+')':'No valid alternate image';el('fw-state').textContent=f.update_state+(f.update_target_version?' → '+f.update_target_version:'')+(f.update_error?' — '+f.update_error:'');el('fw-banner').textContent=f.image_channel==='development'?'Development firmware':'Release firmware';el('fw-banner').className=f.image_channel==='development'?'warn':'';for(const id of ['update','upload','channel','alt','reboot'])el(id).disabled=f.busy||(id==='alt'&&!f.alternate);}catch(e){el('fw-state').textContent='Firmware status failed: '+e.message;el('fw-progress-text').textContent='Connection unavailable; last reported progress may be stale';}}
el('diag-tab').onclick=()=>show('diag');el('config-tab').onclick=()=>{show('config');loadConfig();};el('firmware-tab').onclick=()=>{show('firmware');refreshFirmware();};
el('save-poll').onclick=async()=>{const n=Number(el('poll').value);if(!Number.isInteger(n)||n<15||n>300){el('config-status').textContent='Enter 15 to 300 whole seconds';return;}try{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({poll_seconds:n})});if(!r.ok)throw Error(await r.text());await loadConfig();}catch(e){el('config-status').textContent=e.message;}};
el('download').onclick=()=>{if(!latest)return;const a=document.createElement('a');const u=URL.createObjectURL(new Blob([JSON.stringify(latest,null,2)],{type:'application/json'}));a.href=u;a.download='spa-diagnostics.json';a.click();URL.revokeObjectURL(u);};
el('channel').onchange=async()=>{try{await action('/api/firmware/channel',{channel:el('channel').value});await refreshFirmware();}catch(e){alert(e.message);await refreshFirmware();}};
el('update').onclick=async()=>{if(!confirm('Install the newest image from the selected channel and reboot?'))return;try{await action('/api/firmware/update');await refreshFirmware();}catch(e){alert(e.message);await refreshFirmware();}};
el('alt').onclick=async()=>{if(!confirm('Reboot into the alternate firmware image?'))return;try{await action('/api/firmware/alternate');}catch(e){alert(e.message);await refreshFirmware();}};
el('reboot').onclick=async()=>{if(!confirm('Reboot EspySpa?'))return;try{await action('/api/reboot');}catch(e){alert(e.message);await refreshFirmware();}};
el('upload').onclick=async()=>{const file=el('firmware-file').files[0];if(!file){el('upload-status').textContent='Choose espyspa-matter.bin first';return;}if(!file.name.toLowerCase().endsWith('.bin')){el('upload-status').textContent='Choose the app-only .bin image';return;}if(!confirm('Install '+file.name+' ('+file.size+' bytes) and reboot? Use only the app-only espyspa-matter.bin from a trusted ESP32-C6 release artifact.'))return;el('upload-status').textContent='Checking file…';try{const bytes=await file.arrayBuffer();const hash=await sha256(bytes);el('upload-status').textContent='Uploading and verifying SHA-256…';const r=await fetch('/api/firmware/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream','X-Firmware-SHA256':hash},body:bytes});if(!r.ok)throw Error((await r.text())||('HTTP '+r.status));el('upload-status').textContent='Image accepted. Device is rebooting.';}catch(e){el('upload-status').textContent='Upload failed: '+e.message;await refreshFirmware();}};
async function sha256(buffer){const k=[0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2],ro=(x,n)=>(x>>>n)|(x<<(32-n)),m=new Uint8Array(buffer),bit=m.length*8,pad=((m.length+9+63)>>6)<<6,d=new Uint8Array(pad);d.set(m);d[m.length]=128;const dv=new DataView(d.buffer);dv.setUint32(pad-4,bit>>>0);dv.setUint32(pad-8,Math.floor(bit/4294967296));let h=[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19],w=new Uint32Array(64);for(let o=0;o<pad;o+=64){for(let i=0;i<16;i++)w[i]=dv.getUint32(o+i*4);for(let i=16;i<64;i++){const a=w[i-15],b=w[i-2],s0=ro(a,7)^ro(a,18)^(a>>>3),s1=ro(b,17)^ro(b,19)^(b>>>10);w[i]=(w[i-16]+s0+w[i-7]+s1)>>>0;}let [a,b,c,e,f,g,q,z]=h;for(let i=0;i<64;i++){const s1=ro(f,6)^ro(f,11)^ro(f,25),ch=(f&g)^(~f&q),t1=(z+s1+ch+k[i]+w[i])>>>0,s0=ro(a,2)^ro(a,13)^ro(a,22),maj=(a&b)^(a&c)^(b&c),t2=(s0+maj)>>>0;z=q;q=g;g=f;f=(e+t1)>>>0;e=c;c=b;b=a;a=(t1+t2)>>>0;}h=[(h[0]+a)>>>0,(h[1]+b)>>>0,(h[2]+c)>>>0,(h[3]+e)>>>0,(h[4]+f)>>>0,(h[5]+g)>>>0,(h[6]+q)>>>0,(h[7]+z)>>>0];}return h.map(x=>x.toString(16).padStart(8,'0')).join('');}
refresh();timer=setInterval(refresh,5000);setInterval(refreshFirmware,1000);</script></html>)HTML";
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
    cJSON_AddStringToObject(root, "product_url", product_url_snapshot().c_str());
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddNumberToObject(root, "uptime_ms", now);
    cJSON_AddNumberToObject(root, "reset_reason", esp_reset_reason());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "poll_seconds", spa_poll_interval_seconds());
    cJSON_AddBoolToObject(root, "fresh", s.valid && now - s.last_valid_ms <= uint64_t(spa_poll_interval_seconds()) * 1000 + 10000);
    if (s.last_valid_ms) cJSON_AddNumberToObject(root, "state_age_ms", now - s.last_valid_ms);
    else cJSON_AddNullToObject(root, "state_age_ms");
    cJSON_AddStringToObject(root, "state_source", s.command_acknowledged ? "command_acknowledgement" : (s.valid ? "rf" : "last_known_rf"));
    cJSON_AddStringToObject(root, "last_error", s.error.c_str());
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
        cJSON_AddStringToObject(item, "tx_hex", t.tx_hex.c_str());
        cJSON_AddStringToObject(item, "rx_hex", t.rx_hex.c_str());
        cJSON_AddBoolToObject(item, "tx_hex_truncated", t.tx_truncated);
        cJSON_AddBoolToObject(item, "rx_hex_truncated", t.rx_truncated);
    }
    char *body = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    auto result = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN); cJSON_free(body); return result;
}
}
void diagnostics_start() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.server_port = 80;
    cfg.max_uri_handlers = 12;
    cfg.recv_wait_timeout = 60;
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
    firmware_web_register(server);
    ESP_ERROR_CHECK(xTaskCreate(network_log_worker, "network_log", 4096, nullptr, 1, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
