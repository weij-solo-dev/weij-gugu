// test_parent.c — the pairing page's speaker volume: saved, in use at once (not after a reboot), 0–100 only. Its Wi-Fi
// list: one sweep misses networks, so what the sweeps find is remembered until three in a row have not seen it. Its
// "connect": a network is tried first and kept only if it works. Its captive portal: every name answered with the device.
#include "minitest.h"
#include "parent/parent_internal.h"
#include "store/store.h"
#include "foundation/clock.h"
#include "net/net_wifi_seen.h"
#include "net/net_wifi_join.h"
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

// What the radio says when an attempt ends → the verdict: one bad handshake can be a weak signal, so it takes two.
TEST(test_wifi_join_verdict)
{
    net_join_tally_t t = { 0 };
    CHECK_EQ_INT(net_wifi_join_note(&t, 8), NET_JOIN_TRYING);        // leaving the old network is not a failure of the new one
    CHECK_EQ_INT(t.password + t.missing + t.other, 0);
    CHECK_EQ_INT(net_wifi_join_note(&t, 15), NET_JOIN_TRYING);       // 4-way handshake timeout: the usual wrong password
    CHECK_EQ_INT(net_wifi_join_note(&t, 204), NET_JOIN_BAD_PASSWORD);
    t = (net_join_tally_t){ 0 };
    CHECK_EQ_INT(net_wifi_join_note(&t, 202), NET_JOIN_TRYING);      // WPA3 says it this way
    CHECK_EQ_INT(net_wifi_join_note(&t, 202), NET_JOIN_BAD_PASSWORD);
    t = (net_join_tally_t){ 0 };
    CHECK_EQ_INT(net_wifi_join_note(&t, 201), NET_JOIN_TRYING);      // no such network: one scan can miss a router
    CHECK_EQ_INT(net_wifi_join_note(&t, 201), NET_JOIN_TRYING);
    CHECK_EQ_INT(net_wifi_join_note(&t, 201), NET_JOIN_NOT_FOUND);
    t = (net_join_tally_t){ 0 };
    CHECK_EQ_INT(net_wifi_join_note(&t, 211), NET_JOIN_TRYING);      // there, but its security is too old: neither of the above
    CHECK_EQ_INT(net_wifi_join_note(&t, 211), NET_JOIN_TRYING);
    CHECK_EQ_INT(net_wifi_join_note(&t, 211), NET_JOIN_FAILED);
    t = (net_join_tally_t){ 0 };
    CHECK_EQ_INT(net_wifi_join_note(&t, 15), NET_JOIN_TRYING);       // a mixed bag still ends
    CHECK_EQ_INT(net_wifi_join_note(&t, 201), NET_JOIN_TRYING);
    CHECK_EQ_INT(net_wifi_join_note(&t, 203), NET_JOIN_TRYING);
    CHECK_EQ_INT(net_wifi_join_note(&t, 205), NET_JOIN_FAILED);
}

const char *net_host_last_join(void);                                 // net_host.c: the network last asked for ("" after a leave)
static char s_results[64];
static void on_wifi_result(const char *state, void *user) { (void)user; strlcat(s_results, state, sizeof s_results); strlcat(s_results, " ", sizeof s_results); }

static int join(const char *body)
{
    s_results[0] = '\0';
    return post("/api/wifi/join", body);
}

static bool join_says(const char *state)                              // the page's poll, until the worker is done
{
    char want[40]; snprintf(want, sizeof want, "\"state\":\"%s\"", state);
    bool ok = false;
    for (int64_t t0 = clock_us(); clock_us() - t0 < 5000000; clock_sleep_ms(20)) {
        char *out = NULL; size_t n = 0; const char *ct = NULL, *disp = NULL;
        parent_api_handle("GET", "/api/wifi/join", "", NULL, 0, &out, &n, &ct, &disp);
        bool trying = out && strstr(out, "\"trying\""); ok = out && strstr(out, want);
        free(out);
        if (!trying) break;
    }
    return ok;
}

static bool secret_is(const char *key, const char *want)
{
    char v[64] = "";
    store_secret_get(key, v, sizeof v);
    return !strcmp(v, want);
}

