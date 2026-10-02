/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Wi-Fi without accounts.
 *
 * Boot: connect with the network the Wi-Fi driver saved in NVS (it also reads what the voice_chat app's
 * provisioning saved). With no saved network, or after SETUP_AFTER_FAILS failures in a row, open the
 * setup network: an open access point "VoCat-Setup-XXXX" with a DNS server that answers every name with
 * the board's address, so phones show the setup page as a captive portal. The page lists the networks
 * found by a scan and posts the chosen one with its password. Trying a network writes it to flash (the
 * Wi-Fi driver skips a write when the configuration hasn't changed, so trying it in RAM and writing the
 * same values afterwards saved nothing); if it doesn't connect, the previous network is written back.
 * Once connected, the setup network closes a few seconds later.
 *
 * Our own esp_wifi_disconnect() calls show up as disconnects with reason WIFI_REASON_ASSOC_LEAVE and are
 * not counted as failures.
 *
 * Threading: wifi_task owns all Wi-Fi control. Wi-Fi/IP events, the web handlers, timers and
 * wifi_setup_forget() only post commands to its queue. s_lock guards what the web handlers read.
 */

#include "wifi_setup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_check.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "dns_server.h"
#include "app_dashboard.h"
#include "app_power.h"
#include "app_settings.h"
#include "app_usb_hid.h"

static const char *TAG = "wifi_setup";

#define SETUP_SSID_PREFIX   "VoCat-Setup-"
#define SETUP_ADDRESS       "192.168.4.1"   /* the access point's default address */
#define SETUP_AFTER_FAILS   3           /* failed connects in a row before the setup network opens */
#define TRIAL_ATTEMPTS      2           /* tries for a network entered on the setup page */
#define RETRY_MIN_MS        2000
#define RETRY_MAX_MS        30000
#define RETRY_IN_SETUP_MS   60000       /* while the setup network is open, retry the saved one rarely:
                                           each try scans all channels and briefly drops the setup network */
#define SETUP_CLOSE_MS      8000        /* keep the setup network after connecting, so the page can say so */
#define MAX_NETS            16
#define SCAN_DONE_BIT       BIT0

typedef enum {
    CMD_START,
    CMD_DISCONNECTED,
    CMD_GOT_IP,
    CMD_RETRY,
    CMD_TRY_NETWORK,        /* from the setup page: s_trial holds the network */
    CMD_RESCAN,
    CMD_FORGET,
    CMD_CLOSE_SETUP,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint8_t reason;         /* CMD_DISCONNECTED: wifi_err_reason_t */
} cmd_t;

typedef enum {
    STATUS_IDLE,
    STATUS_CONNECTING,
    STATUS_CONNECTED,
    STATUS_FAILED,
} status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} net_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry_timer, s_close_timer;

/* wifi_task only */
static bool s_have_saved;           /* a network is saved in flash */
static bool s_connected;
static bool s_setup_open;
static bool s_trying;               /* connecting to the network from the setup page */
static wifi_config_t s_saved;       /* what is saved in flash (esp_wifi_get_config returns a RAM-only trial) */
static int s_fails;                 /* failed connects in a row */
static int s_trial_fails;
static bool s_sntp_started;
static httpd_handle_t s_httpd;
static dns_server_handle_t s_dns;
static char s_setup_ssid[33];

/* Under s_lock: read by the web handlers */
static struct {
    net_t nets[MAX_NETS];
    int net_count;
    status_t status;
    char status_ssid[33];
    char message[64];
} s_web;

/* Under s_lock: the network entered on the setup page */
static wifi_config_t s_trial;

/* ------------------------------------------------------------------------------------------------
 * Commands
 * ---------------------------------------------------------------------------------------------- */

static void post(cmd_type_t type, uint8_t reason)
{
    cmd_t cmd = { .type = type, .reason = reason };
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropped %d", type);
    }
}

static void set_status(status_t status, const char *ssid, const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_web.status = status;
    if (ssid) {
        strlcpy(s_web.status_ssid, ssid, sizeof(s_web.status_ssid));
    }
    strlcpy(s_web.message, message ? message : "", sizeof(s_web.message));
    xSemaphoreGive(s_lock);
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return "Wrong password.";
        case WIFI_REASON_NO_AP_FOUND:
            return "Network not found.";
        default:
            return "Couldn't connect.";
    }
}

/* ------------------------------------------------------------------------------------------------
 * Scan
 * ---------------------------------------------------------------------------------------------- */

static int cmp_rssi(const void *a, const void *b)
{
    return ((const net_t *)b)->rssi - ((const net_t *)a)->rssi;
}

