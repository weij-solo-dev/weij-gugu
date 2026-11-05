// app.c — the only assembler (CLAUDE.md rule 1). Constructs modules, wires callbacks, starts tasks.
// Same file on the device and in the simulator; only the paths and the replay hook differ.
#define LOG_TAG "app"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "board/board.h"
#include "ui/ui.h"
#include "net/net.h"
#include "store/store.h"
#include "world/world.h"
#include "provider/provider.h"
#include "mind/mind.h"
#include "audio/audio.h"
#include "engine/engine.h"
#include "call/call.h"
#include "console/console.h"
#include "instinct/instinct.h"
#include "life/life.h"
#include "parent/parent.h"
#include "foundation/task.h"
#include "foundation/atomic_file.h"
#include "foundation/logcap.h"
#include "foundation/strbuf.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef GUGU_HOST
#define CONTENT_DIR GUGU_REPO_ROOT "/content"
#define DATA_DIR    GUGU_REPO_ROOT "/host/data"
#else
#define CONTENT_DIR "/content"
#define DATA_DIR    "/data"
#endif

static engine_t *s_engine;
static atomic_bool s_service_ok = true, s_check_due = true;   // the chat vendor's health (see on_service_checked)
static lang_t s_lang = LANG_ZH;                               // the interaction language (pairing page: settings "lang")
static enum { HELLO_IDLE, HELLO_MAKING, HELLO_OFFERED } s_hello_state;
static int64_t s_hello_retry;
static char s_service_why[120];
static int64_t s_last_check;
static bool can_dial(char *why, size_t cap);
static instinct_ready_t readiness(char *why, size_t cap);

// Pairing mode (匹配模式): the yellow key held 3 s brings the hotspot and its page up, held 3 s again takes them down.
static void toggle_pairing_mode(void)
{
    if (parent_is_active()) { parent_exit(); ui_hide_notice(); LOGI("pairing mode off"); store_event("pairing off"); return; }
    if (parent_enter() != ESP_OK) { store_event("pairing failed"); return; }
    store_event("pairing on: %s", parent_hotspot_ssid());
    char body[160];
    snprintf(body, sizeof body, "Wi-Fi  %s\nPassword  %s\n%s\n\nhold yellow 3 s to leave", parent_hotspot_ssid(), parent_hotspot_password(), parent_hotspot_url());
    ui_show_notice("PAIRING MODE", body);
}

static void on_board_event(const board_event_t *ev, void *user)
{
    (void)user;
    switch (ev->kind) {
    case BOARD_EV_KEY_DOWN:
        store_event("key %s down", ev->a == BOARD_KEY_BLUE ? "blue" : "yellow");
        if (ev->a == BOARD_KEY_BLUE) {
            instinct_hush();                       // a line from the bubble stops: the child wants to talk
            char why[64];
            if (call_state() == CALL_IDLE && !can_dial(why, sizeof why)) {
                store_event("call refused: %s", why); instinct_refuse(why);
                if (readiness(why, sizeof why) == INSTINCT_SERVICE_DOWN) atomic_store(&s_check_due, true);   // the press is a retry: check again now
                break;
            }
            call_key(CALL_KEY_BLUE, true, 0);
        }
        break;
    case BOARD_EV_KEY_HOLD:
        store_event("key %s held %d ms", ev->a == BOARD_KEY_BLUE ? "blue" : "yellow", ev->b);
        if (ev->a == BOARD_KEY_YELLOW) toggle_pairing_mode();      // fires while the key is still down
        break;
    case BOARD_EV_KEY_UP:
        store_event("key %s up after %d ms", ev->a == BOARD_KEY_BLUE ? "blue" : "yellow", ev->b);
        if (ev->a == BOARD_KEY_YELLOW) { if (ev->b < BOARD_HOLD_MS) instinct_yellow_tap(); }   // the yellow key is play, never voice; a long press already did its job
        else call_key(CALL_KEY_BLUE, false, ev->b);
        break;
    case BOARD_EV_TOUCH_DOWN: instinct_touched(ev->a, ev->b); break;
    case BOARD_EV_SHAKE: instinct_shaken(ev->a); break;
    case BOARD_EV_PICKUP: instinct_picked_up(); break;
    default: break;
    }
}

