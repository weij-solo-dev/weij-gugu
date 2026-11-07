// parent_join.c — the page's "connect". The network is tried first and kept only if it works: a mistyped password must
// not cost the network that works. The worker carries on whatever happens to the phone, because joining can move the
// hotspot to the router's channel (one radio, one channel) and the phone may drop off for a moment; the device's screen
// hears the result through the same callback as the page's poll.
#define LOG_TAG "parent"
#include "parent_internal.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/task.h"
#include "net/net.h"
#include "store/store.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define JOIN_SETTLE_MS 300                     // the page's answer gets out before the hotspot may move
#define JOIN_DEADLINE_US (30 * 1000000LL)

static atomic_bool s_running;
static _Atomic(const char *) s_state = "idle";
static char s_ssid[33], s_pass[64];
static parent_join_result_fn s_on_result;
static void *s_user;

static void tell(const char *state) { if (s_on_result) s_on_result(state, s_user); }

static bool keep(const char *ssid, const char *pass)
{
    if (store_secret_set("wifi_ssid", ssid) == ESP_OK && store_secret_set("wifi_pass", pass) == ESP_OK) return true;
    LOGE("wifi join: could not keep the network");
    return false;
}

static void back_to_the_saved_network(void)
{
    char ssid[33] = "", pass[64] = "";
    store_secret_get("wifi_ssid", ssid, sizeof ssid);
    store_secret_get("wifi_pass", pass, sizeof pass);
    if (ssid[0]) net_wifi_join(ssid, pass); else net_wifi_leave();
    memset(pass, 0, sizeof pass);
}

static void join_task(void *arg)
{
    (void)arg;
    tell("trying");
    clock_sleep_ms(JOIN_SETTLE_MS);
    net_wifi_join(s_ssid, s_pass);
    net_join_t r = net_wifi_join_result();
    for (int64_t t0 = clock_us(); r == NET_JOIN_TRYING && clock_us() - t0 < JOIN_DEADLINE_US; r = net_wifi_join_result()) clock_sleep_ms(200);
    if (r == NET_JOIN_OK && !keep(s_ssid, s_pass)) r = NET_JOIN_FAILED;
    if (r != NET_JOIN_OK) back_to_the_saved_network();
    memset(s_pass, 0, sizeof s_pass);
    const char *result = r == NET_JOIN_OK ? "ok" : r == NET_JOIN_BAD_PASSWORD ? "bad_password" : r == NET_JOIN_NOT_FOUND ? "not_found" : "failed";
    LOGI("wifi join: %s", result);
    tell(result);
    atomic_store(&s_running, false);           // the page last: by the time it reads the result, another join can start
    atomic_store(&s_state, result);
}

bool parent_join_start(const char *ssid, const char *pass, parent_join_result_fn on_result, void *user)
{
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_running, &idle, true)) return false;
    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    snprintf(s_pass, sizeof s_pass, "%s", pass);
    s_on_result = on_result; s_user = user;
    atomic_store(&s_state, "trying");          // from here on: the page's first poll may come before the worker's first line
    if (task_spawn("gugu_parent_join", join_task, NULL, 6144, 1, 0)) return true;
    atomic_store(&s_state, "failed");
    atomic_store(&s_running, false);
    return true;
}

const char *parent_join_state(void) { return atomic_load(&s_state); }
