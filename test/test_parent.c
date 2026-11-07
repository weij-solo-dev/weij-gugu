// test_parent.c — the pairing page's speaker volume: saved, in use at once (not after a reboot), 0–100 only. Its Wi-Fi
// list: one sweep misses networks, so what the sweeps find is remembered until three in a row have not seen it.
#include "minitest.h"
#include "parent/parent_internal.h"
#include "store/store.h"
#include "net/net_wifi_seen.h"
#include <stdlib.h>
#include <string.h>

#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"

static int s_heard = -1;
static void on_volume(int percent, void *user) { (void)user; s_heard = percent; }

static int post(const char *path, const char *body)
{
    char *out = NULL; size_t n = 0; const char *ct = NULL, *disp = NULL;
    int status = parent_api_handle("POST", path, "", body, body ? strlen(body) : 0, &out, &n, &ct, &disp);
    free(out);
    return status;
}

TEST(test_parent_volume)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    int keep = store_setting_int("volume", -1);
    parent_hooks_t h = { .volume = on_volume };
    parent_set_hooks(&h);
    CHECK_EQ_INT(post("/api/volume", "{\"volume\":40}"), 200);
    CHECK_EQ_INT(s_heard, 40);                                   // the speaker has it now
    CHECK_EQ_INT(store_setting_int("volume", -1), 40);           // and it is kept for the next boot
    const char *bad[] = { "{\"volume\":101}", "{\"volume\":-1}", "{\"volume\":\"loud\"}", "{}", "not json", NULL };
    for (int i = 0; bad[i]; i++) {
        s_heard = -1;
        CHECK_EQ_INT(post("/api/volume", bad[i]), 400);
        CHECK_EQ_INT(s_heard, -1);
    }
    CHECK_EQ_INT(post("/api/volume", NULL), 400);
    CHECK_EQ_INT(store_setting_int("volume", -1), 40);           // nothing bad was kept
    if (keep >= 0) store_setting_set_int("volume", keep);
    parent_set_hooks(NULL);
}

static net_wifi_ap_t ap(const char *ssid, int rssi)
{
    net_wifi_ap_t a = { .rssi = rssi, .secure = true };
    snprintf(a.ssid, sizeof a.ssid, "%s", ssid);
    return a;
}

TEST(test_wifi_seen)
{
    static net_wifi_seen_t s; memset(&s, 0, sizeof s);
    static net_wifi_ap_t out[NET_WIFI_SEEN_MAX + 8];
    net_wifi_ap_t one[] = { ap("near", -40), ap("far", -80), ap("", -30), ap("near", -60) };   // a nameless one, the same name twice
    net_wifi_seen_merge(&s, one, 4);
    CHECK_EQ_INT(net_wifi_seen_list(&s, out, 8), 2);
    CHECK_EQ_STR(out[0].ssid, "near"); CHECK_EQ_INT(out[0].rssi, -40);                         // strongest first; a name once, at its best
    CHECK_EQ_STR(out[1].ssid, "far");
    net_wifi_ap_t two[] = { ap("new", -50), ap("far", -70) };                                  // this sweep missed "near" and found "new"
    net_wifi_seen_merge(&s, two, 2);
    CHECK_EQ_INT(net_wifi_seen_list(&s, out, 8), 3);                                           // the union, not the last sweep
    CHECK_EQ_STR(out[0].ssid, "near"); CHECK_EQ_STR(out[1].ssid, "new"); CHECK_EQ_STR(out[2].ssid, "far");
    CHECK_EQ_INT(out[2].rssi, -70);                                                            // a later sweep's reading replaces the old one
    net_wifi_seen_merge(&s, two, 2);                                                           // "near" unseen twice: still there
    CHECK_EQ_INT(net_wifi_seen_list(&s, out, 8), 3);
    net_wifi_seen_merge(&s, two, 2);                                                           // three times: gone (the router went off)
    CHECK_EQ_INT(net_wifi_seen_list(&s, out, 8), 2);
    CHECK_EQ_STR(out[0].ssid, "new");
    CHECK_EQ_INT(net_wifi_seen_list(&s, out, 1), 1);                                           // never past the caller's room
    static net_wifi_ap_t crowd[NET_WIFI_SEEN_MAX + 8];                                         // a block of flats: the strong ones stay when it is full
    for (int i = 0; i < NET_WIFI_SEEN_MAX + 8; i++) { char n[16]; snprintf(n, sizeof n, "flat-%d", i); crowd[i] = ap(n, -90 + i); }
    net_wifi_seen_merge(&s, crowd, NET_WIFI_SEEN_MAX + 8);
    CHECK_EQ_INT(net_wifi_seen_list(&s, out, NET_WIFI_SEEN_MAX + 8), NET_WIFI_SEEN_MAX);
    char last[16]; snprintf(last, sizeof last, "flat-%d", NET_WIFI_SEEN_MAX + 7);
    CHECK_EQ_STR(out[0].ssid, last);
    net_wifi_seen_merge(&s, NULL, 0);                                                          // a sweep that found nothing costs nobody yet
    CHECK_EQ_INT(net_wifi_seen_list(&s, out, NET_WIFI_SEEN_MAX + 8), NET_WIFI_SEEN_MAX);
}

// The page lists what the sweeps found so far and whether one is running; only ?sweep=1 starts another.
TEST(test_parent_wifi_list)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    char *out = NULL; size_t n = 0; const char *ct = NULL, *disp = NULL;
    CHECK_EQ_INT(parent_api_handle("GET", "/api/wifi/scan", "sweep=1", NULL, 0, &out, &n, &ct, &disp), 200);
    CHECK(out && strstr(out, "\"sweeping\":") && strstr(out, "\"aps\":[{\"ssid\":\"") && strstr(out, "\"secure\":"));
    free(out); out = NULL;
    CHECK_EQ_INT(parent_api_handle("GET", "/api/wifi/scan", "", NULL, 0, &out, &n, &ct, &disp), 200);
    CHECK(out && strstr(out, "\"aps\":["));
    free(out);
}

void test_parent(void)
{
    RUN(test_parent_volume);
    RUN(test_wifi_seen);
    RUN(test_parent_wifi_list);
}