static void on_call_state(call_state_t s, void *user)
{
    (void)user;
    store_event("call %s", call_state_name(s));
    instinct_set_line_open(s != CALL_IDLE && s != CALL_ENDED);   // the 12 s after an answer included: no bubbles
    instinct_set_busy(s == CALL_CONNECTING || s == CALL_LISTENING || s == CALL_THINKING || s == CALL_SPEAKING);   // a turn: play waits
}

static void on_trip_delete(void *user) { (void)user; world_trip_clear(); }   // the parent page cancels a trip
// A failed or late turn puts the service in doubt at once: the bubble shows it (⚠, or the Wi-Fi icon when the network
// went) until a check, or the next answer, says it is fine. The check asks the role that failed besides the chat.
static atomic_uint s_check_roles = LIFE_CHECK_CHAT;
static void on_call_outcome(bool ok, const char *stage, const char *why, void *user)
{
    (void)user;
    if (ok) {
        if (!atomic_exchange(&s_service_ok, true)) store_event("service back: a turn was answered");
        atomic_store(&s_check_roles, LIFE_CHECK_CHAT); atomic_store(&s_check_due, false);
        return;
    }
    unsigned roles = !strcmp(stage, "chat") ? 0 : !strcmp(stage, "tts") ? LIFE_CHECK_TTS : !strcmp(stage, "asr") ? LIFE_CHECK_ASR : LIFE_CHECK_TTS | LIFE_CHECK_ASR;
    atomic_fetch_or(&s_check_roles, roles);
    snprintf(s_service_why, sizeof s_service_why, "%s", why);
    if (atomic_exchange(&s_service_ok, false)) store_event("service in doubt: %s", why);
    atomic_store(&s_check_due, true);
    instinct_refuse(why);                          // a buzz, a head shake, and the bubble
}
static instinct_ready_t ready_probe(char *why, size_t cap, void *user) { (void)user; return readiness(why, cap); }   // the bubble and its icon
static void on_bubble_tap(void *user) { (void)user; if (!parent_is_active()) { store_event("bubble tapped"); toggle_pairing_mode(); } }   // the bubble opens pairing mode

// The chat vendor's health, from the tiny check in life: down after a failed check, up after a good one. Checked once
// online after boot, after the page saves settings, after a failed turn or a refused press while down, and every
// minute while down.
static void on_service_checked(bool ok, int kind, const char *why, void *user)
{
    (void)user; (void)kind;
    snprintf(s_service_why, sizeof s_service_why, "%s", why);
    if (ok) atomic_store(&s_check_roles, LIFE_CHECK_CHAT);   // what a failed turn added was asked and answered
    if (atomic_exchange(&s_service_ok, ok) != ok) store_event("service %s: %s", ok ? "back" : "trouble", why);
}
static void on_test_sound(void *user) { (void)user; store_event("speaker test from the page"); audio_sfx_play("coo"); ui_play_action(UI_ACT_WAVE); ui_set_emotion(UI_EMO_HAPPY); }
static void on_volume(int percent, void *user) { (void)user; audio_set_volume((uint8_t)percent); store_event("volume %d from the page", percent); }
static int on_test_audio(char *json, size_t cap, void *user)
{
    (void)user;
    audio_selftest_t t; esp_err_t e = audio_selftest(&t);
    if (e != ESP_OK) return snprintf(json, cap, "{\"error\":\"%s\"}", e == ESP_ERR_INVALID_STATE ? "a call is using the microphone" : esp_err_to_name(e));
    store_event("audio test: volume %u, quiet peak %d over %u ms, beep peak %d over %u ms", t.volume, t.ambient_peak, (unsigned)t.ambient_ms, t.tone_peak, (unsigned)t.tone_ms);
    return snprintf(json, cap, "{\"volume\":%u,\"ambient_ms\":%u,\"ambient_peak\":%d,\"ambient_rms\":%d,\"tone_ms\":%u,\"tone_peak\":%d,\"tone_rms\":%d,\"sfx\":\"%s\"}",
                    t.volume, (unsigned)t.ambient_ms, t.ambient_peak, t.ambient_rms, (unsigned)t.tone_ms, t.tone_peak, t.tone_rms, esp_err_to_name(t.sfx));
}
static void configure_providers(void);
static bool keys_ok(char *why, size_t cap);

