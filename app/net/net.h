// net.h — the two transports vendors need: a streaming HTTP client with keep-alive (SSE and JSON
// bodies) and a WebSocket client, both over TLS with session reuse and a pre-warm hook. The device
// backend wraps esp_http_client / esp_websocket_client; the host backend uses libcurl + OpenSSL.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t net_init(void);
bool net_is_online(void);                        // Wi-Fi up with an IP (host: always true)
esp_err_t net_wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms);   // device; host no-op
esp_err_t net_time_sync(const char *ntp_host);   // SNTP against this host whenever Wi-Fi is up (device; host no-op)
typedef struct { char ssid[33]; int rssi; bool secure; } net_wifi_ap_t;
int net_wifi_scan(net_wifi_ap_t *out, int cap);     // blocking (~2 s): nearby networks, strongest first, no duplicates; host: a made-up list
esp_err_t net_wifi_join(const char *ssid, const char *pass);   // join now without stopping the radio (the hotspot stays up); net_is_online() tells the result

// ---- HTTP ----
typedef struct net_http net_http_t;
typedef struct {
    const char *name;
    const char *value;
} net_header_t;
typedef struct {
    const char *method;          // "GET" | "POST"
    const char *url;
    const net_header_t *headers; size_t header_count;
    const uint8_t *body; size_t body_len;       // for a single-buffer body
    // Streaming request body (multipart uploads): called repeatedly until it returns 0.
    size_t (*body_read)(uint8_t *buf, size_t cap, void *user);
    size_t body_total;           // Content-Length when body_read is used
    uint32_t timeout_ms;         // whole-request deadline for non-streaming; first-byte for streaming
    // Response streaming: called per chunk as bytes arrive. If NULL, the body is buffered in resp_buf.
    void (*on_body)(const uint8_t *chunk, size_t n, void *user);
    void *user;
} net_http_request_t;
typedef struct {
    int status;
    char *body;                  // buffered body (NUL-terminated) when on_body is NULL; caller mem_release()s
    size_t body_len;
    int64_t t_connect_us, t_first_byte_us, t_done_us;   // relative to the request start
    int64_t t_sent_us;           // the whole request body was handed to the network (device: into the socket's send buffer); 0: not known (host)
    bool reused_connection;      // served on the keep-alive connection left by the previous request / pre-warm
} net_http_response_t;

// A client keeps one connection to one host alive across requests (open it while the line opens).
esp_err_t net_http_open(const char *base_url, net_http_t **out);
esp_err_t net_http_prewarm(net_http_t *c, uint32_t timeout_ms);        // TLS handshake now, nothing sent
esp_err_t net_http_perform(net_http_t *c, const net_http_request_t *req, net_http_response_t *resp);
void      net_http_cancel(net_http_t *c);                              // abort an in-flight streaming request
int       net_http_current_status(net_http_t *c);                      // status seen so far during a streaming perform (0 = none)
void      net_http_close(net_http_t *c);

// ---- WebSocket ----
typedef struct net_ws net_ws_t;
typedef struct {
    void (*on_connected)(void *user);
    void (*on_text)(const char *utf8, size_t n, void *user);          // one complete message (fragments joined)
    void (*on_binary)(const uint8_t *data, size_t n, void *user);
    void (*on_closed)(int code, const char *reason, void *user);
    void (*on_error)(const char *what, void *user);
} net_ws_events_t;
typedef struct {
    const char *url;             // wss://host/path
    const net_header_t *headers; size_t header_count;
    uint32_t connect_timeout_ms;
    size_t max_message;          // largest message we accept (bytes); bigger ones are dropped with on_error
} net_ws_params_t;

esp_err_t net_ws_open(const net_ws_params_t *p, const net_ws_events_t *ev, void *user, net_ws_t **out);
esp_err_t net_ws_send_text(net_ws_t *w, const char *utf8, size_t n);
esp_err_t net_ws_send_binary(net_ws_t *w, const uint8_t *data, size_t n);
bool      net_ws_is_connected(net_ws_t *w);
void      net_ws_close(net_ws_t *w);
