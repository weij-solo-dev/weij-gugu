// test_parent.c — the pairing page's speaker volume: saved, in use at once (not after a reboot), 0–100 only. Its Wi-Fi
// list: one sweep misses networks, so what the sweeps find is remembered until three in a row have not seen it. Its
// "connect": a network is tried first and kept only if it works. Its captive portal: every name answered with the device.
// The device's screen in pairing mode: the codes for the phone's camera, a step at a time.
// Its lock: the Wi-Fi password and the keys only to a phone on the hotspot, in pairing mode.
#include "minitest.h"
#include "parent/parent_internal.h"
#include "store/store.h"
#include "foundation/clock.h"
#include "net/net_wifi_seen.h"
#include "net/net_wifi_join.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"

static int s_heard = -1;
static void on_volume(int percent, void *user) { (void)user; s_heard = percent; }

// One request; `hotspot`: from a phone on the hotspot (else from the home network). *out, when asked for, is the caller's.
static int call(const char *method, const char *path, const char *body, bool hotspot, char **out)
{
    char *o = NULL; size_t n = 0; const char *ct = NULL, *disp = NULL;
    int status = parent_api_handle(method, path, "", body, body ? strlen(body) : 0, hotspot, &o, &n, &ct, &disp);
    if (out) *out = o; else free(o);
    return status;
}
static int post(const char *path, const char *body) { return call("POST", path, body, true, NULL); }

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
    CHECK_EQ_INT(parent_api_handle("GET", "/api/wifi/scan", "sweep=1", NULL, 0, true, &out, &n, &ct, &disp), 200);
    CHECK(out && strstr(out, "\"sweeping\":") && strstr(out, "\"aps\":[{\"ssid\":\"") && strstr(out, "\"secure\":"));
    free(out); out = NULL;
    CHECK_EQ_INT(parent_api_handle("GET", "/api/wifi/scan", "", NULL, 0, true, &out, &n, &ct, &disp), 200);
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
        parent_api_handle("GET", "/api/wifi/join", "", NULL, 0, true, &out, &n, &ct, &disp);
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
    CHECK_EQ_INT(parent_enter(), ESP_OK);                                 // the page's "connect" works from the hotspot only
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
        CHECK_EQ_INT(parent_api_handle("GET", "/api/status", "", NULL, 0, true, &out, &n, &ct, &disp), 200);
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
    parent_exit();
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

// The screen in pairing mode, a step at a time (2026-11-10, the owner: "比如手机扫码直接连接到配置网页"): a Wi-Fi code the
// phone's camera joins with, the page's address once a phone is on, "carry on" once the page is open, then how a try went.
static char s_qr[160], s_text[200], s_seen[400];
static int s_cards;
static bool s_card_gone;
static void on_card(const char *qr, const char *text, void *user)
{
    (void)user;
    s_cards++;
    s_card_gone = !text;
    snprintf(s_qr, sizeof s_qr, "%s", qr ? qr : "");
    snprintf(s_text, sizeof s_text, "%s", text ? text : "");
    strlcat(s_seen, text ? text : "(gone)", sizeof s_seen); strlcat(s_seen, "|", sizeof s_seen);
}

static void page_asks(const char *path)
{
    char *out = NULL; size_t n = 0; const char *ct = NULL, *disp = NULL;
    parent_api_handle("GET", path, "", NULL, 0, true, &out, &n, &ct, &disp);
    free(out);
}

#define WIFI_CODE "WIFI:T:WPA;S:gugu-7A3F;P:40917263;;"

