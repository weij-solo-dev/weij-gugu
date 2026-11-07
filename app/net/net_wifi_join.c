// net_wifi_join.c — see net_wifi_join.h. The reasons are the driver's numbers (net_esp.c checks them against its enum at
// compile time): this file also builds on the host, which has no driver.
#include "net/net_wifi_join.h"

#define PASSWORD_TRIES 2
#define MISSING_TRIES  3
#define OTHER_TRIES    3
#define ANY_TRIES      4

net_join_t net_wifi_join_note(net_join_tally_t *t, int reason)
{
    switch (reason) {
    case NET_WIFI_REASON_ASSOC_LEAVE: return NET_JOIN_TRYING;          // the network being left, not the one being joined
    case NET_WIFI_REASON_MIC_FAILURE:
    case NET_WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case NET_WIFI_REASON_AUTH_FAIL:
    case NET_WIFI_REASON_HANDSHAKE_TIMEOUT: t->password++; break;
    case NET_WIFI_REASON_NO_AP_FOUND: t->missing++; break;
    default: t->other++; break;                                        // among them: found, but its security is too old
    }
    if (t->password >= PASSWORD_TRIES) return NET_JOIN_BAD_PASSWORD;
    if (t->missing >= MISSING_TRIES) return NET_JOIN_NOT_FOUND;
    if (t->other >= OTHER_TRIES || t->password + t->missing + t->other >= ANY_TRIES) return NET_JOIN_FAILED;
    return NET_JOIN_TRYING;
}
