/*
 * SoftAP WiFi provisioning.
 */

#include "bsp_wifi_prov.h"
#include "bsp_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"

static const char *TAG = "bsp_wifi_prov";

#define DEFAULT_AP_PREFIX  "gution-setup"
#define AP_MAX_CLIENTS     4
#define AP_CHANNEL         1
#define SCAN_MAX_APS       20
#define PROVISIONED_BIT    BIT0

static bool               s_active;
static httpd_handle_t     s_server;
static esp_netif_t       *s_ap_netif;
static EventGroupHandle_t s_events;
static TaskHandle_t       s_dns_task;
static volatile bool      s_dns_run;
static char               s_ap_ssid[33];

/* ── Setup page ─────────────────────────────────────────────────── */
static const char PAGE_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>WiFi setup</title><style>"
":root{color-scheme:light dark;--fg:#111;--bg:#fff;--mut:#666;--line:#d4d4d8;--ac:#2563eb}"
"@media(prefers-color-scheme:dark){:root{--fg:#e5e5e5;--bg:#18181b;--mut:#a1a1aa;--line:#3f3f46;--ac:#60a5fa}}"
"*{box-sizing:border-box}body{margin:0;padding:2rem 1rem;background:var(--bg);color:var(--fg);"
"font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;display:flex;justify-content:center}"
".card{width:100%;max-width:26rem}h1{font-size:1.25rem;margin:0 0 .25rem}"
".sub{color:var(--mut);font-size:.875rem;margin:0 0 1.5rem}"
"ul{list-style:none;margin:0 0 1rem;padding:0;border:1px solid var(--line);border-radius:.5rem;"
"max-height:15rem;overflow:auto}"
"li{padding:.6rem .875rem;border-bottom:1px solid var(--line);cursor:pointer;display:flex;"
"justify-content:space-between;gap:1rem}li:last-child{border-bottom:0}"
"li:hover,li.sel{background:color-mix(in srgb,var(--ac) 12%,transparent)}"
".nm{font-weight:500;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".rs{color:var(--mut);font-size:.8rem;font-variant-numeric:tabular-nums;flex:none}"
"input{width:100%;padding:.6rem .75rem;margin-bottom:.6rem;border:1px solid var(--line);"
"border-radius:.5rem;background:var(--bg);color:inherit;font:inherit}"
"button{width:100%;padding:.7rem;font-size:1rem;font-weight:600;border:0;border-radius:.5rem;"
"background:var(--ac);color:#fff;cursor:pointer}button:disabled{opacity:.5;cursor:default}"
"#msg{margin-top:.875rem;font-size:.875rem}.ok{color:#16a34a}.err{color:#dc2626}"
"</style></head><body><div class=card>"
"<h1>WiFi setup</h1><p class=sub>Guition JC4880P443C &middot; ESP32-P4</p>"
"<ul id=list><li><span class=nm>Scanning&hellip;</span></li></ul>"
"<input id=ssid placeholder='Network name' autocapitalize=off autocorrect=off spellcheck=false>"
"<input id=pass type=password placeholder='Password (blank if open)'>"
"<button id=b>Connect</button><div id=msg></div>"
"</div><script>"
"var L=document.getElementById('list'),S=document.getElementById('ssid'),"
"P=document.getElementById('pass'),B=document.getElementById('b'),M=document.getElementById('msg');"
"function scan(){fetch('/scan').then(r=>r.json()).then(j=>{L.innerHTML='';"
"if(!j.length){L.innerHTML='<li><span class=nm>No networks found</span></li>';return;}"
"j.forEach(n=>{var li=document.createElement('li');"
"li.innerHTML='<span class=nm></span><span class=rs></span>';"
"li.firstChild.textContent=(n.open?'':'\\uD83D\\uDD12 ')+n.ssid;"
"li.lastChild.textContent=n.rssi+' dBm';"
"li.onclick=()=>{S.value=n.ssid;"
"[].forEach.call(L.children,e=>e.classList.remove('sel'));li.classList.add('sel');"
"(n.open?B:P).focus();};L.appendChild(li);});})"
".catch(()=>{L.innerHTML='<li><span class=nm>Scan failed</span></li>';});}"
"scan();"
"B.onclick=()=>{if(!S.value){M.className='err';M.textContent='Pick a network first.';return;}"
"B.disabled=true;M.className='';M.textContent='Connecting\\u2026 this can take 20 seconds.';"
"fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:S.value,pass:P.value})})"
".then(r=>r.json().then(j=>({ok:r.ok,j:j})))"
".then(o=>{if(o.ok){M.className='ok';"
"M.textContent='Connected. IP '+o.j.ip+'. This network will now disappear.';}"
"else{M.className='err';M.textContent=o.j.error||'Could not connect.';B.disabled=false;}})"
".catch(()=>{M.className='err';M.textContent='Lost connection to the device.';B.disabled=false;});};"
"</script></body></html>";