/* Adds or updates `ssid` in nets[0..*n), keeping each network's strongest signal */
static void merge_net(net_t *nets, int *n, const wifi_ap_record_t *rec)
{
    const char *ssid = (const char *)rec->ssid;
    for (int j = 0; j < *n; j++) {
        if (strcmp(nets[j].ssid, ssid) == 0) {
            if (rec->rssi > nets[j].rssi) {
                nets[j].rssi = rec->rssi;
            }
            return;
        }
    }
    if (*n < MAX_NETS) {
        strlcpy(nets[*n].ssid, ssid, sizeof(nets[*n].ssid));
        nets[*n].rssi = rec->rssi;
        nets[*n].open = rec->authmode == WIFI_AUTH_OPEN;
        (*n)++;
    }
}

/*
 * Blocking scan; the station must not be connecting. The default scan listens only briefly on each channel
 * and missed networks a phone lists, so listen longer and, when the setup network isn't up yet, scan twice
 * and merge. While the setup network is up, one shorter pass: each channel hop takes the radio away from
 * the phone on the setup network.
 */
static void scan_networks(void)
{
    const wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 120, .max = s_setup_open ? 150 : 320 },
    };
    int passes = s_setup_open ? 1 : 2;

    net_t nets[MAX_NETS];
    int n = 0, seen = 0;
    for (int pass = 0; pass < passes; pass++) {
        esp_err_t err = esp_wifi_scan_start(&cfg, true);
        uint16_t count = 0;
        if (err == ESP_OK) {
            err = esp_wifi_scan_get_ap_num(&count);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
            esp_wifi_clear_ap_list();
            continue;
        }
        wifi_ap_record_t *records = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
        if (!records) {
            esp_wifi_clear_ap_list();
            break;
        }
        esp_wifi_scan_get_ap_records(&count, records);
        seen += count;
        for (int i = 0; i < count; i++) {
            if (records[i].ssid[0]) {       /* hidden networks: use "Other network" on the page */
                merge_net(nets, &n, &records[i]);
            }
        }
        free(records);
    }
    if (n == 0 && seen == 0) {
        return;     /* keep the last list rather than show an empty one */
    }
    qsort(nets, n, sizeof(net_t), cmp_rssi);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_web.nets, nets, sizeof(net_t) * n);
    s_web.net_count = n;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Scan (%d pass%s): %d access points, %d named networks", passes, passes > 1 ? "es" : "", seen, n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %-32s %4d dBm%s", nets[i].ssid, nets[i].rssi, nets[i].open ? "  open" : "");
    }
}

/* ------------------------------------------------------------------------------------------------
 * Setup page
 * ---------------------------------------------------------------------------------------------- */

/* The pages use no JavaScript: the captive-portal windows of phones and laptops don't reliably run it.
 * The list links each network to its own password page, and the result page refreshes itself.
 */
static const char PAGE_START[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>VoCat Wi-Fi</title>";

static const char PAGE_STYLE[] =
    "<style>"
    "*{box-sizing:border-box}"
    "body{margin:0;font:16px/1.45 system-ui,-apple-system,sans-serif;background:#0b0f13;color:#e9eef4}"
    "main{max-width:452px;margin:0 auto;padding:28px 16px 40px}"
    "h1{font-size:22px;margin:0 0 6px}p{color:#8a94a6;margin:0 0 18px}a{color:#5ec8f2}b{color:#e9eef4}"
    ".net{display:flex;align-items:center;gap:12px;padding:16px;margin:0 0 8px;background:#141920;"
    "border:1px solid #252c36;border-radius:12px;color:inherit;text-decoration:none}"
    ".net:active,.net:focus{border-color:#5ec8f2}"
    ".name{flex:1;overflow-wrap:anywhere}.sig{color:#5ec8f2;font-size:13px;letter-spacing:1px}.sig i{color:#2c3a47;font-style:normal}"
    ".tag{color:#8a94a6;font-size:13px}"
    "label.f{display:block;margin:18px 0 6px;color:#8a94a6;font-size:14px}"
    "input.t{box-sizing:border-box;width:100%;padding:14px;border-radius:12px;border:1px solid #252c36;"
    "background:#141920;color:inherit;font:inherit}input.t:focus{outline:2px solid #5ec8f2;border-color:transparent}"
    ".go{margin-top:22px;width:100%;padding:14px;border:0;border-radius:12px;background:#5ec8f2;"
    "color:#03222f;font:600 16px system-ui,sans-serif;cursor:pointer}"
    ".tag{color:#8a94a6;font-size:13px;margin:0 0 18px}.ok{color:#5ccb8f;margin:0 0 14px}"
    ".note{color:#8a94a6;font-size:12px;margin:8px 0 0}"
    ".seg{display:flex;gap:6px}.seg-item{flex:1;display:flex;align-items:center;justify-content:center;gap:6px;"
    "padding:11px 4px;background:#141920;border:1px solid #252c36;border-radius:12px;cursor:pointer;font-size:14px}"
    ".seg-item.on{border-color:#5ec8f2;color:#5ec8f2}.seg-item input{accent-color:#5ec8f2;margin:0}"
    ".range{width:100%;accent-color:#5ec8f2}"
    "h2{font-size:17px;margin:30px 0 0}.texts input.t{margin-bottom:8px}"
    ".swatches{display:flex;flex-wrap:wrap;gap:8px}.sw{display:flex;align-items:center;gap:8px;padding:9px 12px;"
    "background:#141920;border:1px solid #252c36;border-radius:12px;cursor:pointer;font-size:14px}"
    ".sw.on{border-color:#e9eef4}.sw i{width:18px;height:18px;border-radius:50%;display:inline-block}"
    ".sw input{margin:0}.sw input[type=color]{width:34px;height:22px;border:0;padding:0;background:none}"
    "input.num{width:90px}"
    ".btn{display:block;margin-top:10px;width:100%;padding:13px;text-align:center;border-radius:12px;"
    "border:1px solid #252c36;background:transparent;color:#e9eef4;font:500 15px system-ui,sans-serif;"
    "text-decoration:none;cursor:pointer}.btn.warn{border-color:#4a2a2a;color:#f0a19a}"
    "</style></head><body><main>";

static const char PAGE_END[] = "</main></body></html>";

/* Sends a piece of a page. httpd_resp_sendstr_chunk() with "" sends a zero-length chunk, which ends the
 * whole response (browsers then showed the list only up to the first network), so skip empty pieces.
 * The page ends with send_page_end().
 */
static void send_str(httpd_req_t *req, const char *s)
{
    if (s && *s) {
        httpd_resp_sendstr_chunk(req, s);
    }
}

/* Sends `s` with HTML special characters escaped */
static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[200];
    size_t n = 0;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        size_t len = rep ? strlen(rep) : 1;
        if (n + len >= sizeof(buf)) {
            httpd_resp_send_chunk(req, buf, n);
            n = 0;
        }
        if (rep) {
            memcpy(buf + n, rep, len);
        } else {
            buf[n] = *s;
        }
        n += len;
    }
    if (n) {
        httpd_resp_send_chunk(req, buf, n);
    }
}