// ---- the pairing page's two tests: do the vendors answer, and how does a voice sound (each waits for life's worker) ----
static signal_t s_page_done;
static struct { bool ok; char why[160], path[160]; } s_page;
static int page_answer(char *json, size_t cap, bool ok, const char *why)
{
    strbuf_t b; strbuf_init(&b);
    strbuf_printf(&b, "{\"ok\":%s,\"why\":\"", ok ? "true" : "false"); strbuf_append_json_escaped(&b, why ? why : "");
    strbuf_printf(&b, "\",\"asr\":\"%s\",\"chat\":\"%s\",\"tts\":\"%s\"}", provider_for(PROVIDER_ROLE_ASR)->id, provider_for(PROVIDER_ROLE_CHAT)->id, provider_for(PROVIDER_ROLE_TTS)->id);
    int n = snprintf(json, cap, "%s", b.s ? b.s : "{}"); strbuf_free(&b);
    return n;
}
static bool page_busy(char *why, size_t cap)
{
    if (!net_is_online()) { snprintf(why, cap, "the device is not online"); return true; }
    if (call_state() != CALL_IDLE) { snprintf(why, cap, "Gugu is on a call; try again in a moment"); return true; }
    if (!s_page_done) s_page_done = signal_create();
    while (signal_wait(s_page_done, 0)) { }                   // a late answer from an earlier test
    return false;
}
static void on_page_checked(bool ok, int kind, const char *why, void *user)
{
    on_service_checked(ok, kind, why, user);                 // the bubble follows the page's test too
    s_page.ok = ok; snprintf(s_page.why, sizeof s_page.why, "%s", why);
    signal_set(s_page_done);
}
static int on_page_check(char *json, size_t cap, void *user)
{
    (void)user;
    char why[160];
    if (!keys_ok(why, sizeof why) || page_busy(why, sizeof why)) return page_answer(json, cap, false, why);
    s_last_check = clock_us();
    life_check_service(LIFE_CHECK_CHAT | LIFE_CHECK_TTS | LIFE_CHECK_ASR, on_page_checked, NULL);
    if (!signal_wait(s_page_done, 45000)) return page_answer(json, cap, false, "no answer within 45 s");
    store_event("vendors tested from the page: %s (%s)", s_page.ok ? "ok" : "failed", s_page.why);
    return page_answer(json, cap, s_page.ok, s_page.why);
}
static void on_page_line(const life_line_t *L, void *user)
{
    (void)user;
    s_page.ok = L->ok; snprintf(s_page.why, sizeof s_page.why, "%s", L->why); snprintf(s_page.path, sizeof s_page.path, "%s", L->path);
    signal_set(s_page_done);
}
static int on_voice_test(const char *lang, const char *voice, char *json, size_t cap, void *user)
{
    (void)user;
    char why[160];
    if (!provider_role_ready(PROVIDER_ROLE_TTS, why, sizeof why) || page_busy(why, sizeof why)) return page_answer(json, cap, false, why);
    lang_t l = lang_from_code(lang);
    life_voice_sample(l == LANG_UNKNOWN ? s_lang : l, voice, on_page_line, NULL);
    if (!signal_wait(s_page_done, 30000)) return page_answer(json, cap, false, "no voice within 30 s");
    store_event("voice tried from the page: %s %s: %s", lang, voice && *voice ? voice : "(default)", s_page.ok ? "ok" : s_page.why);
    if (s_page.ok) { instinct_hush(); audio_play_file(s_page.path); }
    return page_answer(json, cap, s_page.ok, s_page.why);
}

static void on_apply(bool wifi_changed, void *user)   // the page saved secrets: keys take effect now, Wi-Fi joins without a reboot
{
    (void)user;
    configure_providers();                         // vendors per role, regions, voices, keys: in use from the next request
    char ssid[64], pass[64];
    if (wifi_changed && store_secret_get("wifi_ssid", ssid, sizeof ssid) == ESP_OK && ssid[0]) {
        if (store_secret_get("wifi_pass", pass, sizeof pass) != ESP_OK) pass[0] = '\0';
        net_wifi_join(ssid, pass);
    }
    store_event("settings applied from the page (wifi %s)", wifi_changed ? "rejoined" : "unchanged");
    atomic_store(&s_check_due, true);              // new keys: is the service answering now?
    lang_t l = lang_from_code(store_setting_str("lang", "zh"));
    if (l == LANG_UNKNOWN) l = LANG_ZH;
    if (l != s_lang) {                             // a new interaction language: answers, the voice, and a new self-introduction
        s_lang = l; mind_set_language(l); call_set_lang(l);
        s_hello_state = HELLO_IDLE; s_hello_retry = 0;
        store_event("interaction language: %s", lang_code(l));
    }
    mind_set_child(store_setting_str("child_name", NULL), store_setting_int("child_age_months", 0));
    audio_set_volume((uint8_t)store_setting_int("volume", AUDIO_VOLUME_DEFAULT));   // saved with the settings: heard now, not after a reboot
}