// "Connect" tries the network and keeps it only if it works: a mistyped password must not cost the network that works.
TEST(test_parent_wifi_join)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    parent_hooks_t h = { .wifi_result = on_wifi_result };
    parent_set_hooks(&h);
    const char *body = "{\"ssid\":\"NewNet\",\"pass\":\"newpass12\"}";

    store_secret_set("wifi_ssid", "OldNet"); store_secret_set("wifi_pass", "oldpass12");
    setenv("GUGU_WIFI_JOIN", "bad_password", 1);
    CHECK_EQ_INT(join(body), 200);
    CHECK(join_says("bad_password"));
    CHECK(secret_is("wifi_ssid", "OldNet") && secret_is("wifi_pass", "oldpass12"));   // kept as they were
    CHECK(!strcmp(net_host_last_join(), "OldNet"));                                    // and the device is back on it
    CHECK(!strcmp(s_results, "trying bad_password "));                                 // the screen hears both

    store_secret_set("wifi_ssid", ""); store_secret_set("wifi_pass", "");              // first setup: nothing to go back to
    setenv("GUGU_WIFI_JOIN", "not_found", 1);
    CHECK_EQ_INT(join(body), 200);
    CHECK(join_says("not_found"));
    CHECK(secret_is("wifi_ssid", ""));
    CHECK(!strcmp(net_host_last_join(), ""));                                          // no retries in the hotspot's way

    setenv("GUGU_WIFI_JOIN", "ok", 1);
    CHECK_EQ_INT(join(body), 200);
    CHECK(join_says("ok"));
    CHECK(secret_is("wifi_ssid", "NewNet") && secret_is("wifi_pass", "newpass12"));    // it works: now it is kept
    CHECK(!strcmp(s_results, "trying ok "));
    {
        char *out = NULL; size_t n = 0; const char *ct = NULL, *disp = NULL;
        CHECK_EQ_INT(parent_api_handle("GET", "/api/status", "", NULL, 0, &out, &n, &ct, &disp), 200);
        CHECK(out && strstr(out, "\"wifi_configured\":true"));                         // at once, not after a reboot
        free(out);
    }

    CHECK_EQ_INT(join("{\"ssid\":\"OpenNet\"}"), 200);                                  // an open network has no password
    CHECK(join_says("ok"));
    CHECK(secret_is("wifi_ssid", "OpenNet") && secret_is("wifi_pass", ""));            // and the last one's does not stay behind

    const char *bad[] = { "{\"pass\":\"newpass12\"}", "{\"ssid\":\"\",\"pass\":\"newpass12\"}", "{\"ssid\":\"N\",\"pass\":\"short\"}",
                          "{\"ssid\":\"123456789012345678901234567890123\"}",     // 33 bytes
                          "{\"ssid\":\"N\",\"pass\":\"1234567890123456789012345678901234567890123456789012345678901234\"}",   // 64
                          "not json", NULL };
    for (int i = 0; bad[i]; i++) CHECK_EQ_INT(post("/api/wifi/join", bad[i]), 400);
    CHECK_EQ_INT(post("/api/wifi/join", NULL), 400);
    CHECK(secret_is("wifi_ssid", "OpenNet"));

    unsetenv("GUGU_WIFI_JOIN");
    store_secret_set("wifi_ssid", ""); store_secret_set("wifi_pass", "");
    parent_set_hooks(NULL);
}

static size_t dns_query(uint8_t *q, const char *name, uint16_t type)
{
    static const uint8_t head[12] = { 0xAB, 0xCD, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };   // id, recursion desired, one question
    memcpy(q, head, 12);
    size_t p = 12;
    for (const char *s = name; *s; ) {
        const char *dot = strchr(s, '.'); size_t l = dot ? (size_t)(dot - s) : strlen(s);
        q[p++] = (uint8_t)l; memcpy(q + p, s, l); p += l; s += l + (dot ? 1 : 0);
    }
    q[p++] = 0; q[p++] = 0; q[p++] = (uint8_t)type; q[p++] = 0; q[p++] = 1;
    return p;
}