/* Logs each request to the setup page, and asks for a fresh connection per request. The server
 * recycles idle connections when a phone's background checks open new ones, and a request sent on a
 * connection that was just recycled fails before it reaches the board.
 */
static void log_request(httpd_req_t *req)
{
    char host[48] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    ESP_LOGI(TAG, "%s %s (Host: %s)", req->method == HTTP_POST ? "POST" : "GET", req->uri, host);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

/* Starts an HTML page; refresh_s > 0 reloads it after that many seconds */
static void send_page_start(httpd_req_t *req, int refresh_s)
{
    httpd_resp_set_type(req, "text/html");
    send_str(req, PAGE_START);
    if (refresh_s > 0) {
        char meta[48];
        snprintf(meta, sizeof(meta), "<meta http-equiv=refresh content=%d>", refresh_s);
        send_str(req, meta);
    }
    send_str(req, PAGE_STYLE);
}

static esp_err_t send_page_end(httpd_req_t *req)
{
    send_str(req, PAGE_END);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* Value of `key` in an x-www-form-urlencoded body or URL query, decoded; false if missing or too long */
static bool form_value(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            size_t n = 0;
            while (*p && *p != '&') {
                char c = *p++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
                    char hex[3] = { p[0], p[1], 0 };
                    c = (char)strtol(hex, NULL, 16);
                    p += 2;
                }
                if (n + 1 >= out_size) {
                    return false;
                }
                out[n++] = c;
            }
            out[n] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

/* Sends `s` percent-encoded for a URL query (the result needs no further HTML escaping) */
static void send_url_encoded(httpd_req_t *req, const char *s)
{
    char buf[3 * 32 + 1];
    size_t n = 0;
    for (; *s && n + 3 < sizeof(buf); s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            buf[n++] = (char)c;
        } else {
            n += snprintf(buf + n, sizeof(buf) - n, "%%%02X", c);
        }
    }
    buf[n] = '\0';
    send_str(req, buf);
}

/* The list of networks: each one is a link to its own password page, like a phone's Wi-Fi picker */
static esp_err_t list_get(httpd_req_t *req)
{
    log_request(req);
    char query[32] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (strstr(query, "rescan")) {
        xEventGroupClearBits(s_events, SCAN_DONE_BIT);
        post(CMD_RESCAN, 0);
        xEventGroupWaitBits(s_events, SCAN_DONE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(6000));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = s_web.net_count;
    net_t nets[MAX_NETS];
    memcpy(nets, s_web.nets, sizeof(net_t) * count);
    xSemaphoreGive(s_lock);

    send_page_start(req, 0);
    send_str(req, "<h1>Connect VoCat to Wi-Fi</h1><p>Tap your network. VoCat uses 2.4 GHz "
                                  "Wi-Fi, so 5 GHz-only networks aren't listed.</p>");
    for (int i = 0; i < count; i++) {
        int bars = nets[i].rssi > -55 ? 4 : nets[i].rssi > -65 ? 3 : nets[i].rssi > -75 ? 2 : 1;
        static const char *const bar_html[5] = { "", "\xE2\x96\x82<i>\xE2\x96\x84\xE2\x96\x86\xE2\x96\x88</i>",
                                                  "\xE2\x96\x82\xE2\x96\x84<i>\xE2\x96\x86\xE2\x96\x88</i>",
                                                  "\xE2\x96\x82\xE2\x96\x84\xE2\x96\x86<i>\xE2\x96\x88</i>",
                                                  "\xE2\x96\x82\xE2\x96\x84\xE2\x96\x86\xE2\x96\x88" };
        send_str(req, "<a class=net href=\"/join?s=");
        send_url_encoded(req, nets[i].ssid);
        send_str(req, "\"><span class=name>");
        send_escaped(req, nets[i].ssid);
        send_str(req, nets[i].open ? " <span class=tag>open</span>" : "");
        send_str(req, "</span><span class=sig>");
        send_str(req, bar_html[bars]);
        send_str(req, "</span></a>");
    }
    if (count == 0) {
        send_str(req, "<p>No networks found.</p>");
    }
    ESP_LOGI(TAG, "List page: %d networks", count);
    send_str(req,
        "<a class=net href=\"/join\"><span class=name>Other network\xE2\x80\xA6</span></a>"
        "<p style=margin-top:18px><a href=\"/wifi?rescan=1\">Scan again</a> · "
        "<a href=\"/settings\">VoCat settings</a></p>");
    return send_page_end(req);
}

/* One network's page: its password (if it has one) and Connect. Without a name, asks for the name too. */
static esp_err_t join_get(httpd_req_t *req)
{
    log_request(req);
    char query[128] = "", ssid[33] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        form_value(query, "s", ssid, sizeof(ssid));
    }

    bool open = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_web.net_count; i++) {
        if (ssid[0] && strcmp(s_web.nets[i].ssid, ssid) == 0) {
            open = s_web.nets[i].open;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    send_page_start(req, 0);
    send_str(req, "<form method=post action=/connect>");
    if (ssid[0]) {
        send_str(req, "<h1>");
        send_escaped(req, ssid);
        send_str(req, open ? "</h1><p>This network has no password.</p>"
                                           : "</h1><p>Enter the Wi-Fi password.</p>");
        send_str(req, "<input type=hidden name=s value=\"");
        send_escaped(req, ssid);
        send_str(req, "\">");
    } else {
        send_str(req, "<h1>Other network</h1><p>For a network that doesn't appear in the list.</p>"
                                      "<label class=f for=s>Network name</label>"
                                      "<input class=t id=s name=s maxlength=32 required autocapitalize=none autocorrect=off>");
    }
    if (!open) {
        send_str(req, "<label class=f for=p>Password</label>"
                                      "<input class=t id=p name=p type=password maxlength=64 autofocus>");
    }
    send_str(req, "<button class=go>Connect</button></form>"
                                  "<p style=margin-top:18px><a href=\"/\">Back to the list</a></p>");
    return send_page_end(req);
}

static esp_err_t redirect(httpd_req_t *req, const char *status, const char *location)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    log_request(req);
    char body[256];
    if (req->content_len <= 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form");
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[33] = "", pass[65] = "";
    form_value(body, "s", ssid, sizeof(ssid));
    if (!ssid[0]) {
        return redirect(req, "303 See Other", "/join");
    }
    form_value(body, "p", pass, sizeof(pass));
    ESP_LOGI(TAG, "Setup page: join \"%s\" (password %u characters)", ssid, (unsigned)strlen(pass));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_trial, 0, sizeof(s_trial));
    strlcpy((char *)s_trial.sta.ssid, ssid, sizeof(s_trial.sta.ssid));
    strlcpy((char *)s_trial.sta.password, pass, sizeof(s_trial.sta.password));
    s_trial.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    xSemaphoreGive(s_lock);
    set_status(STATUS_CONNECTING, ssid, NULL);

    post(CMD_TRY_NETWORK, 0);
    return redirect(req, "303 See Other", "/result");
}

static esp_err_t result_get(httpd_req_t *req)
{
    log_request(req);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    status_t status = s_web.status;
    char ssid[33], message[64];
    strlcpy(ssid, s_web.status_ssid, sizeof(ssid));
    strlcpy(message, s_web.message, sizeof(message));
    xSemaphoreGive(s_lock);

    if (status == STATUS_IDLE) {
        return redirect(req, "302 Found", "/");
    }
    send_page_start(req, status == STATUS_CONNECTING ? 2 : 0);
    switch (status) {
        case STATUS_CONNECTING:
            send_str(req, "<h1>Connecting\xE2\x80\xA6</h1><p>VoCat is joining <b>");
            send_escaped(req, ssid);
            send_str(req, "</b>.</p><p>If this page stops updating, look at VoCat: it shows the "
                                          "clock once it is online, and this setup network closes.</p>");
            break;
        case STATUS_CONNECTED:
            send_str(req, "<h1>Connected</h1><p>VoCat is online on <b>");
            send_escaped(req, ssid);
            send_str(req, "</b>. This setup network closes in a few seconds.</p>");
            break;
        case STATUS_FAILED:
        default:
            send_str(req, "<h1>Couldn't connect</h1><p>");
            send_escaped(req, message);
            send_str(req, "</p><p><a href=\"/\">Try again</a></p>");
            break;
    }
    return send_page_end(req);
}

/* What the board is called and where it is, for the top of the settings page */
static void send_status_line(httpd_req_t *req)
{
    char ssid[33] = "";
    if (s_connected) {
        wifi_config_t cfg = { 0 };
        esp_wifi_get_config(WIFI_IF_STA, &cfg);
        strlcpy(ssid, (const char *)cfg.sta.ssid, sizeof(ssid));
    }
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
    char line[80];
    snprintf(line, sizeof(line), "%s · " IPSTR, ssid[0] ? ssid : "not connected", IP2STR(&ip.ip));
    send_str(req, "<p class=tag>");
    send_escaped(req, line);
    send_str(req, "</p>");
}

/* Neurons page colours offered as swatches; any other colour is "Custom" */
static const struct {
    uint32_t rgb;
    const char *name;
} s_neuron_colors[] = {
    { 0x5EC8F2, "Cyan" }, { 0xFFC857, "Amber" }, { 0xFF6B5E, "Red" },
    { 0x7CE38B, "Green" }, { 0xC792EA, "Violet" }, { 0xF2F5F8, "White" },
};
#define NEURON_COLOR_COUNT (sizeof(s_neuron_colors) / sizeof(s_neuron_colors[0]))

/* The Neurons page section of the settings form: its texts, colour and hold time */
static void send_neurons_fields(httpd_req_t *req)
{
    send_str(req, "<h2>Neurons page</h2>"
                  "<p class=note>Tap the Neurons page to play the name, then these texts. Empty ones are skipped.</p>"
                  "<div class=texts><label class=f>Texts after the name</label>");
    for (int i = 0; i < APP_SETTINGS_TEXT_COUNT; i++) {
        char text[APP_SETTINGS_NAME_MAX + 1];
        app_settings_get_text(i, text, sizeof(text));
        char field[120];
        snprintf(field, sizeof(field), "<input class=t name=t%d maxlength=32 aria-label=\"Text %d\" "
                 "placeholder=\"Text %d (optional)\" value=\"", i + 1, i + 2, i + 2);
        send_str(req, field);
        send_escaped(req, text);
        send_str(req, "\">");
    }
    send_str(req, "</div><label class=f>Colour</label><div class=swatches>");

    uint32_t color = app_settings_get_neuron_color();
    bool preset = false;
    for (int i = 0; i < NEURON_COLOR_COUNT; i++) {
        bool on = s_neuron_colors[i].rgb == color;
        preset = preset || on;
        char item[220];
        snprintf(item, sizeof(item),
                 "<label class=\"sw%s\"><input type=radio name=c value=%06lX%s><i style=\"background:#%06lX\"></i>%s</label>",
                 on ? " on" : "", (unsigned long)s_neuron_colors[i].rgb, on ? " checked" : "",
                 (unsigned long)s_neuron_colors[i].rgb, s_neuron_colors[i].name);
        send_str(req, item);
    }
    char custom[260];
    snprintf(custom, sizeof(custom),
             "<label class=\"sw%s\"><input type=radio name=c value=custom%s>Custom "
             "<input type=color name=cx value=\"#%06lX\" aria-label=\"Custom colour\"></label></div>",
             preset ? "" : " on", preset ? "" : " checked", (unsigned long)color);
    send_str(req, custom);

    char hold[220];
    snprintf(hold, sizeof(hold),
             "<label class=f for=h>Seconds on each text</label>"
             "<input class=\"t num\" id=h name=h type=number min=%d max=%d value=%d>"
             "<p class=note>Each play varies this a little, so no two look the same.</p>",
             APP_SETTINGS_NEURON_HOLD_MIN, APP_SETTINGS_NEURON_HOLD_MAX, app_settings_get_neuron_hold());
    send_str(req, hold);
}

static esp_err_t settings_get(httpd_req_t *req)
{
    log_request(req);
    char query[32] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char name[APP_SETTINGS_NAME_MAX + 1];
    app_settings_get_name(name, sizeof(name));
    app_settings_os_t os = app_settings_get_os();
    int brightness = app_settings_get_brightness();

    send_page_start(req, 0);
    send_str(req, "<h1>VoCat</h1>");
    send_status_line(req);
    if (strstr(query, "saved")) {
        send_str(req, "<p class=ok>Saved.</p>");
    }

    send_str(req, "<form method=post action=/settings>"
                  "<label class=f for=n>Name on screen</label>"
                  "<input class=t id=n name=n maxlength=32 placeholder=\"No name\" value=\"");
    send_escaped(req, name);
    send_str(req, "\">"
                  "<label class=f>Keys are for</label><div class=seg>");
    for (int i = 0; i <= APP_SETTINGS_OS_COUNT; i++) {
        char item[200];
        const char *label = i == APP_SETTINGS_OS_COUNT ? "Ask me" : app_settings_os_name((app_settings_os_t)i);
        snprintf(item, sizeof(item),
                 "<label class=\"seg-item%s\"><input type=radio name=o value=%d%s><span>%s</span></label>",
                 os == i ? " on" : "", i, os == i ? " checked" : "", label);
        send_str(req, item);
    }
    char bright[220];
    snprintf(bright, sizeof(bright),
             "</div><label class=f for=b>Brightness: %d%%</label>"
             "<input class=range id=b name=b type=range min=10 max=100 step=5 value=%d>", brightness, brightness);
    send_str(req, bright);
    send_neurons_fields(req);
    send_str(req, "<button class=go>Save</button></form>");

    send_str(req, "<label class=f>Wi-Fi</label>"
                  "<a class=btn href=\"/wifi\">Change network</a>"
                  "<form method=post action=/off onsubmit=\"return confirm('Turn VoCat off?')\">"
                  "<button class=btn warn>Turn VoCat off</button></form>"
                  "<p class=note>Switching on again needs the button on the board.</p>");
    return send_page_end(req);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    log_request(req);
    static char body[1024];     /* the server handles one request at a time */
    if (req->content_len <= 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form");
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    char value[APP_SETTINGS_NAME_MAX + 1];
    if (form_value(body, "n", value, sizeof(value))) {
        app_settings_set_name(value);
    }
    char num[8];
    if (form_value(body, "o", num, sizeof(num))) {
        app_settings_set_os((app_settings_os_t)atoi(num));
    }
    if (form_value(body, "b", num, sizeof(num))) {
        app_settings_set_brightness(atoi(num));
    }
    for (int i = 0; i < APP_SETTINGS_TEXT_COUNT; i++) {
        char key[4], text[APP_SETTINGS_NAME_MAX + 1];
        snprintf(key, sizeof(key), "t%d", i + 1);
        if (form_value(body, key, text, sizeof(text))) {
            app_settings_set_text(i, text);
        }
    }
    char color[12];
    if (form_value(body, "c", color, sizeof(color))) {
        if (strcmp(color, "custom") == 0 && !form_value(body, "cx", color, sizeof(color))) {
            color[0] = '\0';
        }
        const char *hex = color[0] == '#' ? color + 1 : color;
        char *end;
        unsigned long rgb = strtoul(hex, &end, 16);
        if (strlen(hex) == 6 && *end == '\0') {
            app_settings_set_neuron_color((uint32_t)rgb);
        }
    }
    if (form_value(body, "h", num, sizeof(num))) {
        app_settings_set_neuron_hold(atoi(num));
    }
    ESP_LOGI(TAG, "Settings page: name \"%s\", keys for %s, brightness %d%%", value,
             app_settings_os_name(app_settings_get_os()), app_settings_get_brightness());
    return redirect(req, "303 See Other", "/?saved=1");
}

static void off_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(700));     /* let the page reach the browser first */
    app_power_off();
    vTaskDelete(NULL);
}

