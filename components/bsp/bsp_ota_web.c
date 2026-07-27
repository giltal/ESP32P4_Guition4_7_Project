/*
 * Firmware upload page served from the device.
 */

#include "bsp_ota_web.h"
#include "bsp_ota.h"
#include "bsp_remote.h"
#include "bsp_screenshot.h"
#include "bsp_copro.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "mbedtls/base64.h"

static const char *TAG = "bsp_ota_web";

#define RECV_CHUNK_BYTES  4096
#define REBOOT_DELAY_US   1000000   /* let the response reach the browser */

static httpd_handle_t s_server;
static char          *s_expected_auth;   /* "Basic <base64>", or NULL */

/* ── Upload page ────────────────────────────────────────────────── */
/*
 * The image is sent as the raw request body rather than multipart/form-data:
 * parsing multipart on-device would mean buffering and scanning for
 * boundaries, and the body is already exactly the bytes we want to flash.
 * XMLHttpRequest is used instead of fetch() because it reports upload
 * progress, which fetch() still cannot do.
 */
static const char PAGE_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Firmware update</title><style>"
":root{color-scheme:light dark;--fg:#111;--bg:#fff;--mut:#666;--line:#d4d4d8;--ac:#2563eb}"
"@media(prefers-color-scheme:dark){:root{--fg:#e5e5e5;--bg:#18181b;--mut:#a1a1aa;--line:#3f3f46;--ac:#60a5fa}}"
"*{box-sizing:border-box}body{margin:0;padding:2rem 1rem;background:var(--bg);color:var(--fg);"
"font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;display:flex;justify-content:center}"
".card{width:100%;max-width:30rem}h1{font-size:1.25rem;margin:0 0 .25rem}"
".sub{color:var(--mut);font-size:.875rem;margin:0 0 1.5rem}"
"dl{display:grid;grid-template-columns:auto 1fr;gap:.35rem 1rem;font-size:.875rem;margin:0 0 1.5rem;"
"padding:.875rem;border:1px solid var(--line);border-radius:.5rem}"
"dt{color:var(--mut)}dd{margin:0;font-family:ui-monospace,monospace}"
"input[type=file]{width:100%;padding:.75rem;border:1px dashed var(--line);border-radius:.5rem;background:none;color:inherit}"
"button{width:100%;margin-top:.875rem;padding:.7rem;font-size:1rem;font-weight:600;border:0;border-radius:.5rem;"
"background:var(--ac);color:#fff;cursor:pointer}button:disabled{opacity:.5;cursor:default}"
"#bar{display:none;margin-top:1rem;height:.5rem;background:var(--line);border-radius:.25rem;overflow:hidden}"
"#fill{height:100%;width:0;background:var(--ac);transition:width .2s}"
"#msg{margin-top:.875rem;font-size:.875rem;white-space:pre-wrap}"
".ok{color:#16a34a}.err{color:#dc2626}"
".tabs{display:flex;gap:.25rem;margin-bottom:1.25rem;border-bottom:1px solid var(--line)}"
".tab{width:auto;flex:1;margin:0;padding:.55rem;background:none;color:var(--mut);"
"font-weight:500;border-radius:.4rem .4rem 0 0}"
".tab.on{color:var(--ac);box-shadow:inset 0 -2px 0 var(--ac)}"
".pane.hide{display:none}.hint{color:var(--mut);font-size:.85rem;margin:0 0 1rem}"
"fieldset{border:1px solid var(--line);border-radius:.5rem;padding:.75rem .875rem;margin:0 0 1rem}"
"legend{padding:0 .35rem;color:var(--mut);font-size:.8rem}"
"fieldset label{display:flex;align-items:center;gap:.5rem;padding:.3rem 0;cursor:pointer}"
"input[type=radio],input[type=checkbox]{width:1.15rem;height:1.15rem;accent-color:var(--ac);margin:0}"
".fld{display:block;margin-bottom:1rem;color:var(--mut);font-size:.85rem}"
".fld input[type=text],.fld input[type=range]{margin-top:.35rem;width:100%}"
"input[type=text]{padding:.6rem .7rem;border:1px solid var(--line);border-radius:.5rem;"
"background:var(--bg);color:var(--fg);font:inherit}"
"input[type=range]{accent-color:var(--ac)}"
"output{color:var(--fg);font-family:ui-monospace,monospace}"
".sw{display:flex;align-items:center;gap:.5rem;margin-bottom:1rem;cursor:pointer}"
"#cmsg{margin-top:.875rem;font-size:.875rem}"
"a{color:var(--ac)}"
"</style></head><body><div class=card>"
"<h1>Guition P4</h1><p class=sub>JC4880P443C &middot; ESP32-P4</p>"
"<div class=tabs><button class='tab on' data-t=control>Control</button>"
"<button class=tab data-t=fw>Firmware</button></div>"
"<div id=control class=pane>"
"<p class=hint>Changes here appear on the device screen straight away.</p>"
"<fieldset><legend>Mode</legend>"
"<label><input type=radio name=mode value=0> Idle</label>"
"<label><input type=radio name=mode value=1> Run</label>"
"<label><input type=radio name=mode value=2> Test</label>"
"</fieldset>"
"<label class=fld>Level <output id=lvlout>50</output>"
"<input type=range id=level min=0 max=100 value=50></label>"
"<label class=sw><input type=checkbox id=led> Indicator</label>"
"<label class=fld>Message"
"<input type=text id=label maxlength=32 placeholder='Shown on the device'></label>"
"<button id=apply>Send to device</button>"
"<div id=cmsg></div>"
"<p class=hint style='margin-top:1.25rem'>"
"<a href='/shot.png' target=_blank rel=noopener>Capture the screen &rarr;</a>"
" &mdash; a PNG of whatever the panel is showing right now.</p></div>"
"<div id=fw class='pane hide'>"
"<dl><dt>Version</dt><dd id=ver>&hellip;</dd>"
"<dt>Running from</dt><dd id=part>&hellip;</dd>"
"<dt>Target slot</dt><dd id=next>&hellip;</dd></dl>"
"<input type=file id=f accept='.bin'>"
"<button id=b disabled>Upload and reboot</button>"
"<div id=bar><div id=fill></div></div><div id=msg></div>"
"</div>"
"</div><script>"
"var $=function(i){return document.getElementById(i);};"
"[].forEach.call(document.querySelectorAll('.tab'),function(t){t.onclick=function(){"
"[].forEach.call(document.querySelectorAll('.tab'),function(x){x.classList.remove('on');});"
"[].forEach.call(document.querySelectorAll('.pane'),function(p){p.classList.add('hide');});"
"t.classList.add('on');$(t.dataset.t).classList.remove('hide');};});"
/* Control tab. `rev` lets us ignore polls that carry nothing new, and skip
 * overwriting fields the student is in the middle of editing. */
