// net_host_ws.c — minimal WebSocket client (RFC 6455) over OpenSSL for the host build: text and
// binary frames, fragmentation, masking, ping/pong, close. One reader thread per connection.
#define LOG_TAG "net"
#include "net/net.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/hexb64.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <errno.h>

struct net_ws {
    SSL_CTX *ctx; SSL *ssl; int fd;
    net_ws_events_t ev; void *user;
    pthread_t reader; pthread_mutex_t wlock;
    atomic_bool connected, closing;
    size_t max_message;
    uint8_t *msg; size_t msg_len, msg_cap; int msg_op;
};

static bool parse_url(const char *url, char *host, size_t hcap, int *port, char *path, size_t pcap)
{
    const char *p = url;
    if (!strncmp(p, "wss://", 6)) { p += 6; *port = 443; }
    else return false;
    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    char hostport[160];
    if (hl >= sizeof hostport) return false;
    memcpy(hostport, p, hl); hostport[hl] = '\0';
    char *colon = strchr(hostport, ':');
    if (colon) { *colon = '\0'; *port = atoi(colon + 1); }
    snprintf(host, hcap, "%s", hostport);
    snprintf(path, pcap, "%s", slash ? slash : "/");
    return true;
}

static bool ssl_write_all(struct net_ws *w, const uint8_t *data, size_t n)
{
    while (n) {
        int r = SSL_write(w->ssl, data, (int)n);
        if (r <= 0) return false;
        data += r; n -= (size_t)r;
    }
    return true;
}

static bool ssl_read_exact(struct net_ws *w, uint8_t *out, size_t n)
{
    while (n) {
        int r = SSL_read(w->ssl, out, (int)n);
        if (r <= 0) return false;
        out += r; n -= (size_t)r;
    }
    return true;
}

static bool send_frame(struct net_ws *w, int opcode, const uint8_t *data, size_t n)
{
    uint8_t hdr[14]; size_t hl = 0;
    hdr[hl++] = 0x80 | (uint8_t)opcode;
    if (n < 126) hdr[hl++] = 0x80 | (uint8_t)n;
    else if (n < 65536) { hdr[hl++] = 0x80 | 126; hdr[hl++] = n >> 8; hdr[hl++] = n & 0xFF; }
    else { hdr[hl++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[hl++] = (uint8_t)((uint64_t)n >> (8 * i)); }
    uint8_t mask[4]; RAND_bytes(mask, 4);
    memcpy(hdr + hl, mask, 4); hl += 4;
    uint8_t *masked = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) masked[i] = data[i] ^ mask[i & 3];
    pthread_mutex_lock(&w->wlock);
    bool ok = ssl_write_all(w, hdr, hl) && ssl_write_all(w, masked, n);
    pthread_mutex_unlock(&w->wlock);
    free(masked);
    return ok;
}

static void deliver(struct net_ws *w)
{
    if (w->msg_op == 1 && w->ev.on_text) { w->msg[w->msg_len] = '\0'; w->ev.on_text((const char *)w->msg, w->msg_len, w->user); }
    else if (w->msg_op == 2 && w->ev.on_binary) w->ev.on_binary(w->msg, w->msg_len, w->user);
    w->msg_len = 0;
}

static void *reader_main(void *arg)
{
    struct net_ws *w = arg;
    uint8_t h[2];
    while (!atomic_load(&w->closing) && ssl_read_exact(w, h, 2)) {
        bool fin = h[0] & 0x80; int op = h[0] & 0x0F; bool masked = h[1] & 0x80; uint64_t len = h[1] & 0x7F;
        if (len == 126) { uint8_t e[2]; if (!ssl_read_exact(w, e, 2)) break; len = (e[0] << 8) | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!ssl_read_exact(w, e, 8)) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i]; }
        uint8_t mk[4] = {0};
        if (masked && !ssl_read_exact(w, mk, 4)) break;
        if (len > w->max_message) { if (w->ev.on_error) w->ev.on_error("message too large", w->user); break; }
        if (op == 8) {   // close
            uint8_t *pl = malloc(len + 1); ssl_read_exact(w, pl, len);
            int code = len >= 2 ? (pl[0] << 8) | pl[1] : 1005;
            free(pl);
            atomic_store(&w->connected, false);
            if (w->ev.on_closed) w->ev.on_closed(code, "", w->user);
            return NULL;
        }
        if (op == 9 || op == 10) {   // ping / pong
            uint8_t *pl = malloc(len + 1); if (len && !ssl_read_exact(w, pl, len)) { free(pl); break; }
            if (op == 9) send_frame(w, 10, pl, (size_t)len);
            free(pl); continue;
        }
        if (op == 1 || op == 2) w->msg_op = op;
        if (w->msg_len + len + 1 > w->msg_cap) {
            size_t ncap = w->msg_cap ? w->msg_cap : 4096;
            while (ncap < w->msg_len + len + 1) ncap *= 2;
            uint8_t *nb = mem_alloc_big(ncap);
            if (w->msg) { memcpy(nb, w->msg, w->msg_len); mem_release(w->msg); }
            w->msg = nb; w->msg_cap = ncap;
        }
        if (len && !ssl_read_exact(w, w->msg + w->msg_len, (size_t)len)) break;
        if (masked) for (uint64_t i = 0; i < len; i++) w->msg[w->msg_len + i] ^= mk[i & 3];
        w->msg_len += (size_t)len;
        if (fin) deliver(w);
    }
    if (atomic_exchange(&w->connected, false) && !atomic_load(&w->closing)) {
        if (w->ev.on_error) w->ev.on_error("connection lost", w->user);
        if (w->ev.on_closed) w->ev.on_closed(1006, "lost", w->user);
    }
    return NULL;
}