static esp_err_t off_post(httpd_req_t *req)
{
    log_request(req);
    send_page_start(req, 0);
    send_str(req, "<h1>Switching off</h1><p>Press the button on VoCat to switch it on again.</p>");
    esp_err_t err = send_page_end(req);
    xTaskCreate(off_task, "web_off", 3072, NULL, 5, NULL);
    return err;
}

/* "/" is the settings page at home, and the network list on the setup network */
static esp_err_t root_get(httpd_req_t *req)
{
    return s_setup_open ? list_get(req) : settings_get(req);
}

/* Anything else (a phone's captive-portal probe, a typed address) goes to the setup page */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    log_request(req);
    return redirect(req, "302 Found", "http://" SETUP_ADDRESS "/");
}

/* ------------------------------------------------------------------------------------------------
 * Setup network
 * ---------------------------------------------------------------------------------------------- */

static void open_setup(void)
{
    if (s_setup_open) {
        return;
    }
    /* Scan while the station is idle (stop a connect in progress), before the access point starts */
    esp_wifi_disconnect();
    scan_networks();

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_setup_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_setup_ssid);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Setup network didn't start: %s", esp_err_to_name(err));
        return;
    }

    /* Tell phones the setup page's address in DHCP (option 114, RFC 8910), so they don't depend on
     * detecting the captive portal through DNS. dhcps keeps the pointer, hence static.
     */
    static const char portal_uri[] = "http://" SETUP_ADDRESS;
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_dhcps_stop(ap_netif);
    if (esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                               (void *)portal_uri, strlen(portal_uri)) != ESP_OK) {
        ESP_LOGW(TAG, "Couldn't set the DHCP captive portal option");
    }
    esp_netif_dhcps_start(ap_netif);

    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns);

    s_setup_open = true;
    set_status(STATUS_IDLE, NULL, NULL);
    app_dashboard_set_wifi_setup(s_setup_ssid, SETUP_ADDRESS);
    ESP_LOGI(TAG, "Setup network %s open: join it and follow the page", s_setup_ssid);
}

