#pragma once
#include "parent/parent.h"
#include <stddef.h>
#include <stdint.h>

// The platform-independent request handler. `body` may be NULL. `hotspot`: the request came from a phone on the hotspot
// (the platform knows by the addresses; the simulator says so in pairing mode). Only such a request, in pairing mode, is
// answered; anyone else gets 403 (from the API a word, for the page content/web/locked.html: how to get in). On return,
// *out is a malloc'd buffer the platform sends and frees; *ctype is a static string. Returns the HTTP status.
int parent_api_handle(const char *method, const char *path, const char *query, const char *body, size_t body_len, bool hotspot,
                      char **out, size_t *out_len, const char **ctype, const char **disposition);

// parent_join.c: try a network, keep it only if it works. The result (trying | ok | bad_password | not_found | failed) comes
// on the worker's task. Returns false when one is already going.
typedef void (*parent_join_result_fn)(const char *state, void *user);
bool parent_join_start(const char *ssid, const char *pass, parent_join_result_fn on_result, void *user);
const char *parent_join_state(void);           // idle, or the last result

// parent_pair.c: the device's screen in pairing mode, a step at a time. The platform calls begin/end with the hotspot and
// phone(true/false) as phones join and leave it; every request the page makes is pair_page(); the page's "connect" pair_join.
void pair_set_card(parent_card_fn fn, void *user);
void pair_begin(void);
void pair_end(void);
void pair_phone(bool joined);
void pair_page(void);
void pair_join(const char *state);
size_t parent_wifi_code(char *out, size_t cap, const char *ssid, const char *pass);   // the camera's Wi-Fi code; 0 if it does not fit

// parent_portal.c: the DNS answer to one query, every name being `ip`. Returns its length, 0 for no answer.
int parent_dns_answer(const uint8_t *q, size_t n, const uint8_t ip[4], uint8_t *out, size_t cap);
bool parent_host_is_name(const char *host);    // a Host header with a name, not an address: it came through our DNS