"var lvl=$('level'),lvlout=$('lvlout'),led=$('led'),lab=$('label'),"
"apply=$('apply'),cmsg=$('cmsg'),rev=-1,editing=false;"
"lvl.oninput=function(){lvlout.textContent=lvl.value;};"
"lab.onfocus=function(){editing=true;};lab.onblur=function(){editing=false;};"
"function paint(s){"
"var r=document.querySelector('input[name=mode][value=\"'+s.mode+'\"]');if(r)r.checked=true;"
"lvl.value=s.level;lvlout.textContent=s.level;led.checked=!!s.led;"
"if(!editing)lab.value=s.label;rev=s.rev;}"
"function poll(){fetch('/state').then(function(r){return r.json();}).then(function(s){"
"if(s.rev!==rev)paint(s);}).catch(function(){});}"
"poll();setInterval(poll,1500);"
"apply.onclick=function(){"
"var m=document.querySelector('input[name=mode]:checked');"
"apply.disabled=true;cmsg.className='';cmsg.textContent='Sending\\u2026';"
"fetch('/state',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({mode:m?+m.value:0,level:+lvl.value,led:led.checked,label:lab.value})})"
".then(function(r){return r.json();}).then(function(s){paint(s);"
"cmsg.className='ok';cmsg.textContent='Applied on the device.';apply.disabled=false;})"
".catch(function(){cmsg.className='err';cmsg.textContent='Could not reach the device.';"
"apply.disabled=false;});};"
/* Firmware tab. */
"var f=document.getElementById('f'),b=document.getElementById('b'),"
"bar=document.getElementById('bar'),fill=document.getElementById('fill'),msg=document.getElementById('msg');"
"fetch('/info').then(r=>r.json()).then(j=>{"
"document.getElementById('ver').textContent=j.version;"
"document.getElementById('part').textContent=j.running;"
"document.getElementById('next').textContent=j.next;}).catch(()=>{});"
"f.onchange=()=>{b.disabled=!f.files.length;msg.textContent='';msg.className='';};"
"b.onclick=()=>{var file=f.files[0];if(!file)return;"
"b.disabled=true;f.disabled=true;bar.style.display='block';msg.className='';"
"msg.textContent='Uploading '+(file.size/1024).toFixed(0)+' KB\\u2026';"
"var x=new XMLHttpRequest();x.open('POST','/update');"
"x.upload.onprogress=e=>{if(e.lengthComputable){var p=e.loaded/e.total*100;"
"fill.style.width=p+'%';msg.textContent='Uploading \\u2026 '+p.toFixed(0)+'%';}};"
"x.onload=()=>{if(x.status===200){fill.style.width='100%';msg.className='ok';"
"msg.textContent='Update written. The device is rebooting \\u2014 reload in a few seconds.';}"
"else{msg.className='err';msg.textContent='Failed: '+x.responseText;"
"b.disabled=false;f.disabled=false;}};"
"x.onerror=()=>{msg.className='err';msg.textContent='Connection lost during upload.';"
"b.disabled=false;f.disabled=false;};"
"x.send(file);};"
"</script></body></html>";

