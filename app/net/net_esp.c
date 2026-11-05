// net_esp.c — device backend: Wi-Fi station, esp_http_client with keep-alive and streamed bodies,
// esp_websocket_client with message reassembly. Same net.h surface as the host.
#define LOG_TAG "net"
#include "net/net.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

// ---------------------------------------------------------------- Wi-Fi ----
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static atomic_bool s_online;
static int s_retries;

// Wall-clock time: SNTP starts on the first IP and re-syncs on lwIP's own schedule afterwards. The host
// name must outlive the call (lwIP keeps the pointer), hence the static copy.
static char s_ntp_host[64];
static bool s_sntp_started;
static void sntp_synced(struct timeval *tv) { LOGI("time synced (%lld)", (long long)tv->tv_sec); }
static void start_sntp(void)
{
    if (s_sntp_started || !s_ntp_host[0]) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_host);
    cfg.sync_cb = sntp_synced;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) s_sntp_started = true;
}
esp_err_t net_time_sync(const char *ntp_host)
{
    if (!ntp_host || !*ntp_host) return ESP_ERR_INVALID_ARG;
    strlcpy(s_ntp_host, ntp_host, sizeof s_ntp_host);
    if (net_is_online()) start_sntp();
    return ESP_OK;
}

static esp_timer_handle_t s_retry_timer;
static void retry_cb(void *arg) { (void)arg; esp_wifi_connect(); }

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&s_online, false);
        if (s_retries++ < 5) { esp_wifi_connect(); LOGW("wifi retry %d", s_retries); }
        else {                                   // five quick tries failed: tell the waiter, then keep trying every 30 s (a router that reboots comes back)
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            if (!s_retry_timer) { const esp_timer_create_args_t a = { .callback = retry_cb, .name = "gugu_net_retry" }; esp_timer_create(&a, &s_retry_timer); }
            if (s_retry_timer && !esp_timer_is_active(s_retry_timer)) esp_timer_start_once(s_retry_timer, 30ULL * 1000000ULL);
            if (s_retries == 6) LOGW("wifi: still offline; retrying every 30 s");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        LOGI("wifi got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        atomic_store(&s_online, true);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        start_sntp();
    }
}

esp_err_t net_init(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);          // latency over power during calls (sleep policy comes later)
    LOGI("device net ready");
    return ESP_OK;
}

bool net_is_online(void) { return atomic_load(&s_online); }

esp_err_t net_wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass ? pass : "");
    wc.sta.threshold.authmode = pass && *pass ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    s_retries = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    int64_t t0 = clock_us();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    LOGI("wifi %s: %s in %lld ms", ssid, (bits & WIFI_CONNECTED_BIT) ? "connected" : "failed", (long long)((clock_us() - t0) / 1000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t net_wifi_join(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass ? pass : "");
    wc.sta.threshold.authmode = pass && *pass ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    s_retries = 0;
    esp_wifi_start();                                    // a no-op when running (pairing mode keeps it running)
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_connect();
    LOGI("wifi join %s: %s", ssid, esp_err_to_name(err));
    return err;
}

int net_wifi_scan(net_wifi_ap_t *out, int cap)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) return 0;
    esp_wifi_start();                                   // harmless when already started; needed when STA never connected
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) { LOGW("wifi scan: %s", esp_err_to_name(err)); return 0; }
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof *recs);
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&n, recs);
    int count = 0;
    for (int i = 0; i < n && count < cap; i++) {        // the records come strongest first; keep one entry per name
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < count; k++) if (!strcmp(out[k].ssid, (const char *)recs[i].ssid)) dup = true;
        if (dup) continue;
        snprintf(out[count].ssid, sizeof out[count].ssid, "%s", (const char *)recs[i].ssid);
        out[count].rssi = recs[i].rssi; out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    free(recs);
    LOGI("wifi scan: %d networks", count);
    return count;
}

// ----------------------------------------------------------------- HTTP ----
struct net_http {
    esp_http_client_handle_t h;
    char base[160];
    atomic_bool cancel;
    atomic_int status;
    bool alive;          // the previous request (or pre-warm) left a keep-alive connection open in the client
    void (*stream)(const uint8_t *chunk, size_t n, void *user); void *stream_user;   // the request in flight wants its body as it comes
};