static void close_setup(void)
{
    if (!s_setup_open) {
        return;
    }
    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_setup_open = false;
    app_dashboard_set_wifi_setup(NULL, NULL);
    ESP_LOGI(TAG, "Setup network closed");
}

/* ------------------------------------------------------------------------------------------------
 * Wi-Fi task
 * ---------------------------------------------------------------------------------------------- */

static void schedule_retry(void)
{
    uint64_t ms = RETRY_MIN_MS << (s_fails > 4 ? 4 : s_fails);
    if (ms > RETRY_MAX_MS) {
        ms = RETRY_MAX_MS;
    }
    if (s_setup_open) {
        ms = RETRY_IN_SETUP_MS;
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, ms * 1000);
}

static void connect_saved(void)
{
    if (!s_have_saved || s_connected || s_trying) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "Connect: %s", esp_err_to_name(err));
    }
}

static void start_trial(void)
{
    wifi_config_t cfg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cfg = s_trial;
    xSemaphoreGive(s_lock);

    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);     /* written to flash; put back on failure */
    s_trying = true;
    s_trial_fails = 0;
    ESP_LOGI(TAG, "Trying %s", (const char *)cfg.sta.ssid);
    esp_wifi_connect();
}

static void end_trial_failed(uint8_t reason)
{
    s_trying = false;
    set_status(STATUS_FAILED, NULL, reason_text(reason));
    ESP_LOGW(TAG, "Setup network attempt failed: %s (reason %u)", reason_text(reason), reason);
    /* Put the previous network back, in flash as well as in memory */
    esp_wifi_set_config(WIFI_IF_STA, &s_saved);
    schedule_retry();
}

