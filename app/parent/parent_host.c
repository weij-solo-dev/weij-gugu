// parent_host.c — a tiny HTTP/1.1 server on localhost:8088 so the same page and API run in the simulator.
#define LOG_TAG "parent"
#include "parent_internal.h"
#include "foundation/log.h"
#include "foundation/task.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PORT 8088
static bool s_active;

static void serve(int fd)
{
    char buf[16384]; size_t n = 0;
    for (;;) {
        ssize_t r = recv(fd, buf + n, sizeof buf - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r; buf[n] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (!hdr_end) continue;
        size_t clen = 0; char *cl = strcasestr(buf, "Content-Length:");
        if (cl) clen = (size_t)atoi(cl + 15);
        size_t have = n - (size_t)(hdr_end + 4 - buf);
        if (have < clen && n < sizeof buf - 1) continue;
        char method[8], uri[512];
        if (sscanf(buf, "%7s %511s", method, uri) != 2) break;
        char *q = strchr(uri, '?'); if (q) *q++ = '\0';
        char *out = NULL; size_t out_len = 0; const char *ctype, *disp;
        int status = parent_api_handle(method, uri, q, hdr_end + 4, clen, &out, &out_len, &ctype, &disp);
        char head[512];
        int hl = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%s%s%sConnection: close\r\n\r\n",
                          status, status == 200 ? "OK" : "Error", ctype, out_len, disp ? "Content-Disposition: " : "", disp ? disp : "", disp ? "\r\n" : "");
        send(fd, head, (size_t)hl, 0);
        if (out) send(fd, out, out_len, 0);
        free(out);
        break;
    }
    close(fd);
}

static void server_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) { LOGW("parent web: port %d busy", PORT); close(s); return; }
    LOGI("parent web ready: http://127.0.0.1:%d/", PORT);
    for (;;) { int c = accept(s, NULL, NULL); if (c >= 0) serve(c); }
}

esp_err_t parent_init(void) { return task_spawn("gugu_parent", server_task, NULL, 65536, 1, 0) ? ESP_OK : ESP_FAIL; }
esp_err_t parent_enter(void)
{
    s_active = true;
    LOGI("hotspot simulated: gugu-HOST");
    pair_begin();
    if (getenv("GUGU_PAIR_PHONE")) pair_phone(true);   // screenshots: a phone on the hotspot (any request to the page then opens it)
    return ESP_OK;
}
void parent_exit(void) { if (s_active) { s_active = false; pair_end(); } }
bool parent_is_active(void) { return s_active; }
const char *parent_hotspot_ssid(void) { return "gugu-HOST"; }
const char *parent_hotspot_password(void) { return s_active ? "12345678" : ""; }
const char *parent_hotspot_url(void) { return "http://127.0.0.1:8088"; }
