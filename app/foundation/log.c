#include "foundation/log.h"
#include <string.h>
#include <stdlib.h>

const char *log_mask(const char *secret, char out[8])
{
    if (secret == NULL || secret[0] == '\0') {
        strcpy(out, "(none)");
        return out;
    }
    size_t n = strlen(secret);
    size_t keep = n < 4 ? n : 4;
    memcpy(out, secret, keep);
    out[keep] = '\0';
    strcat(out, "...");
    return out;
}

#ifdef GUGU_HOST
int log_debug_enabled(void)
{
    static int cached = -1;
    if (cached < 0) cached = getenv("GUGU_DEBUG") != NULL;
    return cached;
}
#endif
