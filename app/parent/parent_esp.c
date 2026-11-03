// parent_esp.c — esp_http_server on the station interface (and the hotspot when entered).
#define LOG_TAG "parent"
#include "parent_internal.h"
#include "foundation/log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_check.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static httpd_handle_t s_httpd;
static bool s_active;
static char s_ssid[32], s_pass[12], s_url[40];
static esp_netif_t *s_ap_netif;             // created once; enter/exit only switch the Wi-Fi mode

static esp_err_t any_handler(httpd_req_t *req)
{
    char *body = NULL; size_t blen = req->content_len;
    if (blen > 0 && blen < 16384) {
        body = malloc(blen + 1);
        size_t got = 0;
        while (got < blen) { int r = httpd_req_recv(req, body + got, blen - got); if (r <= 0) break; got += (size_t)r; }
        body[got] = '\0'; blen = got;
    }
    char path[128], query[256] = "";
    const char *q = strchr(req->uri, '?');
    snprintf(path, sizeof path, "%.*s", (int)(q ? q - req->uri : (long)strlen(req->uri)), req->uri);
    if (q) snprintf(query, sizeof query, "%s", q + 1);
    char *out = NULL; size_t out_len = 0; const char *ctype, *disp;
    int status = parent_api_handle(req->method == HTTP_POST ? "POST" : "GET", path, query, body, blen, &out, &out_len, &ctype, &disp);
    free(body);
    char st[16]; snprintf(st, sizeof st, "%d", status);
    httpd_resp_set_status(req, status == 200 ? "200 OK" : status == 404 ? "404 Not Found" : "400 Bad Request");
    httpd_resp_set_type(req, ctype);
    if (disp) httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out ? out : "", (ssize_t)out_len);
    free(out);
    return err;
}

esp_err_t parent_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;
    httpd_uri_t get = { .uri = "/*", .method = HTTP_GET, .handler = any_handler };
    httpd_uri_t post = { .uri = "/*", .method = HTTP_POST, .handler = any_handler };
    httpd_register_uri_handler(s_httpd, &get);
    httpd_register_uri_handler(s_httpd, &post);
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof s_ssid, "gugu-%02X%02X", mac[4], mac[5]);
    LOGI("parent web ready on port %d (hotspot name %s)", cfg.server_port, s_ssid);
    return ESP_OK;
}

esp_err_t parent_enter(void)
{
    uint32_t r = esp_random();
    snprintf(s_pass, sizeof s_pass, "%08u", (unsigned)(r % 100000000u));
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "%s", s_ssid);
    snprintf((char *)ap.ap.password, sizeof ap.ap.password, "%s", s_pass);
    ap.ap.ssid_len = strlen(s_ssid); ap.ap.max_connection = 2; ap.ap.authmode = WIFI_AUTH_WPA2_PSK; ap.ap.channel = 6;
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), LOG_TAG, "apsta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), LOG_TAG, "ap cfg");
    esp_wifi_start();
    esp_netif_ip_info_t ip;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) snprintf(s_url, sizeof s_url, "http://" IPSTR, IP2STR(&ip.ip));
    else snprintf(s_url, sizeof s_url, "http://192.168.4.1");
    s_active = true;
    LOGI("pairing mode: hotspot %s up (password shown on the device only)", s_ssid);
    return ESP_OK;
}

void parent_exit(void)
{
    if (!s_active) return;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_active = false;
    s_pass[0] = '\0';
    LOGI("hotspot down");
}

bool parent_is_active(void) { return s_active; }
const char *parent_hotspot_ssid(void) { return s_ssid; }
const char *parent_hotspot_password(void) { return s_pass; }
const char *parent_hotspot_url(void) { return s_url; }
