// net_wifi_join.h — how a join is going, from the reason the radio gives each time an attempt ends. One bad handshake can
// be a weak signal and one scan can miss a router, so a verdict waits for the attempts after it. Pure: the device backend
// feeds it, the host test drives it.
#pragma once
#include "net/net.h"

// The driver's wifi_err_reason_t values this file tells apart.
#define NET_WIFI_REASON_ASSOC_LEAVE            8
#define NET_WIFI_REASON_MIC_FAILURE            14
#define NET_WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define NET_WIFI_REASON_NO_AP_FOUND            201
#define NET_WIFI_REASON_AUTH_FAIL              202
#define NET_WIFI_REASON_HANDSHAKE_TIMEOUT      204

typedef struct { uint8_t password, missing, other; } net_join_tally_t;

net_join_t net_wifi_join_note(net_join_tally_t *t, int reason);   // reason: the driver's wifi_err_reason_t. NET_JOIN_TRYING: no verdict yet