static void on_got_ip(void)
{
    s_connected = true;
    s_fails = 0;
    esp_timer_stop(s_retry_timer);

    wifi_config_t cfg = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (s_trying) {
        /* It works: it is already in flash, so just remember it as the one to fall back to */
        s_trying = false;
        s_saved = cfg;
        s_have_saved = true;
        ESP_LOGI(TAG, "Saved %s", (const char *)cfg.sta.ssid);
    }
    set_status(STATUS_CONNECTED, (const char *)cfg.sta.ssid, NULL);
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
    ESP_LOGI(TAG, "Online on %s at " IPSTR, (const char *)cfg.sta.ssid, IP2STR(&ip.ip));

    /* Where to find the settings page, on screen for a moment */
    char address[48];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
    app_dashboard_notice(address, 30);

    if (s_setup_open) {
        esp_timer_stop(s_close_timer);
        esp_timer_start_once(s_close_timer, SETUP_CLOSE_MS * 1000ULL);
    }
    if (!s_sntp_started) {
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&sntp) == ESP_OK) {
            s_sntp_started = true;
        }
    }
}

static void on_disconnected(uint8_t reason)
{
    bool was_connected = s_connected;
    s_connected = false;
    if (reason == WIFI_REASON_ASSOC_LEAVE) {
        return;     /* our own esp_wifi_disconnect() */
    }
    if (s_trying) {
        bool password = reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                        reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
        if (password || ++s_trial_fails >= TRIAL_ATTEMPTS) {
            end_trial_failed(reason);
        } else {
            esp_wifi_connect();
        }
        return;
    }
    s_fails++;
    ESP_LOGW(TAG, "%s (reason %u), try %d", was_connected ? "Connection lost" : "Couldn't connect", reason, s_fails);
    if (s_fails >= SETUP_AFTER_FAILS && !s_setup_open) {
        open_setup();
    }
    schedule_retry();
}