// Every name is the device, so a phone's connectivity probe lands on the page. Anything malformed gets no answer.
TEST(test_parent_dns)
{
    const uint8_t ip[4] = { 192, 168, 4, 1 };
    uint8_t q[128], a[128];
    size_t n = dns_query(q, "captive.apple.com", 1);
    int m = parent_dns_answer(q, n, ip, a, sizeof a);
    CHECK_EQ_INT(m, (int)n + 16);
    CHECK(a[0] == 0xAB && a[1] == 0xCD);                              // the same id
    CHECK(a[2] & 0x80);                                               // a response
    CHECK(a[2] & 0x01);                                               // recursion desired, repeated
    CHECK_EQ_INT(a[3] & 0x0F, 0);                                     // no error
    CHECK_EQ_INT(a[7], 1);                                            // one answer
    CHECK(!memcmp(a + 12, q + 12, n - 12));                           // the question, repeated
    CHECK(a[n] == 0xC0 && a[n + 1] == 0x0C);                          // the name: a pointer to the question's
    CHECK(!memcmp(a + m - 4, ip, 4));

    n = dns_query(q, "captive.apple.com", 28);                        // AAAA: no such record, at once (no waiting, no IPv6 route)
    m = parent_dns_answer(q, n, ip, a, sizeof a);
    CHECK_EQ_INT(m, (int)n);
    CHECK_EQ_INT(a[7], 0);
    CHECK_EQ_INT(a[3] & 0x0F, 0);

    n = dns_query(q, "gugu.test", 1);
    uint8_t edns[140]; memcpy(edns, q, n); edns[11] = 1; memset(edns + n, 0, 11); edns[n + 2] = 41;   // an EDNS record behind the question
    m = parent_dns_answer(edns, n + 11, ip, a, sizeof a);
    CHECK_EQ_INT(m, (int)n + 16);
    CHECK_EQ_INT(a[11], 0);                                           // not repeated in the answer

    CHECK_EQ_INT(parent_dns_answer(q, 11, ip, a, sizeof a), 0);       // shorter than a header
    CHECK_EQ_INT(parent_dns_answer(q, n - 1, ip, a, sizeof a), 0);    // cut inside the question
    CHECK_EQ_INT(parent_dns_answer(q, n, ip, a, n + 15), 0);          // no room for the answer
    uint8_t bad[32]; memcpy(bad, q, n);
    bad[2] |= 0x80; CHECK_EQ_INT(parent_dns_answer(bad, n, ip, a, sizeof a), 0);      // already an answer: never one to a reply
    memcpy(bad, q, n); bad[2] |= 0x08; CHECK_EQ_INT(parent_dns_answer(bad, n, ip, a, sizeof a), 0);   // not a plain query
    memcpy(bad, q, n); bad[5] = 2; CHECK_EQ_INT(parent_dns_answer(bad, n, ip, a, sizeof a), 0);       // two questions
    memcpy(bad, q, n); bad[12] = 200; CHECK_EQ_INT(parent_dns_answer(bad, n, ip, a, sizeof a), 0);    // a label running off the end
    memcpy(bad, q, n); bad[12] = 0xC0; CHECK_EQ_INT(parent_dns_answer(bad, n, ip, a, sizeof a), 0);   // a pointer in a question
}

// Asked for by name: it came through our DNS, somebody's probe. Asked for by address: the page itself.
TEST(test_parent_host_is_name)
{
    CHECK(parent_host_is_name("captive.apple.com"));
    CHECK(parent_host_is_name("connectivitycheck.gstatic.com:80"));
    CHECK(!parent_host_is_name("192.168.4.1"));
    CHECK(!parent_host_is_name("192.168.4.1:80"));
    CHECK(!parent_host_is_name("[fe80::abcd]"));
    CHECK(!parent_host_is_name(""));
}

void test_parent(void)
{
    RUN(test_parent_volume);
    RUN(test_wifi_seen);
    RUN(test_parent_wifi_list);
    RUN(test_wifi_join_verdict);
    RUN(test_parent_wifi_join);
    RUN(test_parent_dns);
    RUN(test_parent_host_is_name);
}