// Nagle off. Our writes are small and each one is wanted at the other end now: esp_http_client sends the headers, then
// the body; esp_websocket_client sends every frame as two TLS records, header then payload. With Nagle on (lwIP's
// default) a short segment waits until the server has acknowledged the one before, which a server delays up to its
// delayed-ACK timer, and an upload stalls once for every 4 KB TLS record (its last, short segment): a 100 KB recording
// went out at ~200 KB/s on a link that carries several times that (2026-11-05 latency bench).
static void nodelay(int fd) { int one = 1; if (fd >= 0) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
static void nodelay_all(void)   // esp_websocket_client keeps its socket to itself: every TCP socket lwIP has (closed or UDP ones refuse)
{
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) nodelay(fd);
}

// A streamed body (the model's SSE) is handed on per TLS read, as the parser sees it. esp_http_client_read() returns only
// when the whole buffer it was given is full (or the body ends), so reading the stream through it handed the first
// words over in 2 KB batches: the first sentence reached the voice when 2 KB of events had arrived, ~0.4 s late.
static esp_err_t http_event(esp_http_client_event_t *e)
{
    struct net_http *c = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && c && c->stream && !atomic_load(&c->cancel) && esp_http_client_get_status_code(e->client) == 200)
        c->stream((const uint8_t *)e->data, (size_t)e->data_len, c->stream_user);
    return ESP_OK;
}

esp_err_t net_http_open(const char *base_url, net_http_t **out)
{
    struct net_http *c = calloc(1, sizeof *c);
    if (!c) return ESP_ERR_NO_MEM;
    snprintf(c->base, sizeof c->base, "%s", base_url ? base_url : "");
    esp_http_client_config_t cfg = {
        .url = c->base[0] ? c->base : "https://localhost/",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = true,
        .event_handler = http_event,
        .user_data = c,
    };
    c->h = esp_http_client_init(&cfg);
    if (!c->h) { free(c); return ESP_FAIL; }
    *out = c;
    return ESP_OK;
}

esp_err_t net_http_prewarm(net_http_t *c, uint32_t timeout_ms)
{
    if (!c->base[0]) return ESP_ERR_INVALID_STATE;
    if (!net_is_online()) return ESP_ERR_INVALID_STATE;
    esp_http_client_set_url(c->h, c->base);
    esp_http_client_set_method(c->h, HTTP_METHOD_HEAD);
    esp_http_client_set_timeout_ms(c->h, (int)timeout_ms);
    int64_t t0 = clock_us();
    esp_err_t err = esp_http_client_perform(c->h);          // keep-alive leaves the TLS session open
    c->alive = err == ESP_OK;
    LOGI("prewarm %s: %s (%d) in %lld ms", c->base, esp_err_to_name(err), esp_http_client_get_status_code(c->h), (long long)((clock_us() - t0) / 1000));
    return err;
}

int net_http_current_status(net_http_t *c) { return atomic_load(&c->status); }