esp_err_t net_ws_open(const net_ws_params_t *p, const net_ws_events_t *ev, void *user, net_ws_t **out)
{
    char host[160], path[200]; int port;
    if (!parse_url(p->url, host, sizeof host, &port, path, sizeof path)) return ESP_ERR_INVALID_ARG;
    struct net_ws *w = calloc(1, sizeof *w);
    w->ev = *ev; w->user = user; w->max_message = p->max_message ? p->max_message : (1u << 20);
    pthread_mutex_init(&w->wlock, NULL);
    int64_t t0 = clock_us();

    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL;
    char portstr[8]; snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) { free(w); return ESP_ERR_NOT_FOUND; }
    w->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(w->fd, res->ai_addr, res->ai_addrlen) != 0) { freeaddrinfo(res); close(w->fd); free(w); return ESP_FAIL; }
    freeaddrinfo(res);
    w->ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_default_verify_paths(w->ctx);
    w->ssl = SSL_new(w->ctx);
    SSL_set_tlsext_host_name(w->ssl, host);
    SSL_set_fd(w->ssl, w->fd);
    if (SSL_connect(w->ssl) != 1) { LOGE("TLS to %s failed", host); SSL_free(w->ssl); SSL_CTX_free(w->ctx); close(w->fd); free(w); return ESP_FAIL; }

    uint8_t nonce[16]; RAND_bytes(nonce, 16);
    char key[32]; b64_encode(nonce, 16, key, sizeof key);
    char req[1024]; size_t n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n",
        path, host, key);
    for (size_t i = 0; i < p->header_count && n < sizeof req - 4; i++)
        n += snprintf(req + n, sizeof req - n, "%s: %s\r\n", p->headers[i].name, p->headers[i].value);
    n += snprintf(req + n, sizeof req - n, "\r\n");
    if (!ssl_write_all(w, (const uint8_t *)req, n)) { net_ws_close(w); return ESP_FAIL; }

    char resp[2048]; size_t rl = 0;
    while (rl < sizeof resp - 1) {
        int r = SSL_read(w->ssl, resp + rl, 1);
        if (r <= 0) break;
        rl += (size_t)r; resp[rl] = '\0';
        if (rl >= 4 && !memcmp(resp + rl - 4, "\r\n\r\n", 4)) break;
    }
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        LOGE("ws upgrade failed: %.60s", resp);
        net_ws_close(w);
        return ESP_ERR_INVALID_RESPONSE;
    }
    atomic_store(&w->connected, true);
    LOGI("ws connected %s%s in %lld ms", host, path, (long long)((clock_us() - t0) / 1000));
    if (w->ev.on_connected) w->ev.on_connected(w->user);
    pthread_create(&w->reader, NULL, reader_main, w);
    *out = w;
    return ESP_OK;
}

esp_err_t net_ws_send_text(net_ws_t *w, const char *utf8, size_t n) { return send_frame(w, 1, (const uint8_t *)utf8, n) ? ESP_OK : ESP_FAIL; }
esp_err_t net_ws_send_binary(net_ws_t *w, const uint8_t *data, size_t n) { return send_frame(w, 2, data, n) ? ESP_OK : ESP_FAIL; }
bool net_ws_is_connected(net_ws_t *w) { return w && atomic_load(&w->connected); }

void net_ws_close(net_ws_t *w)
{
    if (!w) return;
    atomic_store(&w->closing, true);
    if (atomic_load(&w->connected)) { uint8_t c[2] = { 0x03, 0xE8 }; send_frame(w, 8, c, 2); }
    if (w->ssl) { SSL_shutdown(w->ssl); }
    if (w->fd > 0) { shutdown(w->fd, SHUT_RDWR); }
    if (w->reader) pthread_join(w->reader, NULL);
    if (w->ssl) SSL_free(w->ssl);
    if (w->ctx) SSL_CTX_free(w->ctx);
    if (w->fd > 0) close(w->fd);
    mem_release(w->msg);
    pthread_mutex_destroy(&w->wlock);
    free(w);
}