static void wifi_task(void *arg)
{
    (void)arg;
    cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd.type) {
            case CMD_START:
                if (s_have_saved) {
                    connect_saved();
                } else {
                    open_setup();
                }
                break;
            case CMD_DISCONNECTED:
                on_disconnected(cmd.reason);
                break;
            case CMD_GOT_IP:
                on_got_ip();
                break;
            case CMD_RETRY:
                connect_saved();
                break;
            case CMD_TRY_NETWORK:
                vTaskDelay(pdMS_TO_TICKS(500));     /* let the page's response reach the phone first */
                start_trial();
                break;
            case CMD_RESCAN:
                if (!s_trying) {
                    esp_timer_stop(s_retry_timer);
                    if (!s_connected) {
                        esp_wifi_disconnect();      /* a connect in progress would make the scan fail */
                    }
                    scan_networks();
                    if (!s_connected) {
                        schedule_retry();
                    }
                }
                xEventGroupSetBits(s_events, SCAN_DONE_BIT);
                break;
            case CMD_FORGET: {
                ESP_LOGW(TAG, "Forgetting the saved network");
                esp_timer_stop(s_retry_timer);
                s_trying = false;
                s_have_saved = false;
                esp_wifi_disconnect();
                memset(&s_saved, 0, sizeof(s_saved));
                esp_wifi_set_config(WIFI_IF_STA, &s_saved);
                open_setup();
                break;
            }
            case CMD_CLOSE_SETUP:
                if (s_connected) {
                    close_setup();
                }
                break;
        }
    }
}

