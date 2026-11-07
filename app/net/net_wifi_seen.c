// net_wifi_seen.c — see net_wifi_seen.h.
#include "net/net_wifi_seen.h"
#include "foundation/mem.h"
#include <stdlib.h>
#include <string.h>

static int find(const net_wifi_seen_t *s, const char *ssid)
{
    for (int i = 0; i < s->n; i++) if (!strcmp(s->ap[i].ssid, ssid)) return i;
    return -1;
}

static int weakest(const net_wifi_seen_t *s)
{
    int w = 0;
    for (int i = 1; i < s->n; i++) if (s->ap[i].rssi < s->ap[w].rssi) w = i;
    return w;
}

void net_wifi_seen_merge(net_wifi_seen_t *s, const net_wifi_ap_t *found, int n)
{
    for (int i = 0; i < s->n; i++) s->missed[i]++;
    for (int i = 0; i < n; i++) {
        const net_wifi_ap_t *a = &found[i];
        if (!a->ssid[0]) continue;
        int k = find(s, a->ssid);
        if (k >= 0) {                                       // the first reading of this sweep replaces the old; a second router of that name counts if stronger
            if (s->missed[k] == 0 && a->rssi <= s->ap[k].rssi) continue;
        } else if (s->n < NET_WIFI_SEEN_MAX) {
            k = s->n++;
        } else {                                            // full: a stronger one takes the weakest's place
            k = weakest(s);
            if (a->rssi <= s->ap[k].rssi) continue;
        }
        s->ap[k] = *a; s->missed[k] = 0;
    }
    if (n <= 0) { for (int i = 0; i < s->n; i++) s->missed[i]--; return; }   // a sweep that found nothing says more about the radio than the air
    int kept = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->missed[i] >= NET_WIFI_SEEN_KEEP) continue;
        if (kept != i) { s->ap[kept] = s->ap[i]; s->missed[kept] = s->missed[i]; }
        kept++;
    }
    s->n = kept;
}

static int stronger(const void *a, const void *b) { return ((const net_wifi_ap_t *)b)->rssi - ((const net_wifi_ap_t *)a)->rssi; }

int net_wifi_seen_list(const net_wifi_seen_t *s, net_wifi_ap_t *out, int cap)
{
    static MEM_BIG_BSS net_wifi_ap_t sorted[NET_WIFI_SEEN_MAX];         // callers hold the backend's lock, or are the test
    memcpy(sorted, s->ap, (size_t)s->n * sizeof sorted[0]);
    qsort(sorted, (size_t)s->n, sizeof sorted[0], stronger);
    int n = s->n < cap ? s->n : cap;
    memcpy(out, sorted, (size_t)n * sizeof out[0]);
    return n;
}