/* ── Authentication ─────────────────────────────────────────────── */
static esp_err_t build_expected_auth(const char *user, const char *pass)
{
    char plain[128];
    int  n = snprintf(plain, sizeof(plain), "%s:%s", user, pass ? pass : "");
    if (n <= 0 || n >= (int)sizeof(plain)) return ESP_ERR_INVALID_ARG;

    unsigned char b64[192];
    size_t        b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                              (const unsigned char *)plain, n) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_expected_auth = malloc(b64_len + 8);
    if (s_expected_auth == NULL) return ESP_ERR_NO_MEM;
    snprintf(s_expected_auth, b64_len + 8, "Basic %.*s", (int)b64_len, b64);
    return ESP_OK;
}

/** @return ESP_OK when the request may proceed. */
static esp_err_t check_auth(httpd_req_t *req)
{
    if (s_expected_auth == NULL) return ESP_OK;

    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len > 0 && len < 256) {
        char got[256];
        if (httpd_req_get_hdr_value_str(req, "Authorization", got, sizeof(got)) == ESP_OK &&
            strcmp(got, s_expected_auth) == 0) {
            return ESP_OK;
        }
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"firmware\"");
    httpd_resp_sendstr(req, "Authentication required");
    return ESP_FAIL;
}

/* ── Handlers ───────────────────────────────────────────────────── */
static esp_err_t page_get(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK) return ESP_OK;

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t info_get(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK) return ESP_OK;

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    char body[192];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"running\":\"%s\",\"next\":\"%s\",\"idf\":\"%s\"}",
             esp_app_get_description()->version,
             bsp_ota_running_partition_label(),
             next ? next->label : "none",
             esp_app_get_description()->idf_ver);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* ── Control state ──────────────────────────────────────────────── */
static void state_to_json(const bsp_remote_state_t *s, char *out, size_t out_len)
{
    /* The label is user-supplied, so escape it rather than splicing it in. */
    char safe[BSP_REMOTE_LABEL_MAX * 2 + 1];
    size_t o = 0;
    for (size_t i = 0; s->label[i] && o + 2 < sizeof(safe); i++) {
        if (s->label[i] == '"' || s->label[i] == '\\') safe[o++] = '\\';
        else if ((unsigned char)s->label[i] < 0x20) continue;
        safe[o++] = s->label[i];
    }
    safe[o] = '\0';

    snprintf(out, out_len,
             "{\"mode\":%d,\"level\":%d,\"led\":%s,\"label\":\"%s\",\"rev\":%" PRIu32 "}",
             (int)s->mode, s->level, s->led ? "true" : "false", safe, s->revision);
}