// ---- the friendly bubbles: life makes the lines, instinct shows and plays them ----
static void on_line(const life_line_t *L, void *user)   // on life's worker
{
    instinct_offer_t kind = (instinct_offer_t)(intptr_t)user;
    if (!L->ok) {
        store_event("line %s failed: %s", kind == INSTINCT_OFFER_HELLO ? "hello" : "idea", L->why);
        if (kind == INSTINCT_OFFER_HELLO) { s_hello_state = HELLO_IDLE; s_hello_retry = clock_us() + 60 * 1000000LL; }
        return;
    }
    instinct_offer(kind, L->path, L->text, L->question);
    if (kind == INSTINCT_OFFER_HELLO) s_hello_state = HELLO_OFFERED;
}
static void on_want_idea(void *user)   // instinct: time for a light bulb
{
    (void)user;
    char why[96];
    if (readiness(why, sizeof why) == INSTINCT_READY) life_prepare_idea(s_lang, on_line, (void *)(intptr_t)INSTINCT_OFFER_IDEA);
}
static void on_heard(instinct_offer_t kind, const char *text, bool question, void *user)   // the child tapped and heard it
{
    (void)user;
    if (kind == INSTINCT_OFFER_HELLO) { store_setting_set_str("hello_lang", lang_code(s_lang)); s_hello_state = HELLO_IDLE; }
    if (question) mind_set_asked(text, 3 * 60 * 1000);   // the next three minutes of calls know what the child is answering
    char said[100]; snprintf(said, sizeof said, "%s", text); mind_utf8_cut(said, 80);
    store_event("bubble heard (%s): %s", kind == INSTINCT_OFFER_HELLO ? "hello" : question ? "question" : "share", said);
}
static void hello_tick(void)   // once a second: the self-introduction, the first time everything works in this language
{
    char why[96];
    if (s_hello_state != HELLO_IDLE || clock_us() < s_hello_retry) return;
    if (!strcmp(store_setting_str("hello_lang", ""), lang_code(s_lang))) return;
    if (readiness(why, sizeof why) != INSTINCT_READY || call_state() != CALL_IDLE) return;
    s_hello_state = HELLO_MAKING;
    life_prepare_hello(s_lang, on_line, (void *)(intptr_t)INSTINCT_OFFER_HELLO);
}

// Can a call start right now, and if not, why: setup unfinished (no Wi-Fi configured, a vendor or key missing), Wi-Fi
// down (configured, not joined), or the service down (the last check failed). Checked before dialling so the child
// gets an answer at once instead of a beep and twenty seconds of silence; the reason picks the bubble's icon.
static bool keys_ok(char *why, size_t cap)   // every vendor a call needs can sign its requests
{
    return provider_role_ready(PROVIDER_ROLE_CHAT, why, cap) && provider_role_ready(PROVIDER_ROLE_ASR, why, cap) &&
           provider_role_ready(PROVIDER_ROLE_TTS, why, cap);
}

static instinct_ready_t readiness(char *why, size_t cap)
{
    if (!keys_ok(why, cap)) return INSTINCT_NEED_SETUP;
    if (!net_is_online()) {
        char ssid[64];
        if (store_secret_get("wifi_ssid", ssid, sizeof ssid) != ESP_OK || !ssid[0]) { snprintf(why, cap, "no wifi configured"); return INSTINCT_NEED_SETUP; }
        snprintf(why, cap, "offline"); return INSTINCT_WIFI_DOWN;
    }
    if (!atomic_load(&s_service_ok)) { snprintf(why, cap, "service: %s", s_service_why); return INSTINCT_SERVICE_DOWN; }
    return INSTINCT_READY;
}

static bool can_dial(char *why, size_t cap) { return readiness(why, cap) == INSTINCT_READY; }

