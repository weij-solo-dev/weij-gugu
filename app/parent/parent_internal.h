#pragma once
#include "parent/parent.h"
#include <stddef.h>
#include <stdint.h>

// The platform-independent request handler. `body` may be NULL. On return, *out is a malloc'd buffer
// the platform sends and frees; *ctype is a static string. Returns the HTTP status.
int parent_api_handle(const char *method, const char *path, const char *query, const char *body, size_t body_len,
                      char **out, size_t *out_len, const char **ctype, const char **disposition);

// parent_join.c: try a network, keep it only if it works. The result (trying | ok | bad_password | not_found | failed) comes
// on the worker's task. Returns false when one is already going.
typedef void (*parent_join_result_fn)(const char *state, void *user);
bool parent_join_start(const char *ssid, const char *pass, parent_join_result_fn on_result, void *user);
const char *parent_join_state(void);           // idle, or the last result

// parent_portal.c: the DNS answer to one query, every name being `ip`. Returns its length, 0 for no answer.
int parent_dns_answer(const uint8_t *q, size_t n, const uint8_t ip[4], uint8_t *out, size_t cap);
bool parent_host_is_name(const char *host);    // a Host header with a name, not an address: it came through our DNS
