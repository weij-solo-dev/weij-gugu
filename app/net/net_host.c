// net_host.c — host backend: libcurl for HTTP (keep-alive per client, streamed bodies both ways),
// a small RFC 6455 WebSocket client over OpenSSL for wss://. Same net.h surface as the device.
#define LOG_TAG "net"
#include "net/net.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

esp_err_t net_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    LOGI("host net: %s", curl_version());
    return ESP_OK;
}
bool net_is_online(void) { return true; }
esp_err_t net_wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms) { (void)ssid; (void)pass; (void)timeout_ms; return ESP_OK; }
esp_err_t net_time_sync(const char *ntp_host) { (void)ntp_host; return ESP_OK; }

// ---------------------------------------------------------------- HTTP ----
struct net_http {
    CURL *curl;
    char base[160];
    atomic_bool cancel;
    atomic_int status;
    const net_http_request_t *req;
    strbuf_t buf;              // buffered body when req->on_body == NULL
    bool first_byte_seen;
    int64_t t0;
    int64_t t_first;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct net_http *c = userdata;
    size_t n = size * nmemb;
    long code = 0;
    curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &code);
    atomic_store(&c->status, (int)code);
    if (!c->first_byte_seen) { c->first_byte_seen = true; c->t_first = clock_us(); }
    if (atomic_load(&c->cancel)) return 0;      // abort the transfer
    if (c->req->on_body && code == 200) c->req->on_body((const uint8_t *)ptr, n, c->req->user);
    else strbuf_append(&c->buf, ptr, n);        // error bodies are always buffered for the caller
    return n;
}

static size_t read_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    struct net_http *c = userdata;
    if (atomic_load(&c->cancel)) return CURL_READFUNC_ABORT;
    return c->req->body_read ? c->req->body_read((uint8_t *)buffer, size * nitems, c->req->user) : 0;
}

static int progress_cb(void *clientp, curl_off_t a, curl_off_t b, curl_off_t cc, curl_off_t d)
{
    (void)a; (void)b; (void)cc; (void)d;
    return atomic_load(&((struct net_http *)clientp)->cancel) ? 1 : 0;
}

esp_err_t net_http_open(const char *base_url, net_http_t **out)
{
    struct net_http *c = calloc(1, sizeof *c);
    if (!c) return ESP_ERR_NO_MEM;
    c->curl = curl_easy_init();
    if (!c->curl) { free(c); return ESP_FAIL; }
    snprintf(c->base, sizeof c->base, "%s", base_url ? base_url : "");
    strbuf_init(&c->buf);
    curl_easy_setopt(c->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c->curl, CURLOPT_ACCEPT_ENCODING, "");
    *out = c;
    return ESP_OK;
}

esp_err_t net_http_prewarm(net_http_t *c, uint32_t timeout_ms)
{
    if (!c->base[0]) return ESP_ERR_INVALID_STATE;
    // A HEAD to the endpoint: completes TCP + TLS and leaves a keep-alive connection in the handle's
    // pool (a CONNECT_ONLY transfer would not be reused by a later normal transfer).
    curl_easy_setopt(c->curl, CURLOPT_URL, c->base);
    curl_easy_setopt(c->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c->curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(c->curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    int64_t t0 = clock_us();
    CURLcode rc = curl_easy_perform(c->curl);
    curl_easy_setopt(c->curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(c->curl, CURLOPT_HTTPGET, 1L);
    LOGI("prewarm %s: %s in %lld ms", c->base, rc == CURLE_OK ? "ok" : curl_easy_strerror(rc), (long long)((clock_us() - t0) / 1000));
    return rc == CURLE_OK ? ESP_OK : ESP_FAIL;
}

int net_http_current_status(net_http_t *c) { return atomic_load(&c->status); }

esp_err_t net_http_perform(net_http_t *c, const net_http_request_t *req, net_http_response_t *resp)
{
    memset(resp, 0, sizeof *resp);
    c->req = req; c->first_byte_seen = false; c->t_first = 0;
    atomic_store(&c->cancel, false); atomic_store(&c->status, 0);
    strbuf_clear(&c->buf);
    char url[320];
    if (!strncmp(req->url, "http", 4)) snprintf(url, sizeof url, "%s", req->url);
    else snprintf(url, sizeof url, "%s%s", c->base, req->url);

    CURL *h = c->curl;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, c);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    if (req->on_body) {   // streaming: guard against stalls instead of a whole-request timeout
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 0L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, (long)((req->timeout_ms ? req->timeout_ms : 15000) / 1000 + 1));
    } else {
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)(req->timeout_ms ? req->timeout_ms : 20000));
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 0L);
    }
    struct curl_slist *hdrs = NULL;
    for (size_t i = 0; i < req->header_count; i++) {
        char line[512];
        snprintf(line, sizeof line, "%s: %s", req->headers[i].name, req->headers[i].value);
        hdrs = curl_slist_append(hdrs, line);
    }
    hdrs = curl_slist_append(hdrs, "Expect:");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    if (req->method && !strcmp(req->method, "POST")) {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        if (req->body_read) {
            curl_easy_setopt(h, CURLOPT_READFUNCTION, read_cb);
            curl_easy_setopt(h, CURLOPT_READDATA, c);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, NULL);
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_total);
        } else {
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, req->body ? (const char *)req->body : "");
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
        }
    } else {
        curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    }
    c->t0 = clock_us();
    CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(hdrs);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(h, CURLOPT_READFUNCTION, NULL);

    long code = 0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    resp->status = (int)code;
    curl_off_t app = 0, start = 0, total = 0; long nconn = 0;
    curl_easy_getinfo(h, CURLINFO_APPCONNECT_TIME_T, &app);
    curl_easy_getinfo(h, CURLINFO_STARTTRANSFER_TIME_T, &start);
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME_T, &total);
    curl_easy_getinfo(h, CURLINFO_NUM_CONNECTS, &nconn);
    resp->t_connect_us = app; resp->t_first_byte_us = start; resp->t_done_us = total;
    resp->reused_connection = nconn == 0;
    if (!req->on_body || code != 200) {
        resp->body = c->buf.s; resp->body_len = c->buf.len;
        strbuf_init(&c->buf);     // ownership moves to the caller
    }
    if (rc == CURLE_ABORTED_BY_CALLBACK || rc == CURLE_WRITE_ERROR) return ESP_ERR_NOT_FINISHED;   // cancelled
    if (rc == CURLE_OPERATION_TIMEDOUT) return ESP_ERR_TIMEOUT;
    if (rc != CURLE_OK) { LOGW("curl: %s (%s)", curl_easy_strerror(rc), url); return ESP_FAIL; }
    return ESP_OK;
}

void net_http_cancel(net_http_t *c) { atomic_store(&c->cancel, true); }

void net_http_close(net_http_t *c)
{
    if (!c) return;
    curl_easy_cleanup(c->curl);
    strbuf_free(&c->buf);
    free(c);
}

// ------------------------------------------------------------ WebSocket ----
// (implemented in net_host_ws.c; declared here so the file set stays flat)