static void service_tick(void)   // once a second from the main loop: start a check when one is due
{
    char why[96];
    if (!net_is_online() || !keys_ok(why, sizeof why) || call_state() != CALL_IDLE) return;
    int64_t now = clock_us();
    bool due = atomic_load(&s_check_due) || (!atomic_load(&s_service_ok) && now - s_last_check > 60 * 1000000LL);
    if (!due || now - s_last_check < 5 * 1000000LL) return;   // not twice within 5 s
    atomic_store(&s_check_due, false); s_last_check = now;
    life_check_service(atomic_load(&s_check_roles) | LIFE_CHECK_CHAT, on_service_checked, NULL);
}

#ifdef GUGU_HOST
static void on_board_event(const board_event_t *ev, void *user);
static void scripted_taps(void *arg)   // GUGU_TAP="x,y,ms;x,y,ms": touches at those screen points after those delays, for screenshots
{
    (void)arg;
    char *list = strdup(getenv("GUGU_TAP")); int64_t t0 = clock_us();
    for (char *tok = strtok(list, ";"); tok; tok = strtok(NULL, ";")) {
        int x = 0, y = 0, ms = 0;
        bool yellow = tok[0] == 'Y' && tok[1] == '@', shake = tok[0] == 'S' && tok[1] == '@', pickup = tok[0] == 'P' && tok[1] == '@', blue = tok[0] == 'B' && tok[1] == '@';
        if (yellow || shake || pickup || blue) ms = atoi(tok + 2); else if (sscanf(tok, "%d,%d,%d", &x, &y, &ms) != 3) continue;
        while (clock_us() - t0 < (int64_t)ms * 1000) clock_sleep_ms(10);
        if (blue) {   // a one-second press of the phone key
            board_event_t d = { .kind = BOARD_EV_KEY_DOWN, .a = BOARD_KEY_BLUE }; on_board_event(&d, NULL);
            clock_sleep_ms(1000);
            board_event_t u = { .kind = BOARD_EV_KEY_UP, .a = BOARD_KEY_BLUE, .b = 1000 }; on_board_event(&u, NULL);
            continue;
        }
        board_event_t ev = yellow ? (board_event_t){ .kind = BOARD_EV_KEY_UP, .a = BOARD_KEY_YELLOW, .b = 120 } : shake ? (board_event_t){ .kind = BOARD_EV_SHAKE, .a = 160 }
                        : pickup ? (board_event_t){ .kind = BOARD_EV_PICKUP } : (board_event_t){ .kind = BOARD_EV_TOUCH_DOWN, .a = x, .b = y };
        on_board_event(&ev, NULL);
    }
    free(list);
}
#endif
static void on_memory_forget(const char *book, const char *key, void *user)  // the parent page deletes a memory
{
    (void)user;
    if (!strcmp(book, "journey")) { mind_journey_forget(key); world_trip_forget(key); }
    else mind_book_forget(book, atoi(key));
}

static void configure_providers(void)
{
    provider_configure();
#ifdef GUGU_HOST
    const char *all = getenv("GUGU_PROVIDERS");                 // e.g. fake: offline stability runs
    if (all) { provider_assign(PROVIDER_ROLE_ASR, all); provider_assign(PROVIDER_ROLE_CHAT, all); provider_assign(PROVIDER_ROLE_TTS, all); }
#endif
}