esp_err_t net_http_perform(net_http_t *c, const net_http_request_t *req, net_http_response_t *resp)
{
    memset(resp, 0, sizeof *resp);
    atomic_store(&c->cancel, false); atomic_store(&c->status, 0);
    char url[320];
    if (!strncmp(req->url, "http", 4)) snprintf(url, sizeof url, "%s", req->url);
    else snprintf(url, sizeof url, "%s%s", c->base, req->url);
    esp_http_client_handle_t h = c->h;
    esp_http_client_set_url(h, url);
    esp_http_client_set_method(h, req->method && !strcmp(req->method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_timeout_ms(h, (int)(req->timeout_ms ? req->timeout_ms : 15000));
    for (size_t i = 0; i < req->header_count; i++) esp_http_client_set_header(h, req->headers[i].name, req->headers[i].value);

    // The connection from the previous request stays open (esp_http_client_close is only called on failure
    // below), so a second request to the same host skips the TLS handshake. A keep-alive connection the server
    // has meanwhile dropped shows up as a failed send or an empty/invalid response; that case is retried once
    // on a fresh connection, which is possible whenever the request body is in memory (not a body_read stream).
    int64_t t0 = clock_us();
    c->stream = req->on_body; c->stream_user = req->user;    // http_event hands a 200 body over as it arrives
    size_t body_len = req->body_read ? req->body_total : req->body_len;
    bool reused = c->alive, retried = false;
    esp_err_t err;
retry:
    err = esp_http_client_open(h, (int)body_len);
    if (err != ESP_OK) {
        if (reused && !retried) { LOGW("keep-alive connection stale (open), reconnecting"); esp_http_client_close(h); c->alive = false; reused = false; retried = true; goto retry; }
        LOGW("open %s: %s", url, esp_err_to_name(err)); goto done;
    }
    resp->t_connect_us = clock_us() - t0;
    resp->reused_connection = reused;
    nodelay(esp_http_client_get_socket(h));
    if (req->body_read) {
        uint8_t *buf = mem_alloc_dma(2048); size_t n;
        while ((n = req->body_read(buf, 2048, req->user)) > 0) {
            if (esp_http_client_write(h, (const char *)buf, (int)n) < 0 || atomic_load(&c->cancel)) { err = ESP_FAIL; break; }
        }
        mem_release(buf);
    } else if (req->body && req->body_len) {
        const uint8_t *p = req->body; size_t left = req->body_len;
        while (left) {
            size_t k = left > 4096 ? 4096 : left;
            int w = esp_http_client_write(h, (const char *)p, (int)k);
            if (w < 0 || atomic_load(&c->cancel)) { err = ESP_FAIL; break; }
            p += w; left -= (size_t)w;
        }
    }
    if (err != ESP_OK) goto close;
    resp->t_sent_us = clock_us() - t0;
    int64_t content_len = esp_http_client_fetch_headers(h);
    int status = content_len < 0 ? 0 : esp_http_client_get_status_code(h);
    if (content_len < 0 || status == 0) {
        if (reused && !retried && !req->body_read) { LOGW("keep-alive connection stale (no response), reconnecting"); esp_http_client_close(h); c->alive = false; reused = false; retried = true; goto retry; }
        err = ESP_FAIL; goto close;
    }
    atomic_store(&c->status, status);
    resp->status = status;
    resp->t_first_byte_us = clock_us() - t0;
    strbuf_t buf; strbuf_init(&buf);
    uint8_t *chunk = mem_alloc_big(2048);
    for (;;) {
        if (atomic_load(&c->cancel)) { err = ESP_ERR_NOT_FINISHED; break; }
        int r = esp_http_client_read(h, (char *)chunk, 2048);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) { if (esp_http_client_is_complete_data_received(h) || content_len == 0) break; else continue; }
        if (!(req->on_body && status == 200)) strbuf_append(&buf, (const char *)chunk, (size_t)r);   // a stream went out through http_event already
    }
    mem_release(chunk);
    resp->body = buf.s; resp->body_len = buf.len;
close:
    if (err == ESP_OK) c->alive = true;                       // keep-alive: the connection stays in the client for the next perform
    else { esp_http_client_close(h); c->alive = false; }      // a failed or cancelled exchange leaves the stream in an unknown state
done:
    c->stream = NULL; c->stream_user = NULL;
    for (size_t i = 0; i < req->header_count; i++) esp_http_client_delete_header(h, req->headers[i].name);
    resp->t_done_us = clock_us() - t0;
    return err;
}

void net_http_cancel(net_http_t *c) { atomic_store(&c->cancel, true); }
void net_http_close(net_http_t *c) { if (!c) return; esp_http_client_cleanup(c->h); free(c); }

// ------------------------------------------------------------ WebSocket ----
struct net_ws {
    esp_websocket_client_handle_t h;
    net_ws_events_t ev; void *user;
    atomic_bool connected;
    uint8_t *msg; size_t msg_len, msg_cap, max_message; int op;
    size_t frame_base;         // where the current frame's bytes start inside msg
    bool dropping;             // the current message outgrew max_message: skip its remaining frames
    EventGroupHandle_t eg;
};

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct net_ws *w = arg;
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        atomic_store(&w->connected, true); xEventGroupSetBits(w->eg, BIT0);
        if (w->ev.on_connected) w->ev.on_connected(w->user);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (atomic_exchange(&w->connected, false) && w->ev.on_closed) w->ev.on_closed(1006, "", w->user);
        xEventGroupSetBits(w->eg, BIT1);
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (w->ev.on_error) w->ev.on_error("websocket error", w->user);
        xEventGroupSetBits(w->eg, BIT1);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x8) { if (atomic_exchange(&w->connected, false) && w->ev.on_closed) w->ev.on_closed(1000, "", w->user); break; }
        if (e->op_code == 0x9 || e->op_code == 0xA) break;      // ping / pong handled by the client
        // A message is one or more frames: the first carries opcode 1 (text) or 2 (binary), the rest opcode 0
        // (continuation), the last one FIN. A frame may itself arrive in several events (payload_offset). The
        // vendor's TTS server splits every audio message into a 4096-byte frame plus a continuation, so joining
        // only within a frame handed the voice on in halves that the JSON scan threw away (踩坑 27).
        if (e->payload_offset == 0) {
            if (e->op_code == 0x1 || e->op_code == 0x2) { w->op = e->op_code; w->msg_len = 0; w->dropping = false; }
            else if (e->op_code != 0x0) break;
            w->frame_base = w->msg_len;
        }
        if (w->dropping || !w->op) break;
        {
            size_t need = w->frame_base + (size_t)e->payload_len;
            if (need > w->max_message) { w->dropping = true; if (w->ev.on_error) w->ev.on_error("message too large", w->user); break; }
            if (w->msg_cap < need + 1) {
                size_t ncap = need + 1 > w->msg_cap * 2 ? need + 1 : w->msg_cap * 2;
                uint8_t *nb = mem_alloc_big(ncap);
                if (!nb) { w->dropping = true; break; }
                if (w->msg) { memcpy(nb, w->msg, w->msg_len); mem_release(w->msg); }
                w->msg = nb; w->msg_cap = ncap;
            }
            memcpy(w->msg + w->frame_base + (size_t)e->payload_offset, e->data_ptr, (size_t)e->data_len);
            size_t frame_got = (size_t)e->payload_offset + (size_t)e->data_len;
            w->msg_len = w->frame_base + frame_got;
            if (frame_got >= (size_t)e->payload_len && e->fin) {        // the last frame is complete: the message is whole
                if (w->op == 0x1 && w->ev.on_text) { w->msg[w->msg_len] = '\0'; w->ev.on_text((const char *)w->msg, w->msg_len, w->user); }
                else if (w->op == 0x2 && w->ev.on_binary) w->ev.on_binary(w->msg, w->msg_len, w->user);
                w->op = 0;
            }
        }
        break;
    default:
        break;
    }
}

