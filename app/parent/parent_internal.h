#pragma once
#include "parent/parent.h"
#include <stddef.h>

// The platform-independent request handler. `body` may be NULL. On return, *out is a malloc'd buffer
// the platform sends and frees; *ctype is a static string. Returns the HTTP status.
int parent_api_handle(const char *method, const char *path, const char *query, const char *body, size_t body_len,
                      char **out, size_t *out_len, const char **ctype, const char **disposition);