void app_main(void)
{
    logcap_init(64 * 1024);                      // every log line from here on is also kept for the pairing page's export
    LOGI("gugu boot: free internal %u B, free big %u B", (unsigned)mem_avail_internal(), (unsigned)mem_avail_big());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(ui_init());

    store_init(DATA_DIR, CONTENT_DIR);
    world_init(DATA_DIR);
    esp_err_t audio_err = audio_init();          // a silent device is still a device: keep going, record it
    if (audio_err != ESP_OK) LOGE("audio init failed: %s", esp_err_to_name(audio_err));
    audio_set_volume((uint8_t)store_setting_int("volume", AUDIO_VOLUME_DEFAULT));
    instinct_init();                       // the right scene is up before the network comes

    net_init();
    net_time_sync(store_setting_str("ntp_server", "ntp.aliyun.com"));
    {
        char ssid[64], pass[64];
        if (store_secret_get("wifi_ssid", ssid, sizeof ssid) == ESP_OK) {
            if (store_secret_get("wifi_pass", pass, sizeof pass) != ESP_OK) pass[0] = 0;
            net_wifi_connect(ssid, pass, 15000);
        } else LOGW("no Wi-Fi configured (console: wifi <ssid> <pass>)");
    }
    char presets[200]; snprintf(presets, sizeof presets, "%s/providers", CONTENT_DIR);
    provider_init(presets);
    configure_providers();
    instinct_set_ready_probe(ready_probe, NULL);   // vendors are known from here on: instinct may ask whether a call could start
    instinct_set_bubble_cb(on_bubble_tap, NULL);

    s_lang = lang_from_code(store_setting_str("lang", "zh"));
    if (s_lang == LANG_UNKNOWN) s_lang = LANG_ZH;
    mind_config_t mc = { .content_dir = CONTENT_DIR, .child_name = store_setting_str("child_name", NULL),
                         .child_age_months = store_setting_int("child_age_months", 0),
                         .family_langs = { s_lang, LANG_UNKNOWN, LANG_UNKNOWN } };   // one interaction language (pairing page)
    mind_init(&mc);

    s_engine = engine_cascade_create(call_engine_event, NULL);
    call_config_t cc = { .idle_timeout_ms = (uint32_t)store_setting_int("idle_timeout_ms", 12000),
                         .max_call_ms = (uint32_t)store_setting_int("max_call_ms", 10 * 60 * 1000),
                         .lang_hint = s_lang, .where = NULL,           // NULL: the mind takes "where" from the world
                         .answer_deadline_ms = (uint32_t)store_setting_int("answer_deadline_ms", 15000), .on_outcome = on_call_outcome };
    ESP_ERROR_CHECK(call_init(s_engine, &cc));
    parent_init();
    parent_hooks_t ph = { .trip_delete = on_trip_delete, .memory_forget = on_memory_forget, .test_sound = on_test_sound, .volume = on_volume, .test_audio = on_test_audio, .apply = on_apply,
                          .check = on_page_check, .voice_test = on_voice_test, .user = NULL };
    parent_set_hooks(&ph);
    life_init();
    {
        instinct_offer_cbs_t oc = { .want_idea = on_want_idea, .heard = on_heard, .user = NULL };
        instinct_set_offer_cbs(&oc);
    }
#ifdef GUGU_HOST
    if (getenv("GUGU_BUBBLE")) {                   // host screenshots: a friendly bubble right away (hello | idea)
        bool hello = !strcmp(getenv("GUGU_BUBBLE"), "hello");
        if (hello) s_hello_state = HELLO_MAKING;
        (hello ? life_prepare_hello : life_prepare_idea)(s_lang, on_line, (void *)(intptr_t)(hello ? INSTINCT_OFFER_HELLO : INSTINCT_OFFER_IDEA));
    }
#endif
    call_set_state_listener(on_call_state, NULL);
    if (getenv("GUGU_YELLOW_HOLD")) { board_event_t ev = { .kind = BOARD_EV_KEY_HOLD, .a = BOARD_KEY_YELLOW, .b = BOARD_HOLD_MS }; on_board_event(&ev, NULL); }   // host: the pairing card, for a screenshot
    if (getenv("GUGU_TOD")) instinct_set_tod_override(atoi(getenv("GUGU_TOD")));   // host screenshots: 0 night, 1 sunrise, 2 day, 3 dusk
    if (getenv("GUGU_IDLE")) instinct_idle_now(getenv("GUGU_IDLE"));   // host screenshots: that idle bit at once
    if (getenv("GUGU_FEED")) {                     // host screenshots: the yellow key drops this item, item[:plan]
        char item[24]; snprintf(item, sizeof item, "%s", getenv("GUGU_FEED"));
        char *colon = strchr(item, ':'); int plan = colon ? atoi(colon + 1) : -1; if (colon) *colon = '\0';
        instinct_feed_override(item, plan);
    }
    if (getenv("GUGU_READY")) {                    // host screenshots: the bubble's reason (yes, setup, wifi, service)
        const char *r = getenv("GUGU_READY");
        instinct_set_ready_override(!strcmp(r, "yes") ? INSTINCT_READY : !strcmp(r, "wifi") ? INSTINCT_WIFI_DOWN : !strcmp(r, "service") ? INSTINCT_SERVICE_DOWN : INSTINCT_NEED_SETUP);
    }
#ifdef GUGU_HOST
    if (getenv("GUGU_TAP")) task_spawn("gugu_tap", scripted_taps, NULL, 16384, 1, 0);   // host screenshots: "x,y,ms;x,y,ms" touches
#endif
    board_set_listener(on_board_event, NULL);
    console_init();
    LOGI("ready: asr=%s chat=%s tts=%s image=%s", provider_for(PROVIDER_ROLE_ASR)->id, provider_for(PROVIDER_ROLE_CHAT)->id,
         provider_for(PROVIDER_ROLE_TTS)->id, provider_for(PROVIDER_ROLE_IMAGE)->id);
    {   // what the pairing page and a report need to know about this boot
        char v[256]; bool have_key = keys_ok(v, sizeof v), have_wifi = store_secret_get("wifi_ssid", v, sizeof v) == ESP_OK && v[0];   // keys: every vendor a call needs has one
        char boot[720], path[200], fw[16], crash[200] = "", bt[200] = "";
        board_firmware_id(fw, sizeof fw);
        bool crashed = board_crash_report(crash, sizeof crash, bt, sizeof bt);   // the last run's crash, if a core dump says so
        snprintf(boot, sizeof boot, "{\"version\":\"v1.1 2026-11-05\",\"firmware\":\"%s\",\"reset_reason\":\"%s\",\"audio\":%s,\"wifi_configured\":%s,\"key_configured\":%s,\"volume\":%d%s%s%s%s%s}",
                 fw, board_reset_reason(), audio_err == ESP_OK ? "true" : "false", have_wifi ? "true" : "false", have_key ? "true" : "false", store_setting_int("volume", AUDIO_VOLUME_DEFAULT),
                 crashed ? ",\"crash\":\"" : "", crashed ? crash : "", crashed ? "\",\"backtrace\":\"" : "", crashed ? bt : "", crashed ? "\"" : "");
        snprintf(path, sizeof path, "%s/logs/boot.json", DATA_DIR);
        atomic_file_write(path, boot, strlen(boot));
        store_event("boot (%s): firmware %s, audio %s, wifi %s, key %s, online %s", board_reset_reason(), fw, audio_err == ESP_OK ? "ok" : "FAILED", have_wifi ? "set" : "unset", have_key ? "set" : "unset", net_is_online() ? "yes" : "no");
        if (crashed) { store_event("crash: %s", crash); store_event("crash backtrace: %s", bt); LOGE("the last run crashed: %s | %s", crash, bt); }
        store_logs_flush();                      // the boot's log reaches flash at once: a crash a minute later must not take it along
    }

#ifdef GUGU_HOST
    const char *replay = getenv("GUGU_REPLAY_WAV"), *replay_list = getenv("GUGU_REPLAY_WAVS");
    if (replay || replay_list) {
        clock_sleep_ms(300);
        int turns = atoi(getenv("GUGU_REPLAY_TURNS") ? getenv("GUGU_REPLAY_TURNS") : "1");
        int ok = 0, total = 0;
        if (replay_list) {                    // several utterances in one call, e.g. zh_01.wav,zh_02.wav
            char *list = strdup(replay_list);
            for (char *f = strtok(list, ","); f; f = strtok(NULL, ",")) {
                total++;
                esp_err_t r = call_replay_wav(f, 60000);
                LOGI("replay %s: %s", f, esp_err_to_name(r));
                if (r == ESP_OK) ok++;
            }
            free(list);
        } else {
            for (int i = 0; i < turns; i++) { total++; esp_err_t r = call_replay_wav(replay, 60000); LOGI("replay turn %d: %s", i + 1, esp_err_to_name(r)); if (r == ESP_OK) ok++; }
        }
        clock_sleep_ms(500);
        call_hangup();
        LOGI("REPLAY DONE %d/%d", ok, total);
        fflush(stderr);
        exit(ok == total ? 0 : 2);
    }
#endif
    for (unsigned sec = 1;; sec++) {
        clock_sleep_ms(1000);
        service_tick();
        hello_tick();
        if (sec % 10) continue;
        if (sec % 30 == 0) store_logs_flush();   // the captured log reaches flash every 30 s
        LOGI("heartbeat: internal free %u B (min %u B), big free %u B, vbat %d mV%s, call %s",
             (unsigned)mem_avail_internal(), (unsigned)mem_min_avail_internal(), (unsigned)mem_avail_big(),
             board_battery_mv(), board_is_charging() ? " charging" : "", call_state_name(call_state()));
    }
}