/* ------------------------------------------------------------------------------------------------
 * Events and public API
 * ---------------------------------------------------------------------------------------------- */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        post(CMD_DISCONNECTED, ((wifi_event_sta_disconnected_t *)data)->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        post(CMD_GOT_IP, 0);
    }
}

static void retry_timer_cb(void *arg)
{
    post(CMD_RETRY, 0);
}

static void close_timer_cb(void *arg)
{
    post(CMD_CLOSE_SETUP, 0);
}

/* "vocat wifi forget" on the USB control port does what the 10 s hold on the clock page does */
static void usb_command(const char *line)
{
    if (strcmp(line, "vocat wifi forget") == 0) {
        wifi_setup_forget();
    }
}

esp_err_t wifi_setup_start(void)
{
    ESP_RETURN_ON_FALSE(s_queue == NULL, ESP_ERR_INVALID_STATE, TAG, "Already started");
    s_queue = xQueueCreate(8, sizeof(cmd_t));
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_queue && s_lock && s_events, ESP_ERR_NO_MEM, TAG, "Out of memory");

    const esp_timer_create_args_t retry_args = { .callback = retry_timer_cb, .name = "wifi_retry" };
    const esp_timer_create_args_t close_args = { .callback = close_timer_cb, .name = "wifi_setup_close" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "Retry timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&close_args, &s_close_timer), TAG, "Close timer");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event_handler, NULL), TAG, "event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL), TAG, "event");

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_setup_ssid, sizeof(s_setup_ssid), SETUP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    if (esp_wifi_get_config(WIFI_IF_STA, &s_saved) == ESP_OK && s_saved.sta.ssid[0]) {
        s_have_saved = true;
        ESP_LOGI(TAG, "Saved network: %s", (const char *)s_saved.sta.ssid);
    } else {
        ESP_LOGI(TAG, "No saved network");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    ESP_RETURN_ON_FALSE(xTaskCreate(wifi_task, "wifi_setup", 6144, NULL, 4, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
        static const httpd_uri_t wifi = { .uri = "/wifi", .method = HTTP_GET, .handler = list_get };
        static const httpd_uri_t join = { .uri = "/join", .method = HTTP_GET, .handler = join_get };
        static const httpd_uri_t connect = { .uri = "/connect", .method = HTTP_POST, .handler = connect_post };
        static const httpd_uri_t result = { .uri = "/result", .method = HTTP_GET, .handler = result_get };
        static const httpd_uri_t settings_page = { .uri = "/settings", .method = HTTP_GET, .handler = settings_get };
        static const httpd_uri_t settings = { .uri = "/settings", .method = HTTP_POST, .handler = settings_post };
        static const httpd_uri_t off = { .uri = "/off", .method = HTTP_POST, .handler = off_post };
        httpd_register_uri_handler(s_httpd, &root);
        httpd_register_uri_handler(s_httpd, &wifi);
        httpd_register_uri_handler(s_httpd, &join);
        httpd_register_uri_handler(s_httpd, &connect);
        httpd_register_uri_handler(s_httpd, &result);
        httpd_register_uri_handler(s_httpd, &settings_page);
        httpd_register_uri_handler(s_httpd, &settings);
        httpd_register_uri_handler(s_httpd, &off);
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);
    } else {
        ESP_LOGE(TAG, "Web server didn't start");
    }

    app_usb_hid_add_command_cb(usb_command);
    post(CMD_START, 0);
    return ESP_OK;
}

void wifi_setup_forget(void)
{
    if (s_queue) {
        post(CMD_FORGET, 0);
    }
}
