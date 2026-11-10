// parent_pair.c — what the device's screen says in pairing mode, a step at a time (2026-11-10, the owner: "需要重新配置时，
// 需要先手机找到 stopwatch 分享的热点，输密码连接，打开网页，然后再找到配置输入。能否简化？比如手机扫码直接连接到配置网页？").
// No code can both join a network and open a page, so it takes two, and the screen shows the one that is wanted now:
//   no phone on the hotspot   → a Wi-Fi code: the phone's own camera joins it with one tap (its name and password under
//                               it, for a phone that cannot scan); the captive portal then opens the page by itself
//   a phone on it, no page    → the page's address as a code, for a phone that did not open it
//   the page in use           → "carry on on the phone" (and how the last try went, if it failed)
//   a network being tried     → "Connecting...", then "Connected!", which stays
// Every change goes to the card hook in order, under this file's lock; the screen copies and returns (any task may get here,
// the Wi-Fi event task included). The password reaches the screen only: never a log line (CLAUDE.md rule 8).
#include "parent_internal.h"
#include "foundation/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define LEAVE_LINE "hold yellow 3 s to leave"

static _Atomic(mutex_t) s_lock;
static bool s_on, s_page;
static int s_phones;
static const char *s_join;                   // NULL until the page asks for a network; then trying | ok | a failure
static parent_card_fn s_card;
static void *s_card_user;

size_t parent_wifi_code(char *out, size_t cap, const char *ssid, const char *pass)
{
    // The de facto format phones' cameras read (ZXing's, adopted by iOS and Android): \ ; , : " escaped with a backslash.
    size_t n = 0;
    const char *parts[] = { "WIFI:T:WPA;S:", ssid, ";P:", pass, ";;" };
    for (int i = 0; i < 5; i++) {
        for (const char *p = parts[i]; *p; p++) {
            bool esc = (i == 1 || i == 3) && strchr("\\;,:\"", *p);
            if (n + (esc ? 2 : 1) >= cap) { if (cap) out[0] = '\0'; return 0; }
            if (esc) out[n++] = '\\';
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return n;
}

static const char *failure_line(const char *join)
{
    if (!join) return NULL;
    if (!strcmp(join, "bad_password")) return "Wrong password?";
    if (!strcmp(join, "not_found")) return "Network not found";
    if (!strcmp(join, "failed")) return "Could not connect";
    return NULL;
}

static void show(void)                       // the lock is held
{
    if (!s_card) return;
    if (!s_on) { s_card(NULL, NULL, s_card_user); return; }
    static char qr[160], text[200];
    const char *code = NULL, *fail = failure_line(s_join);
    if (s_join && !strcmp(s_join, "trying")) snprintf(text, sizeof text, "Connecting...");
    else if (s_join && !strcmp(s_join, "ok")) snprintf(text, sizeof text, "Connected!\n\n" LEAVE_LINE);
    else if (s_page) snprintf(text, sizeof text, "%s\n%s\n\n" LEAVE_LINE, fail ? fail : "Carry on", fail ? "Try again on the phone" : "on the phone");
    else if (s_phones > 0) {
        const char *url = parent_hotspot_url(), *bare = strncmp(url, "http://", 7) ? url : url + 7;
        snprintf(qr, sizeof qr, "%s", url);
        snprintf(text, sizeof text, "Phone joined. No page yet?\nScan this, or open %s\n" LEAVE_LINE, bare);
        code = qr;
    } else {
        if (parent_wifi_code(qr, sizeof qr, parent_hotspot_ssid(), parent_hotspot_password())) code = qr;
        snprintf(text, sizeof text, "%s\n%s   password %s\n" LEAVE_LINE, code ? "Scan with the phone camera" : "Join this Wi-Fi",
                 parent_hotspot_ssid(), parent_hotspot_password());
    }
    s_card(code, text, s_card_user);
}

static bool lock(void)
{
    mutex_t m = atomic_load(&s_lock);
    if (!m) return false;                    // pairing mode was never entered: nothing on screen to change
    mutex_lock(m);
    return true;
}
static void unlock(void) { mutex_unlock(atomic_load(&s_lock)); }

void pair_set_card(parent_card_fn fn, void *user)
{
    bool held = lock();
    s_card = fn; s_card_user = user;
    if (held) unlock();
}

void pair_begin(void)
{
    if (!atomic_load(&s_lock)) {             // the first entry, on the task that brings the hotspot up: nothing else is here yet
        mutex_t m = mutex_create(), none = NULL;
        if (!atomic_compare_exchange_strong(&s_lock, &none, m)) mutex_free(m);
    }
    lock();
    s_on = true; s_page = false; s_phones = 0; s_join = NULL;
    show();
    unlock();
}

void pair_end(void)
{
    if (!lock()) return;
    bool was = s_on;
    s_on = false;
    if (was) show();
    unlock();
}

void pair_phone(bool joined)
{
    if (!lock()) return;
    if (s_on) {
        s_phones += joined ? 1 : -1;
        if (s_phones < 0) s_phones = 0;
        if (!s_phones) s_page = false;       // the page went with the last phone; one that comes back is asked again
        show();
    }
    unlock();
}

void pair_page(void)
{
    if (!lock()) return;
    if (s_on && !s_page) { s_page = true; show(); }   // the page's polls come every few seconds: only the first changes anything
    unlock();
}

void pair_join(const char *state)
{
    if (!lock()) return;
    if (s_on) { s_join = state; show(); }
    unlock();
}