TEST(test_parent_pairing_card)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    parent_hooks_t h = { .card = on_card };
    parent_set_hooks(&h);
    s_cards = 0; s_seen[0] = '\0';
    page_asks("/api/status");                                          // not in pairing mode: the page on the home network
    CHECK_EQ_INT(s_cards, 0);                                          // puts nothing on the screen

    CHECK_EQ_INT(parent_enter(), ESP_OK);                              // 1. no phone yet: the code the camera joins with
    CHECK_EQ_STR(s_qr, WIFI_CODE);
    CHECK(strstr(s_text, "gugu-7A3F") && strstr(s_text, "40917263"));  // and the name and password, for a phone that cannot scan
    CHECK(strstr(s_text, "hold yellow 3 s to leave"));

    pair_phone(true);                                                  // 2. a phone is on, the page not open: its address
    CHECK_EQ_STR(s_qr, "http://192.168.4.1");
    CHECK(strstr(s_text, "open 192.168.4.1"));                         // and to type, for a camera that cannot scan
    CHECK(!strstr(s_text, "40917263"));
    int before = s_cards;
    call("GET", "/", NULL, false, NULL);                               // the page opened on the home network is not the phone's
    CHECK_EQ_INT(s_cards, before);
    CHECK_EQ_STR(s_qr, "http://192.168.4.1");

    page_asks("/");                                                    // 3. the page is open: no code, carry on there
    CHECK_EQ_STR(s_qr, "");
    CHECK(strstr(s_text, "on the phone"));
    int n = s_cards;
    page_asks("/api/wifi/scan"); page_asks("/api/status");             // its polls change nothing
    CHECK_EQ_INT(s_cards, n);
    pair_phone(true); pair_phone(false);                               // a second phone comes and goes: the page stays
    CHECK(strstr(s_text, "on the phone"));

    setenv("GUGU_WIFI_JOIN", "bad_password", 1);                       // 4. connect: trying, then how it went
    s_seen[0] = '\0';
    CHECK_EQ_INT(join("{\"ssid\":\"NewNet\",\"pass\":\"newpass12\"}"), 200);
    CHECK(join_says("bad_password"));
    CHECK(strstr(s_seen, "Connecting...|") == s_seen);                 // first "Connecting...", then the reason
    CHECK(strstr(s_text, "Wrong password?") && strstr(s_text, "Try again on the phone"));
    CHECK_EQ_STR(s_qr, "");

    pair_phone(false);                                                 // the last phone left: the Wi-Fi code again
    CHECK_EQ_STR(s_qr, WIFI_CODE);
    pair_phone(false);                                                 // a leave too many does not go below none
    pair_phone(true);
    CHECK_EQ_STR(s_qr, "http://192.168.4.1");                          // and one phone back is a phone on

    setenv("GUGU_WIFI_JOIN", "ok", 1);                                 // 5. it worked: "Connected!" stays
    CHECK_EQ_INT(join("{\"ssid\":\"NewNet\",\"pass\":\"newpass12\"}"), 200);
    CHECK(join_says("ok"));
    CHECK(strstr(s_text, "Connected!") && strstr(s_text, "hold yellow 3 s to leave"));
    CHECK_EQ_STR(s_qr, "");
    pair_phone(false);                                                 // whatever the phones do (the hotspot may move channel)
    CHECK(strstr(s_text, "Connected!"));

    parent_exit();                                                     // 6. out of pairing mode: the card goes
    CHECK(s_card_gone);
    n = s_cards;
    pair_phone(true); page_asks("/"); pair_join("ok");                 // and nothing after brings it back
    CHECK_EQ_INT(s_cards, n);

    CHECK_EQ_INT(parent_enter(), ESP_OK);                              // 7. the next time starts from the first step
    CHECK_EQ_STR(s_qr, WIFI_CODE);
    parent_exit();

    unsetenv("GUGU_WIFI_JOIN");
    store_secret_set("wifi_ssid", ""); store_secret_set("wifi_pass", "");
    parent_set_hooks(NULL);
}

// The page's banner (2026-11-10, the owner: "出现匹配页面后，应该给强提醒告诉用户遇到什么问题"): /api/status says whether a
// call can start and, if not, what is wrong and whose it is.
static parent_problem_t s_problem;
static void on_problem(parent_problem_t *p, void *user) { (void)user; *p = s_problem; }
static cJSON *status_json(void)
{
    char *out = NULL; size_t n = 0; const char *ct = NULL, *disp = NULL;
    parent_api_handle("GET", "/api/status", "", NULL, 0, true, &out, &n, &ct, &disp);
    cJSON *j = out ? cJSON_Parse(out) : NULL;
    free(out);
    return j;
}

TEST(test_parent_status_problem)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    parent_set_hooks(NULL);
    cJSON *j = status_json();                                          // nobody to ask: the page shows no banner
    CHECK(j && !cJSON_GetObjectItem(j, "ready") && !cJSON_GetObjectItem(j, "problem"));
    cJSON_Delete(j);

    parent_hooks_t h = { .problem = on_problem };
    parent_set_hooks(&h);
    s_problem = (parent_problem_t){ .ready = false, .cause = "auth", .role = "chat", .vendor = "minimax" };
    snprintf(s_problem.why, sizeof s_problem.why, "service: chat: invalid api key \"x\" (http 401, code 1004)");
    j = status_json();
    CHECK(j && cJSON_IsFalse(cJSON_GetObjectItem(j, "ready")));
    cJSON *p = cJSON_GetObjectItem(j, "problem");
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(p, "cause")), "auth");
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(p, "role")), "chat");
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(p, "vendor")), "minimax");
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(p, "why")), s_problem.why);   // quotes and all, as JSON
    CHECK(cJSON_GetObjectItem(j, "battery_mv") != NULL);               // and the rest of the status as before
    cJSON_Delete(j);

    s_problem = (parent_problem_t){ .ready = true };
    j = status_json();
    CHECK(j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ready")) && !cJSON_GetObjectItem(j, "problem"));
    cJSON_Delete(j);
    parent_set_hooks(NULL);
}

