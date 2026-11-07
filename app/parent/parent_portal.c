// parent_portal.c — what makes a phone open the page by itself once it has joined the hotspot. Phones test a joined network
// by asking for a known address by name; the hotspot answers every name with the device, and the web server sends a
// request that came by name to the page, which the phone takes for a sign-in page and shows. The sockets are parent_esp.c's.
#include "parent_internal.h"
#include <ctype.h>
#include <string.h>

#define DNS_HEADER 12
#define DNS_TTL_S  30                          // short: the name means something else again as soon as pairing mode ends

int parent_dns_answer(const uint8_t *q, size_t n, const uint8_t ip[4], uint8_t *out, size_t cap)
{
    if (n < DNS_HEADER || (q[2] & 0xF8)) return 0;                     // a response, or not a standard query
    if (q[4] != 0 || q[5] != 1) return 0;                              // exactly one question, as resolvers ask
    size_t p = DNS_HEADER;
    while (p < n && q[p]) {
        if (q[p] & 0xC0) return 0;                                     // a question spells its name out
        p += 1 + (size_t)q[p];
    }
    if (p + 5 > n) return 0;                                           // the root label, then type and class
    bool want_a = q[p + 1] == 0 && q[p + 2] == 1 && q[p + 3] == 0 && q[p + 4] == 1;
    size_t qend = p + 5, total = qend + (want_a ? 16 : 0);
    if (total > cap) return 0;
    memcpy(out, q, qend);
    out[2] = (uint8_t)(0x84 | (q[2] & 0x01));                          // response, authoritative, recursion desired as asked
    out[3] = 0x80;                                                     // recursion available, no error
    out[6] = 0; out[7] = want_a ? 1 : 0;                               // anything else (AAAA, HTTPS): no such record, at once
    memset(out + 8, 0, 4);                                             // no authority, no additional (an EDNS record is dropped)
    if (want_a) {
        const uint8_t tail[12] = { 0xC0, DNS_HEADER, 0, 1, 0, 1, 0, 0, 0, DNS_TTL_S, 0, 4 };   // the question's name, A, IN, TTL, length
        memcpy(out + qend, tail, sizeof tail);
        memcpy(out + qend + sizeof tail, ip, 4);
    }
    return (int)total;
}

bool parent_host_is_name(const char *host)
{
    if (host[0] == '[') return false;                                  // an IPv6 literal has letters too
    for (const char *h = host; *h && *h != ':'; h++) if (isalpha((unsigned char)*h)) return true;
    return false;
}