static esp_err_t state_get(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK) return ESP_OK;

    bsp_remote_state_t s;
    bsp_remote_get(&s);

    char body[192];
    state_to_json(&s, body, sizeof(body));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* Minimal readers for the flat JSON the page sends — no parser needed. */
static bool json_number(const char *body, const char *key, int *out)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(body, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;

    *out = atoi(p + 1);
    return true;
}

static bool json_bool(const char *body, const char *key, bool *out)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(body, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;

    while (*p == ':' || *p == ' ') p++;
    *out = (strncmp(p, "true", 4) == 0);
    return true;
}

static bool json_string(const char *body, const char *key, char *out, size_t out_len)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(body, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;
    p = strchr(p, '"');
    if (p == NULL) return false;
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_len) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return true;
}

static esp_err_t state_post(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK) return ESP_OK;

    char body[320];
    int  len = req->content_len < (int)sizeof(body) - 1 ? req->content_len
                                                       : (int)sizeof(body) - 1;
    int  got = httpd_req_recv(req, body, len);
    if (got <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"empty body\"}");
    }
    body[got] = '\0';

    /* Start from the current state so a partial POST only changes what it names. */
    bsp_remote_state_t s;
    bsp_remote_get(&s);

    int n;
    if (json_number(body, "mode",  &n)) s.mode  = (bsp_remote_mode_t)n;
    if (json_number(body, "level", &n)) s.level = n;
    json_bool(body, "led", &s.led);
    json_string(body, "label", s.label, sizeof(s.label));

    bsp_remote_set(&s);

    /* Echo the stored state back, so the page shows what was actually
     * accepted after clamping rather than what it hoped for. */
    bsp_remote_get(&s);

    char reply[192];
    state_to_json(&s, reply, sizeof(reply));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, reply);
}

/* ── Screenshot ─────────────────────────────────────────────────── */
static int http_sink(void *ctx, const void *data, size_t len)
{
    /* Chunked, so the megabyte never has to be buffered in RAM. */
    return httpd_resp_send_chunk((httpd_req_t *)ctx, (const char *)data, len) == ESP_OK
               ? 0 : -1;
}

static esp_err_t shot_get(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK) return ESP_OK;

    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "inline; filename=\"gution-screen.png\"");

    esp_err_t err = bsp_screenshot_write(http_sink, req);
    if (err != ESP_OK) {
        /* Headers are already out, so the only signal left is a short body. */
        ESP_LOGE(TAG, "screenshot failed: %s", esp_err_to_name(err));
    }

    httpd_resp_send_chunk(req, NULL, 0);   /* terminate the chunked response */
    return ESP_OK;
}

/* Capture straight to the SD card, for grabbing shots without a browser
 * holding a megabyte open. Returns the path that was written. */
static esp_err_t shot_save_get(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK) return ESP_OK;

    char      path[64] = {0};
    esp_err_t err      = bsp_screenshot_save_next(path, sizeof(path));

    char body[128];
    if (err == ESP_OK) {
        snprintf(body, sizeof(body), "{\"path\":\"%s\"}", path);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, body);
    }

    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", esp_err_to_name(err));
    httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict"
                                                            : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* ── Co-processor update ────────────────────────────────────────── */
static void copro_worker(void *arg)
{
    (void)arg;

    /* Restarts the board on success and never returns. */
    esp_err_t err = bsp_copro_update_force();
    ESP_LOGE(TAG, "Co-processor update failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

/*
 * Answers immediately and does the work on a task: pushing ~1.2 MB in
 * 1400-byte RPCs takes long enough that any client would time out, and the
 * board reboots at the end anyway, so there is nothing to report back to.
 */
static esp_err_t copro_post(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK) return ESP_OK;

    if (xTaskCreate(copro_worker, "copro_ota", 4096, NULL, 5, NULL) != pdPASS) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"could not start\"}");
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(
        req, "{\"status\":\"updating co-processor; the board will reboot\"}");
}

static void reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Rebooting into the uploaded image");
    esp_restart();
}

