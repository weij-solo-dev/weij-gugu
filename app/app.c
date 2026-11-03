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
#include <stdlib.h>
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

#define PARENT_HOLD_MS 3000   // the yellow key held this long toggles parent mode

static void toggle_parent_mode(void)
{
    if (parent_is_active()) { parent_exit(); ui_hide_notice(); LOGI("parent mode off"); return; }
    if (parent_enter() != ESP_OK) return;
    char body[160];
    snprintf(body, sizeof body, "Wi-Fi  %s\nPassword  %s\n%s\n\nhold yellow 3 s to leave", parent_hotspot_ssid(), parent_hotspot_password(), parent_hotspot_url());
    ui_show_notice("PARENT MODE", body);
}

static void on_board_event(const board_event_t *ev, void *user)
{
    (void)user;
    switch (ev->kind) {
    case BOARD_EV_KEY_DOWN: if (ev->a == BOARD_KEY_BLUE) call_key(CALL_KEY_BLUE, true, 0); break;
    case BOARD_EV_KEY_UP:
        if (ev->a == BOARD_KEY_YELLOW) { if (ev->b >= PARENT_HOLD_MS) toggle_parent_mode(); else instinct_yellow_tap(); }   // the yellow key is play, never voice
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
    instinct_set_busy(s != CALL_IDLE && s != CALL_ENDED);
}

static void on_trip_delete(void *user) { (void)user; world_trip_clear(); }   // the parent page cancels a trip

#ifdef GUGU_HOST
static void on_board_event(const board_event_t *ev, void *user);
static void scripted_taps(void *arg)   // GUGU_TAP="x,y,ms;x,y,ms": touches at those screen points after those delays, for screenshots
{
    (void)arg;
    char *list = strdup(getenv("GUGU_TAP")); int64_t t0 = clock_us();
    for (char *tok = strtok(list, ";"); tok; tok = strtok(NULL, ";")) {
        int x = 0, y = 0, ms = 0;
        bool yellow = tok[0] == 'Y' && tok[1] == '@', shake = tok[0] == 'S' && tok[1] == '@', pickup = tok[0] == 'P' && tok[1] == '@';
        if (yellow || shake || pickup) ms = atoi(tok + 2); else if (sscanf(tok, "%d,%d,%d", &x, &y, &ms) != 3) continue;
        while (clock_us() - t0 < (int64_t)ms * 1000) clock_sleep_ms(10);
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

static void load_credentials(void)
{
    char v[256];
    if (store_secret_get("minimax_api_key", v, sizeof v) == ESP_OK) provider_set_credential("minimax", "api_key", v);
    else LOGW("no MiniMax key configured");
    if (store_secret_get("qwen_api_key", v, sizeof v) == ESP_OK) { provider_set_credential("qwen", "api_key", v); provider_set_credential("qwen_realtime", "api_key", v); }
    if (store_secret_get("volc_api_key", v, sizeof v) == ESP_OK) provider_set_credential("volc", "api_key", v);
    if (store_secret_get("volc_ark_api_key", v, sizeof v) == ESP_OK) provider_set_credential("volc", "ark_api_key", v);
}

void app_main(void)
{
    LOGI("gugu boot: free internal %u B, free big %u B", (unsigned)mem_avail_internal(), (unsigned)mem_avail_big());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(ui_init());

    store_init(DATA_DIR, CONTENT_DIR);
    world_init(DATA_DIR);
    ESP_ERROR_CHECK(audio_init());
    audio_set_volume((uint8_t)store_setting_int("volume", 80));
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
    load_credentials();
    const char *asr = store_setting_str("provider_asr", "minimax"), *chat = store_setting_str("provider_chat", "minimax"), *tts = store_setting_str("provider_tts", "minimax");
#ifdef GUGU_HOST
    if (getenv("GUGU_PROVIDERS")) asr = chat = tts = getenv("GUGU_PROVIDERS");   // e.g. fake: offline stability runs
#endif
    provider_assign(PROVIDER_ROLE_ASR, asr); provider_assign(PROVIDER_ROLE_CHAT, chat); provider_assign(PROVIDER_ROLE_TTS, tts);

    mind_config_t mc = { .content_dir = CONTENT_DIR, .child_name = store_setting_str("child_name", NULL),
                         .child_age_months = store_setting_int("child_age_months", 0),
                         .family_langs = { lang_from_code(store_setting_str("lang1", "zh")), lang_from_code(store_setting_str("lang2", "yue")), lang_from_code(store_setting_str("lang3", "en")) } };
    mind_init(&mc);

    s_engine = engine_cascade_create(call_engine_event, NULL);
    call_config_t cc = { .idle_timeout_ms = (uint32_t)store_setting_int("idle_timeout_ms", 12000),
                         .max_call_ms = (uint32_t)store_setting_int("max_call_ms", 10 * 60 * 1000),
                         .lang_hint = LANG_UNKNOWN, .where = NULL };   // NULL: the mind takes "where" from the world
    ESP_ERROR_CHECK(call_init(s_engine, &cc));
    parent_init();
    parent_hooks_t ph = { .trip_delete = on_trip_delete, .memory_forget = on_memory_forget, .user = NULL };
    parent_set_hooks(&ph);
    life_init();
    call_set_state_listener(on_call_state, NULL);
    if (getenv("GUGU_YELLOW_HOLD")) { board_event_t ev = { .kind = BOARD_EV_KEY_UP, .a = BOARD_KEY_YELLOW, .b = PARENT_HOLD_MS }; on_board_event(&ev, NULL); }   // host: the parent card, for a screenshot
    if (getenv("GUGU_TOD")) instinct_set_tod_override(atoi(getenv("GUGU_TOD")));   // host screenshots: 0 night, 1 sunrise, 2 day, 3 dusk
#ifdef GUGU_HOST
    if (getenv("GUGU_TAP")) task_spawn("gugu_tap", scripted_taps, NULL, 16384, 1, 0);   // host screenshots: "x,y,ms;x,y,ms" touches
#endif
    board_set_listener(on_board_event, NULL);
    console_init();
    LOGI("ready: asr=%s chat=%s tts=%s", asr, chat, tts);

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
    for (;;) {
        clock_sleep_ms(10000);
        LOGI("heartbeat: internal free %u B (min %u B), big free %u B, vbat %d mV%s, call %s",
             (unsigned)mem_avail_internal(), (unsigned)mem_min_avail_internal(), (unsigned)mem_avail_big(),
             board_battery_mv(), board_is_charging() ? " charging" : "", call_state_name(call_state()));
    }
}
