#include "app.hpp"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include <cstring>

namespace {
const char *TAG = "firmware_web";

const char page[] = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>EspySpa firmware</title>
<style>
body{font:16px system-ui;max-width:850px;margin:2rem auto;padding:0 1rem;background:#101c26;color:#e6f1f5}
button,select{font:inherit;padding:.6rem;margin:.25rem .25rem .25rem 0}
section{background:#162633;padding:1rem;margin:1rem 0;border-radius:.5rem}
.grid{display:grid;grid-template-columns:max-content 1fr;gap:.4rem 1rem}
.warn{color:#ffd27a;font-weight:700}
.locked{color:#ffb0a8;font-weight:700}
</style>
<h1>EspySpa firmware</h1>
<p>Firmware changes require a recent physical BOOT-button press. Press and release BOOT once, then use the controls below within 60 seconds.</p>
<section>
<p id="banner"></p>
<div class="grid">
<span>Running</span><strong id="running">Loading…</strong>
<span>Partition</span><span id="partition">—</span>
<span>Update channel</span><span><select id="channel"><option value="release">Release</option><option value="development">Development</option></select></span>
<span>Alternate image</span><span id="alternate">—</span>
<span>Management</span><span id="unlock">—</span>
<span>Update status</span><span id="state">—</span>
</div>
<p>
<button id="update">Check and install update</button>
<button id="alt">Reboot into alternate image</button>
<button id="reboot">Reboot current image</button>
</p>
</section>
<script>
const el=id=>document.getElementById(id);
async function action(path,body){
  const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body||{})});
  if(!r.ok)throw Error((await r.text())||('HTTP '+r.status));
  return r;
}
async function refresh(){
  try{
    const r=await fetch('/api/firmware',{cache:'no-store'}); if(!r.ok)throw Error('HTTP '+r.status);
    const f=await r.json();
    el('running').textContent=f.version+' ('+f.image_channel+', '+f.commit+')';
    el('partition').textContent=f.partition;
    el('channel').value=f.update_channel;
    el('alternate').textContent=f.alternate?f.alternate.version+' ('+f.alternate.partition+')':'No valid alternate image';
    el('state').textContent=f.update_state+(f.update_target_version?' → '+f.update_target_version:'')+(f.update_error?' — '+f.update_error:'');
    el('banner').textContent=f.image_channel==='development'?'⚠ Development firmware':'Release firmware';
    el('banner').className=f.image_channel==='development'?'warn':'';
    el('unlock').textContent=f.management_unlocked?'Unlocked':'Locked — press BOOT once';
    el('unlock').className=f.management_unlocked?'':'locked';
    const disabled=!f.management_unlocked||f.busy;
    el('update').disabled=disabled;
    el('channel').disabled=!f.management_unlocked||f.busy;
    el('alt').disabled=disabled||!f.alternate;
    el('reboot').disabled=!f.management_unlocked||f.busy;
  }catch(e){el('state').textContent='Firmware status failed: '+e.message;}
}
el('channel').onchange=async()=>{try{await action('/api/firmware/channel',{channel:el('channel').value});await refresh();}catch(e){alert(e.message);await refresh();}};
el('update').onclick=async()=>{if(!confirm('Install the newest image from the selected channel and reboot?'))return;try{await action('/api/firmware/update');await refresh();}catch(e){alert(e.message);await refresh();}};
el('alt').onclick=async()=>{if(!confirm('Reboot into the alternate firmware image?'))return;try{await action('/api/firmware/alternate');}catch(e){alert(e.message);await refresh();}};
el('reboot').onclick=async()=>{if(!confirm('Reboot EspySpa?'))return;try{await action('/api/reboot');}catch(e){alert(e.message);await refresh();}};
refresh();setInterval(refresh,3000);
</script></html>)HTML";

bool require_unlock(httpd_req_t *req) {
    if (firmware_management_is_unlocked()) return true;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Press and release the physical BOOT button, then try again within 60 seconds.");
    return false;
}

esp_err_t dashboard(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t info(httpd_req_t *req) {
    const auto body = firmware_info_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t channel(httpd_req_t *req) {
    if (!require_unlock(req)) return ESP_FAIL;
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
    if (!require_unlock(req)) return ESP_FAIL;
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

esp_err_t alternate(httpd_req_t *req) {
    if (!require_unlock(req)) return ESP_FAIL;
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
    if (!require_unlock(req)) return ESP_FAIL;
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

void firmware_web_start() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8081;
    cfg.ctrl_port = 32769;
    cfg.max_uri_handlers = 6;
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t ui{}; ui.uri = "/"; ui.method = HTTP_GET; ui.handler = dashboard;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ui));
    httpd_uri_t fw{}; fw.uri = "/api/firmware"; fw.method = HTTP_GET; fw.handler = info;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &fw));
    httpd_uri_t ch{}; ch.uri = "/api/firmware/channel"; ch.method = HTTP_POST; ch.handler = channel;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ch));
    httpd_uri_t up{}; up.uri = "/api/firmware/update"; up.method = HTTP_POST; up.handler = update;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &up));
    httpd_uri_t alt{}; alt.uri = "/api/firmware/alternate"; alt.method = HTTP_POST; alt.handler = alternate;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &alt));
    httpd_uri_t rb{}; rb.uri = "/api/reboot"; rb.method = HTTP_POST; rb.handler = reboot;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rb));

    ESP_LOGI(TAG, "Firmware management UI listening on port 8081");
}