/* ── Handlers ───────────────────────────────────────────────────── */
static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/*
 * Phones probe well-known URLs to detect a captive portal and open a browser
 * when the reply is not what they expect. Redirecting everything unknown to
 * the setup page is what makes that happen.
 */
static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < out_len; i++) {
        if (in[i] == '"' || in[i] == '\\') out[o++] = '\\';
        else if ((unsigned char)in[i] < 0x20) continue;   /* drop control bytes */
        out[o++] = in[i];
    }
    out[o] = '\0';
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_ap_record_t *aps = calloc(SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    if (aps == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "[]");
    }

    uint16_t  count = SCAN_MAX_APS;
    esp_err_t err   = bsp_wifi_scan(aps, &count);
    if (err != ESP_OK) count = 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    for (uint16_t i = 0; i < count; i++) {
        if (aps[i].ssid[0] == '\0') continue;   /* hidden network */

        char safe[80];
        json_escape((const char *)aps[i].ssid, safe, sizeof(safe));

        char entry[160];
        snprintf(entry, sizeof(entry), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                 i ? "," : "", safe, aps[i].rssi,
                 aps[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        httpd_resp_sendstr_chunk(req, entry);
    }

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    free(aps);
    return ESP_OK;
}

/* Minimal extractor for the two flat string fields the page sends. */
static bool json_get_string(const char *body, const char *key, char *out, size_t out_len)
{
    char pattern[32];
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
        if (*p == '\\' && p[1]) p++;   /* unescape */
        out[o++] = *p++;
    }
    out[o] = '\0';
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char body[256];
    int  len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int  got = httpd_req_recv(req, body, len);
    if (got <= 0) return send_json(req, "400 Bad Request", "{\"error\":\"Empty request\"}");
    body[got] = '\0';

    char ssid[BSP_WIFI_SSID_MAX + 1] = {0};
    char pass[BSP_WIFI_PASS_MAX + 1] = {0};

    if (!json_get_string(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return send_json(req, "400 Bad Request", "{\"error\":\"No network name given\"}");
    }
    json_get_string(body, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "Provisioning attempt for \"%s\"", ssid);

    /* Saved only after the credentials are proven to work, just below. */
    esp_err_t err = bsp_wifi_connect(ssid, pass, false);
    if (err != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"Could not start connection\"}");
    }

    if (bsp_wifi_wait_connected(25000) != ESP_OK) {
        ESP_LOGW(TAG, "\"%s\" did not connect", ssid);
        return send_json(req, "400 Bad Request",
                         "{\"error\":\"Could not join that network - check the password\"}");
    }

    /* Now persist them, without disturbing the connection just made. */
    bsp_wifi_save_credentials(ssid, pass);

    esp_ip4_addr_t ip = {0};
    bsp_wifi_get_ip(&ip);

    char reply[64];
    snprintf(reply, sizeof(reply), "{\"ip\":\"" IPSTR "\"}", IP2STR(&ip));

    ESP_LOGI(TAG, "Provisioned — joined \"%s\" as " IPSTR, ssid, IP2STR(&ip));
    send_json(req, "200 OK", reply);

    xEventGroupSetBits(s_events, PROVISIONED_BIT);
    return ESP_OK;
}

/* ── Captive-portal DNS ─────────────────────────────────────────── */
/*
 * Answers every A query with the AP's own address. Enough to trigger the
 * "sign in to network" prompt; it is not a general-purpose resolver.
 */
static void dns_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* So the task notices s_dns_run going false instead of blocking forever. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(s_ap_netif, &ip_info);

    uint8_t buf[256];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t          from_len = sizeof(from);

        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12) continue;                 /* shorter than a DNS header */
        if (n + 16 > (int)sizeof(buf)) continue;

        buf[2] |= 0x80;   /* QR: response */
        buf[3] |= 0x80;   /* recursion available */
        buf[7]  = buf[5]; /* answer count := question count */

        /* Append one answer: pointer to the question name, A, IN, TTL, addr. */
        uint8_t *a = buf + n;
        *a++ = 0xC0; *a++ = 0x0C;                       /* name compression -> offset 12 */
        *a++ = 0x00; *a++ = 0x01;                       /* type A  */
        *a++ = 0x00; *a++ = 0x01;                       /* class IN */
        *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x3C;  /* TTL 60 s */
        *a++ = 0x00; *a++ = 0x04;                       /* RDLENGTH 4 */
        memcpy(a, &ip_info.ip.addr, 4);
        a += 4;

        sendto(sock, buf, a - buf, 0, (struct sockaddr *)&from, from_len);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ── Lifecycle ──────────────────────────────────────────────────── */
