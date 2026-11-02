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
#include "provider/provider.h"
#include "mind/mind.h"
#include "audio/audio.h"
#include "engine/engine.h"
#include "call/call.h"
#include "console/console.h"
#include "instinct/instinct.h"
#include "parent/parent.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef GUGU_HOST
#define CONTENT_DIR GUGU_REPO_ROOT "/content"
#define DATA_DIR    GUGU_REPO_ROOT "/host/data"
#else
#define CONTENT_DIR "/content"
#define DATA_DIR    "/data"
#endif

static engine_t *s_engine;

static void on_board_event(const board_event_t *ev, void *user)
{
    (void)user;
    switch (ev->kind) {
    case BOARD_EV_KEY_DOWN: call_key(ev->a == BOARD_KEY_BLUE ? CALL_KEY_BLUE : CALL_KEY_YELLOW, true, 0); break;
    case BOARD_EV_KEY_UP:   call_key(ev->a == BOARD_KEY_BLUE ? CALL_KEY_BLUE : CALL_KEY_YELLOW, false, ev->b); break;
    case BOARD_EV_TOUCH_DOWN: instinct_touched(ev->a, ev->b); break;
    default: break;
    }
}

static void on_call_state(call_state_t s, void *user)
{
    (void)user;
    instinct_set_busy(s != CALL_IDLE && s != CALL_ENDED);
}

static bool is_night(void)
{
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    return tm.tm_hour >= 19 || tm.tm_hour < 7;
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
    ui_show_scene(UI_SCENE_HOME, is_night());

    store_init(DATA_DIR, CONTENT_DIR);
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
    ESP_ERROR_CHECK(audio_init());
    audio_set_volume((uint8_t)store_setting_int("volume", 80));

    s_engine = engine_cascade_create(call_engine_event, NULL);
    call_config_t cc = { .idle_timeout_ms = (uint32_t)store_setting_int("idle_timeout_ms", 12000),
                         .max_call_ms = (uint32_t)store_setting_int("max_call_ms", 10 * 60 * 1000),
                         .lang_hint = LANG_UNKNOWN, .where = "在家里，刚被叫到洞口聊天" };
    ESP_ERROR_CHECK(call_init(s_engine, &cc));
    instinct_init();
    parent_init();
    call_set_state_listener(on_call_state, NULL);
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
