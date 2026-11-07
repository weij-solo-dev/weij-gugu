// net_wifi_seen.h — the networks Wi-Fi sweeps have found, kept across sweeps: one sweep misses some (a probe answered too
// late, a channel left too soon), so the list is the union, and a network goes only after NET_WIFI_SEEN_KEEP sweeps in a
// row have not seen it. Pure data: the device backend feeds it, the host test drives it.
#pragma once
#include "net/net.h"

#define NET_WIFI_SEEN_MAX  48
#define NET_WIFI_SEEN_KEEP 3

typedef struct {
    net_wifi_ap_t ap[NET_WIFI_SEEN_MAX];
    uint8_t missed[NET_WIFI_SEEN_MAX];     // sweeps in a row that have not seen it
    int n;
} net_wifi_seen_t;

void net_wifi_seen_merge(net_wifi_seen_t *s, const net_wifi_ap_t *found, int n);   // one sweep's results; nameless networks are skipped
int net_wifi_seen_list(const net_wifi_seen_t *s, net_wifi_ap_t *out, int cap);     // strongest first; returns how many