bool bsp_wifi_prov_is_needed(void) { return !bsp_wifi_has_credentials(); }
bool bsp_wifi_prov_is_active(void) { return s_active; }

static void default_ap_name(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};

    /*
     * Must come from esp_wifi_get_mac(), not esp_read_mac(): the WiFi MAC
     * belongs to the C6, not the P4's efuse, so esp_read_mac() fails here with
     * "mac type is incorrect (not found)" and leaves the name ending -0000.
     * esp_wifi_get_mac() fetches it over RPC and needs esp_wifi_init() first,
     * which bsp_wifi_init() has already done by this point.
     */
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read the AP MAC (%s) — using an unsuffixed name",
                 esp_err_to_name(err));
        strlcpy(out, DEFAULT_AP_PREFIX, out_len);
        return;
    }

    snprintf(out, out_len, "%s-%02x%02x", DEFAULT_AP_PREFIX, mac[4], mac[5]);
}

esp_err_t bsp_wifi_prov_start(const bsp_wifi_prov_config_t *cfg)
{
    if (s_active) return ESP_OK;

    bsp_wifi_prov_config_t def = BSP_WIFI_PROV_CONFIG_DEFAULT();
    if (cfg == NULL) cfg = &def;

    /* Creates the STA netif, the event loop and the link to the C6. */
    ESP_RETURN_ON_ERROR(bsp_wifi_init(), TAG, "wifi init");

    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");
    }
    xEventGroupClearBits(s_events, PROVISIONED_BIT);

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_ap_netif, ESP_FAIL, TAG, "create ap netif");
    }

    if (cfg->ap_ssid) {
        strlcpy(s_ap_ssid, cfg->ap_ssid, sizeof(s_ap_ssid));
    } else {
        /* Distinct per board, so several units can be set up side by side. */
        default_ap_name(s_ap_ssid, sizeof(s_ap_ssid));
    }

    bool open_ap = (cfg->ap_password == NULL || strlen(cfg->ap_password) < 8);
    if (cfg->ap_password && open_ap) {
        ESP_LOGW(TAG, "AP password shorter than 8 characters — running open instead");
    }

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len       = strlen(s_ap_ssid);
    ap.ap.channel        = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CLIENTS;
    ap.ap.authmode       = open_ap ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    if (!open_ap) strlcpy((char *)ap.ap.password, cfg->ap_password, sizeof(ap.ap.password));

    /* AP+STA: the AP serves the page while the station scans and test-connects. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set apsta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "set ap config");

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port      = 80;
    http_cfg.stack_size       = 6144;
    http_cfg.lru_purge_enable = true;
    /* The catch-all redirect needs wildcard matching. */
    http_cfg.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s (is the upload page already on port 80?)",
                 esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = page_get     },
        { .uri = "/scan",    .method = HTTP_GET,  .handler = scan_get     },
        { .uri = "/connect", .method = HTTP_POST, .handler = connect_post },
        { .uri = "/*",       .method = HTTP_GET,  .handler = redirect_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    if (cfg->captive_dns) {
        s_dns_run = true;
        if (xTaskCreate(dns_task, "prov_dns", 3072, NULL, 5, &s_dns_task) != pdPASS) {
            ESP_LOGW(TAG, "DNS responder not started — browse to the AP address by hand");
            s_dns_run = false;
        }
    }

    s_active = true;

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(s_ap_netif, &ip_info);
    ESP_LOGI(TAG, "Setup AP \"%s\"%s — open http://" IPSTR "/",
             s_ap_ssid, open_ap ? " (open)" : "", IP2STR(&ip_info.ip));

    return ESP_OK;
}

esp_err_t bsp_wifi_prov_stop(void)
{
    if (!s_active) return ESP_OK;

    s_dns_run = false;
    /* dns_task wakes within its 1 s receive timeout and deletes itself. */
    for (int i = 0; i < 20 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(100));

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    /* Back to station-only; the AP has done its job. */
    esp_wifi_set_mode(WIFI_MODE_STA);

    s_active = false;
    ESP_LOGI(TAG, "Setup AP stopped");
    return ESP_OK;
}

esp_err_t bsp_wifi_prov_wait(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_INVALID_STATE, TAG, "not started");

    EventBits_t bits = xEventGroupWaitBits(s_events, PROVISIONED_BIT, pdFALSE, pdTRUE,
                                           timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);
    return (bits & PROVISIONED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