// The camera's format: \ ; , : " in a name or a password are escaped, or the phone reads a different network.
TEST(test_parent_wifi_code)
{
    char c[64];
    CHECK(parent_wifi_code(c, sizeof c, "a;b\\c", "p:\"q,1") > 0);
    CHECK_EQ_STR(c, "WIFI:T:WPA;S:a\\;b\\\\c;P:p\\:\\\"q\\,1;;");
    CHECK_EQ_INT(parent_wifi_code(c, sizeof c, "gugu-7A3F", "40917263"), (int)strlen(WIFI_CODE));
    CHECK_EQ_STR(c, WIFI_CODE);
    CHECK_EQ_INT(parent_wifi_code(c, 20, "gugu-7A3F", "40917263"), 0);   // too long for the room: none, not a cut one
    CHECK_EQ_STR(c, "");
}

static bool has(const char *out, const char *what) { return out && strstr(out, what); }

// The Wi-Fi password and the keys open only to a phone on the hotspot in pairing mode (2026-11-11: anyone on the home
// network could read them all, and write them). The hotspot's password is on the device's screen, so being on it means
// having Gugu in hand. The rest of the page stays open on the home network.
TEST(test_parent_secrets_locked)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    parent_set_hooks(NULL);
    store_secret_set("wifi_ssid", "HomeNet"); store_secret_set("wifi_pass", "home-pass-9");
    char last_join[40]; snprintf(last_join, sizeof last_join, "%s", net_host_last_join());
    const char *method[] = { "GET", "POST", "POST" }, *path[] = { "/api/secrets", "/api/secret", "/api/wifi/join" };
    const char *body[] = { NULL, "{\"key\":\"wifi_pass\",\"value\":\"stolen-pass\"}", "{\"ssid\":\"EvilNet\",\"pass\":\"evilpass1\"}" };
    char *out = NULL;
    for (int from_ap = 0; from_ap < 2; from_ap++)                      // pairing mode off: locked, from anywhere
        for (int r = 0; r < 3; r++) {
            CHECK_EQ_INT(call(method[r], path[r], body[r], from_ap, &out), 403);
            CHECK(has(out, "\"locked\"") && !has(out, "home-pass-9"));
            free(out);
        }
    CHECK_EQ_INT(call("GET", "/api/status", NULL, false, &out), 200);
    CHECK(has(out, "\"secrets_open\":false") && has(out, "\"pairing\":false"));
    free(out);

    CHECK_EQ_INT(parent_enter(), ESP_OK);                              // pairing mode on, but from the home network: locked
    for (int r = 0; r < 3; r++) {
        CHECK_EQ_INT(call(method[r], path[r], body[r], false, &out), 403);
        CHECK(!has(out, "home-pass-9"));
        free(out);
    }
    CHECK(secret_is("wifi_pass", "home-pass-9") && secret_is("wifi_ssid", "HomeNet"));   // nothing written
    CHECK_EQ_STR(net_host_last_join(), last_join);                                      // nothing tried
    CHECK_EQ_INT(call("GET", "/api/status", NULL, false, &out), 200);
    CHECK(has(out, "\"secrets_open\":false") && has(out, "\"pairing\":true"));
    free(out);
    CHECK_EQ_INT(call("GET", "/api/settings", NULL, false, NULL), 200);  // the rest of the page works at home
    CHECK_EQ_INT(call("GET", "/api/events", NULL, false, NULL), 200);

    CHECK_EQ_INT(call("GET", "/api/status", NULL, true, &out), 200);     // from the hotspot: open
    CHECK(has(out, "\"secrets_open\":true"));
    free(out);
    CHECK_EQ_INT(call("GET", "/api/secrets", NULL, true, &out), 200);
    CHECK(has(out, "\"wifi_pass\":\"home-pass-9\""));
    free(out);
    CHECK_EQ_INT(call("POST", "/api/secret", "{\"key\":\"wifi_pass\",\"value\":\"new-pass-12\"}", true, NULL), 200);
    CHECK(secret_is("wifi_pass", "new-pass-12"));
    CHECK_EQ_INT(call("POST", "/api/secret", "{\"key\":\"made_up\",\"value\":\"x\"}", true, NULL), 400);   // only the secrets declared
    CHECK(!secret_is("made_up", "x"));

    parent_exit();
    CHECK_EQ_INT(call("GET", "/api/secrets", NULL, true, NULL), 403);   // and shut again when pairing mode ends
    store_secret_set("wifi_ssid", ""); store_secret_set("wifi_pass", "");
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
    RUN(test_parent_pairing_card);
    RUN(test_parent_wifi_code);
    RUN(test_parent_status_problem);
    RUN(test_parent_secrets_locked);
}
