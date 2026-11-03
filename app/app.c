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
static bool can_dial(char *why, size_t cap);

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
            char why[64];
            if (call_state() == CALL_IDLE && !can_dial(why, sizeof why)) { store_event("call refused: %s", why); instinct_refuse(why); break; }
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
    instinct_set_busy(s != CALL_IDLE && s != CALL_ENDED);
}

static void on_trip_delete(void *user) { (void)user; world_trip_clear(); }   // the parent page cancels a trip
static void on_test_sound(void *user) { (void)user; store_event("speaker test from the page"); audio_sfx_play("coo"); ui_play_action(UI_ACT_WAVE); ui_set_emotion(UI_EMO_HAPPY); }
static int on_test_audio(char *json, size_t cap, void *user)
{
    (void)user;
    audio_selftest_t t; esp_err_t e = audio_selftest(&t);
    if (e != ESP_OK) return snprintf(json, cap, "{\"error\":\"%s\"}", e == ESP_ERR_INVALID_STATE ? "a call is using the microphone" : esp_err_to_name(e));
    store_event("audio test: volume %u, quiet peak %d over %u ms, beep peak %d over %u ms", t.volume, t.ambient_peak, (unsigned)t.ambient_ms, t.tone_peak, (unsigned)t.tone_ms);
    return snprintf(json, cap, "{\"volume\":%u,\"ambient_ms\":%u,\"ambient_peak\":%d,\"ambient_rms\":%d,\"tone_ms\":%u,\"tone_peak\":%d,\"tone_rms\":%d,\"sfx\":\"%s\"}",
                    t.volume, (unsigned)t.ambient_ms, t.ambient_peak, t.ambient_rms, (unsigned)t.tone_ms, t.tone_peak, t.tone_rms, esp_err_to_name(t.sfx));
}
static void load_credentials(void);
static void on_apply(bool wifi_changed, void *user)   // the page saved secrets: keys take effect now, Wi-Fi joins without a reboot
{
    (void)user;
    load_credentials();
    char ssid[64], pass[64];
    if (wifi_changed && store_secret_get("wifi_ssid", ssid, sizeof ssid) == ESP_OK && ssid[0]) {
        if (store_secret_get("wifi_pass", pass, sizeof pass) != ESP_OK) pass[0] = '\0';
        net_wifi_join(ssid, pass);
    }
    store_event("settings applied from the page (wifi %s)", wifi_changed ? "rejoined" : "unchanged");
}

// Can a call start right now? Checked before dialling so the child gets an answer at once instead of a dial tone and twenty seconds of silence.
static bool can_dial(char *why, size_t cap)
{
    if (!net_is_online()) { snprintf(why, cap, "offline"); return false; }
    const char *roles[] = { "chat", "asr", "tts" }; provider_role_t ids[] = { PROVIDER_ROLE_CHAT, PROVIDER_ROLE_ASR, PROVIDER_ROLE_TTS };
    for (int i = 0; i < 3; i++) {
        const provider_vtable_t *v = provider_for(ids[i]);
        if (!v) { snprintf(why, cap, "no %s vendor", roles[i]); return false; }
        if (strcmp(v->id, "fake")) { const char *k = provider_credential(v->id, "api_key"); if (!k || !*k) { snprintf(why, cap, "no %s key for %s", roles[i], v->id); return false; } }
    }
    return true;
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
    logcap_init(64 * 1024);                      // every log line from here on is also kept for the pairing page's export
    LOGI("gugu boot: free internal %u B, free big %u B", (unsigned)mem_avail_internal(), (unsigned)mem_avail_big());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(ui_init());

    store_init(DATA_DIR, CONTENT_DIR);
    world_init(DATA_DIR);
    esp_err_t audio_err = audio_init();          // a silent device is still a device: keep going, record it
    if (audio_err != ESP_OK) LOGE("audio init failed: %s", esp_err_to_name(audio_err));
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
    parent_hooks_t ph = { .trip_delete = on_trip_delete, .memory_forget = on_memory_forget, .test_sound = on_test_sound, .test_audio = on_test_audio, .apply = on_apply, .user = NULL };
    parent_set_hooks(&ph);
    life_init();
    call_set_state_listener(on_call_state, NULL);
    if (getenv("GUGU_YELLOW_HOLD")) { board_event_t ev = { .kind = BOARD_EV_KEY_HOLD, .a = BOARD_KEY_YELLOW, .b = BOARD_HOLD_MS }; on_board_event(&ev, NULL); }   // host: the pairing card, for a screenshot
    if (getenv("GUGU_TOD")) instinct_set_tod_override(atoi(getenv("GUGU_TOD")));   // host screenshots: 0 night, 1 sunrise, 2 day, 3 dusk
#ifdef GUGU_HOST
    if (getenv("GUGU_TAP")) task_spawn("gugu_tap", scripted_taps, NULL, 16384, 1, 0);   // host screenshots: "x,y,ms;x,y,ms" touches
#endif
    board_set_listener(on_board_event, NULL);
    console_init();
    LOGI("ready: asr=%s chat=%s tts=%s", asr, chat, tts);
    {   // what the pairing page and a report need to know about this boot
        char v[256]; bool have_key = store_secret_get("minimax_api_key", v, sizeof v) == ESP_OK && v[0], have_wifi = store_secret_get("wifi_ssid", v, sizeof v) == ESP_OK && v[0];   // a short buffer reads as "unset": size it for a real key
        char boot[240], path[200];
        snprintf(boot, sizeof boot, "{\"version\":\"v1.1 2026-11-03b\",\"reset_reason\":\"%s\",\"audio\":%s,\"wifi_configured\":%s,\"key_configured\":%s,\"volume\":%d}",
                 board_reset_reason(), audio_err == ESP_OK ? "true" : "false", have_wifi ? "true" : "false", have_key ? "true" : "false", store_setting_int("volume", 80));
        snprintf(path, sizeof path, "%s/logs/boot.json", DATA_DIR);
        atomic_file_write(path, boot, strlen(boot));
        store_event("boot (%s): audio %s, wifi %s, key %s, online %s", board_reset_reason(), audio_err == ESP_OK ? "ok" : "FAILED", have_wifi ? "set" : "unset", have_key ? "set" : "unset", net_is_online() ? "yes" : "no");
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
    for (unsigned beat = 1;; beat++) {
        clock_sleep_ms(10000);
        if (beat % 3 == 0) store_logs_flush();   // the captured log reaches flash every 30 s
        LOGI("heartbeat: internal free %u B (min %u B), big free %u B, vbat %d mV%s, call %s",
             (unsigned)mem_avail_internal(), (unsigned)mem_min_avail_internal(), (unsigned)mem_avail_big(),
             board_battery_mv(), board_is_charging() ? " charging" : "", call_state_name(call_state()));
    }
}