esp_err_t net_ws_open(const net_ws_params_t *p, const net_ws_events_t *ev, void *user, net_ws_t **out)
{
    if (!net_is_online()) return ESP_ERR_INVALID_STATE;
    struct net_ws *w = calloc(1, sizeof *w);
    if (!w) return ESP_ERR_NO_MEM;
    w->ev = *ev; w->user = user; w->max_message = p->max_message ? p->max_message : (1u << 20);
    w->eg = xEventGroupCreate();
    strbuf_t hdrs; strbuf_init(&hdrs);
    for (size_t i = 0; i < p->header_count; i++) strbuf_printf(&hdrs, "%s: %s\r\n", p->headers[i].name, p->headers[i].value);
    esp_websocket_client_config_t cfg = {
        .uri = p->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .headers = hdrs.s,
        .buffer_size = 8192,
        .task_stack = 8192,
        .network_timeout_ms = (int)(p->connect_timeout_ms ? p->connect_timeout_ms : 8000),
        .reconnect_timeout_ms = 0,
        .disable_auto_reconnect = true,
        .ping_interval_sec = 20,
    };
    int64_t t0 = clock_us();
    w->h = esp_websocket_client_init(&cfg);
    if (!w->h) { strbuf_free(&hdrs); free(w); return ESP_FAIL; }
    esp_websocket_register_events(w->h, WEBSOCKET_EVENT_ANY, ws_event, w);
    esp_err_t err = esp_websocket_client_start(w->h);
    strbuf_free(&hdrs);
    if (err != ESP_OK) { esp_websocket_client_destroy(w->h); free(w); return err; }
    EventBits_t bits = xEventGroupWaitBits(w->eg, BIT0 | BIT1, pdFALSE, pdFALSE, pdMS_TO_TICKS(p->connect_timeout_ms ? p->connect_timeout_ms : 8000));
    if (!(bits & BIT0)) { LOGE("ws connect failed: %s", p->url); net_ws_close(w); return ESP_ERR_TIMEOUT; }
    nodelay_all();
    LOGI("ws connected %s in %lld ms", p->url, (long long)((clock_us() - t0) / 1000));
    *out = w;
    return ESP_OK;
}

esp_err_t net_ws_send_text(net_ws_t *w, const char *utf8, size_t n)
{
    int r = esp_websocket_client_send_text(w->h, utf8, (int)n, pdMS_TO_TICKS(5000));
    return r >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t net_ws_send_binary(net_ws_t *w, const uint8_t *data, size_t n)
{
    int r = esp_websocket_client_send_bin(w->h, (const char *)data, (int)n, pdMS_TO_TICKS(5000));
    return r >= 0 ? ESP_OK : ESP_FAIL;
}

bool net_ws_is_connected(net_ws_t *w) { return w && atomic_load(&w->connected); }

void net_ws_close(net_ws_t *w)
{
    if (!w) return;
    if (atomic_load(&w->connected)) esp_websocket_client_close(w->h, pdMS_TO_TICKS(1000));
    esp_websocket_client_stop(w->h);               // joins the client task; destroying a running client crashes
    esp_websocket_client_destroy(w->h);
    vEventGroupDelete(w->eg);
    mem_release(w->msg);
    free(w);
}