static void schedule_reboot(void)
{
    /* One-shot, so the HTTP response is flushed before the reset. */
    const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "ota_reboot" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, REBOOT_DELAY_US);
    } else {
        esp_restart();
    }
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGE(TAG, "Upload rejected: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK) return ESP_OK;

    if (req->content_len <= 0) {
        return fail(req, "400 Bad Request", "Empty request body");
    }

    ESP_LOGI(TAG, "Upload starting (%d bytes)", req->content_len);

    /* Reboot is scheduled by hand below, after the response is sent. */
    bsp_ota_config_t cfg = BSP_OTA_CONFIG_DEFAULT();
    cfg.reboot_on_success = false;

    esp_err_t err = bsp_ota_stream_begin((size_t)req->content_len, &cfg);
    if (err != ESP_OK) {
        return fail(req, "409 Conflict", esp_err_to_name(err));
    }

    uint8_t *buf = malloc(RECV_CHUNK_BYTES);
    if (buf == NULL) {
        bsp_ota_stream_abort();
        return fail(req, "500 Internal Server Error", "Out of memory");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining < RECV_CHUNK_BYTES ? remaining : RECV_CHUNK_BYTES;
        int got  = httpd_req_recv(req, (char *)buf, want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;   /* slow client, keep waiting */
        if (got <= 0) {
            free(buf);
            bsp_ota_stream_abort();
            return fail(req, "400 Bad Request", "Connection closed mid-upload");
        }

        err = bsp_ota_stream_write(buf, (size_t)got);
        if (err != ESP_OK) {
            free(buf);
            bsp_ota_stream_abort();
            return fail(req, "400 Bad Request", esp_err_to_name(err));
        }
        remaining -= got;
    }
    free(buf);

    err = bsp_ota_stream_end();
    if (err != ESP_OK) {
        return fail(req, "400 Bad Request", esp_err_to_name(err));
    }

    httpd_resp_sendstr(req, "Update written; rebooting");
    schedule_reboot();
    return ESP_OK;
}

/* ── Lifecycle ──────────────────────────────────────────────────── */
bool bsp_ota_web_is_running(void) { return s_server != NULL; }

esp_err_t bsp_ota_web_start(const bsp_ota_web_config_t *cfg)
{
    if (s_server) return ESP_OK;

    bsp_ota_web_config_t def = BSP_OTA_WEB_CONFIG_DEFAULT();
    if (cfg == NULL) cfg = &def;

    if (cfg->username) {
        ESP_RETURN_ON_ERROR(build_expected_auth(cfg->username, cfg->password),
                            TAG, "auth setup");
    }

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port      = cfg->port ? cfg->port : 80;
    http_cfg.lru_purge_enable = true;
    /* Flashing runs on this task, so it needs more room than the default. */
    http_cfg.stack_size       = 8192;
    /* A large image over a slow link must not be torn down mid-upload. */
    http_cfg.recv_wait_timeout = 30;
    http_cfg.send_wait_timeout = 30;

    esp_err_t err = httpd_start(&s_server, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        free(s_expected_auth);
        s_expected_auth = NULL;
        return err;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = page_get    },
        { .uri = "/info",   .method = HTTP_GET,  .handler = info_get    },
        { .uri = "/state",  .method = HTTP_GET,  .handler = state_get   },
        { .uri = "/state",  .method = HTTP_POST, .handler = state_post  },
        { .uri = "/shot.png",  .method = HTTP_GET, .handler = shot_get      },
        { .uri = "/shot/save", .method = HTTP_GET, .handler = shot_save_get },
        { .uri = "/copro",  .method = HTTP_POST, .handler = copro_post   },
        { .uri = "/update", .method = HTTP_POST, .handler = update_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    ESP_LOGI(TAG, "Upload page on port %u%s", http_cfg.server_port,
             s_expected_auth ? " (password protected)" : " (open — anyone on the network)");
    return ESP_OK;
}

esp_err_t bsp_ota_web_stop(void)
{
    if (!s_server) return ESP_OK;

    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    free(s_expected_auth);
    s_expected_auth = NULL;
    ESP_LOGI(TAG, "Upload page stopped");
    return err;
}
