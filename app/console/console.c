#define LOG_TAG "console"
#include "console/console.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/clock.h"
#include "foundation/atomic_file.h"
#include "foundation/wav.h"
#include "store/store.h"
#include "provider/provider.h"
#include "net/net.h"
#include "call/call.h"
#include "audio/audio.h"
#include "board/board.h"
#include "parent/parent.h"
#include "world/world.h"
#include "life/life.h"
#include "mind/mind.h"
#include "art/art.h"
#include "instinct/instinct.h"
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

static int cmd_help(int argc, char **argv);

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) { printf("usage: wifi <ssid> [password]\n"); return 1; }
    store_secret_set("wifi_ssid", argv[1]);
    store_secret_set("wifi_pass", argc > 2 ? argv[2] : "");
    printf("saved; connecting...\n");
    esp_err_t err = net_wifi_connect(argv[1], argc > 2 ? argv[2] : "", 20000);
    printf("wifi: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_key(int argc, char **argv)
{
    if (argc < 3) { printf("usage: key <vendor> <api_key>\n"); return 1; }
    char name[64]; snprintf(name, sizeof name, "%s_api_key", argv[1]);
    store_secret_set(name, argv[2]);
    provider_set_credential(argv[1], "api_key", argv[2]);
    printf("key for %s stored (%zu chars)\n", argv[1], strlen(argv[2]));
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 3) { printf("usage: set <key> <value>\n"); return 1; }
    char *end; long v = strtol(argv[2], &end, 10);
    esp_err_t err = *end == '\0' ? store_setting_set_int(argv[1], (int)v) : store_setting_set_str(argv[1], argv[2]);
    printf("%s\n", esp_err_to_name(err));
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    if (argc < 2) { printf("usage: get <key>\n"); return 1; }
    printf("%s = %s\n", argv[1], store_setting_str(argv[1], "(unset)"));
    return 0;
}

static int cmd_call(int argc, char **argv) { (void)argc; (void)argv; call_key(CALL_KEY_BLUE, true, 0); clock_sleep_ms(30); call_key(CALL_KEY_BLUE, false, 30); printf("dialing\n"); return 0; }
static int cmd_hang(int argc, char **argv) { (void)argc; (void)argv; call_hangup(); return 0; }

static int cmd_replay(int argc, char **argv)
{
    if (argc < 2) { printf("usage: replay <wav path> [turns|early]\n"); return 1; }
    if (argc > 2 && !strcmp(argv[2], "early")) { esp_err_t err = call_replay_wav_early(argv[1], 60000); printf("replay early: %s\n", esp_err_to_name(err)); return err == ESP_OK ? 0 : 1; }
    int turns = argc > 2 ? atoi(argv[2]) : 1;
    for (int i = 0; i < turns; i++) {
        esp_err_t err = call_replay_wav(argv[1], 60000);
        printf("replay turn %d: %s\n", i + 1, esp_err_to_name(err));
        if (err != ESP_OK) return 1;
    }
    return 0;
}

// hold <ms> [wav]: press the blue key for <ms> with the real microphone. With a WAV, play it through the
// speaker meanwhile, so the microphone hears speech: an acoustic loopback test of the whole microphone path.
static int cmd_hold(int argc, char **argv)
{
    int ms = argc > 1 ? atoi(argv[1]) : 3000;
    call_key(CALL_KEY_BLUE, true, 0);
    int64_t t0 = clock_us();
    while (call_state() == CALL_CONNECTING && clock_us() - t0 < 30 * 1000000LL) clock_sleep_ms(50);
    if (call_state() != CALL_LISTENING) { printf("not listening: %s\n", call_state_name(call_state())); call_key(CALL_KEY_BLUE, false, 0); return 1; }
    if (argc > 2) {
        size_t n; char *buf = file_read_all(argv[2], &n);
        uint32_t sr, bytes; uint16_t ch; size_t off;
        if (buf && wav_parse_header((uint8_t *)buf, n, &sr, &ch, &bytes, &off) && sr == AUDIO_SAMPLE_RATE && ch == 1) {
            size_t total = bytes / 2; if (off + bytes > n) total = (n - off) / 2;
            audio_play_pcm((const int16_t *)(buf + off), total);
            int wav_ms = (int)(total * 1000 / AUDIO_SAMPLE_RATE);
            if (wav_ms + 400 > ms) ms = wav_ms + 400;
            printf("playing %s (%d ms) through the speaker while listening\n", argv[2], wav_ms);
        } else printf("cannot play %s (need 16 kHz mono 16-bit WAV)\n", argv[2]);
        if (buf) mem_release(buf);
    }
    clock_sleep_ms(ms);
    audio_capture_stats_t cs; audio_capture_stats(&cs);
    call_key(CALL_KEY_BLUE, false, ms);
    printf("held %d ms: captured %u ms, peak %d, rms %d\n", ms, (unsigned)cs.ms, cs.peak, cs.rms);
    return 0;
}

// mic [seconds]: a microphone level meter outside a call (cumulative peak and RMS every half second).
static void mic_cb(const int16_t *pcm, size_t n, void *user) { (void)pcm; (void)n; (void)user; }
static int cmd_mic(int argc, char **argv)
{
    if (call_state() != CALL_IDLE) { printf("hang up first\n"); return 1; }
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    audio_capture_start(mic_cb, NULL);
    for (int i = 0; i < secs * 2; i++) {
        clock_sleep_ms(500);
        audio_capture_stats_t cs; audio_capture_stats(&cs);
        printf("  %5u ms  peak %5d  rms %5d\n", (unsigned)cs.ms, cs.peak, cs.rms);
    }
    audio_capture_stop();
    return 0;
}

static int cmd_sleep(int argc, char **argv) { clock_sleep_ms(argc > 1 ? atoi(argv[1]) : 1000); return 0; }   // let a turn finish while the log streams

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    mem_print_stats();
    return 0;
}
static int cmd_stat(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("internal free %u B (min %u), big free %u B, vbat %d mV%s, online %d, call %s, audio queued %u ms\n",
           (unsigned)mem_avail_internal(), (unsigned)mem_min_avail_internal(), (unsigned)mem_avail_big(),
           board_battery_mv(), board_is_charging() ? " charging" : "", (int)net_is_online(), call_state_name(call_state()), (unsigned)audio_play_queued_ms());
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : store_data_dir();
    DIR *d = opendir(dir);
    if (!d) { printf("cannot open %s\n", dir); return 1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) printf("  %s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
    closedir(d);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) { printf("usage: cat <path>\n"); return 1; }
    size_t n; char *s = file_read_all(argv[1], &n);
    if (!s) { printf("no such file\n"); return 1; }
    fwrite(s, 1, n, stdout); printf("\n");
    mem_release(s);
    return 0;
}

// ---- the trip and the clock (world / life) ----
static int cmd_trip(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "new")) {
        life_plan_t p = { .kind_hint = argc > 2 ? argv[2] : NULL, .place_hint = argc > 3 ? argv[3] : NULL, .nights = argc > 4 ? atoi(argv[4]) : 0 };
        char why[200]; int64_t t0 = clock_us();
        esp_err_t err = life_plan_trip(&p, why, sizeof why);
        printf("trip new: %s in %lld ms: %s\n", esp_err_to_name(err), (long long)((clock_us() - t0) / 1000), why);
        return err == ESP_OK ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "clear")) { printf("%s\n", esp_err_to_name(world_trip_clear())); return 0; }
    if (argc > 1 && !strcmp(argv[1], "json")) { char p[200]; snprintf(p, sizeof p, "%s/trips/current.json", store_data_dir()); char *a[] = { "cat", p }; return cmd_cat(2, a); }
    world_state_t st; world_state(&st);
    printf("phase %s at %s, %s, local %02d:%02d, sun %02d:%02d-%02d:%02d, clock offset %+lld s\n", world_phase_name(st.phase), st.place, world_tod_name(st.tod),
           st.local_hour, st.local_min, st.sunrise_min / 60, st.sunrise_min % 60, st.sunset_min / 60, st.sunset_min % 60, (long long)world_clock_offset());
    const world_trip_t *t = world_trip_lock();
    if (!t) { printf("  no trip card (trip new [deliver|vacation|visit] [place] [nights])\n"); return 0; }
    printf("  %s: %s to %s (%s), %s..%s, %d nights, day %d, %d nights left\n  reason: %s\n", t->id, world_kind_name(t->kind), t->place.name, t->place.country,
           t->depart_date, t->return_date, t->nights, st.day_index, st.nights_left, t->reason);
    for (int i = 0; i < t->days_count; i++) printf("  day %d: %s\n", i + 1, t->days[i]);
    for (int i = 0; i < t->fact_count; i++) printf("  fact: %s\n", t->facts[i]);
    printf("  friend %s (%s), souvenir %s, objects %d, scene: %s\n", t->friend_name, t->friend_animal, t->souvenir, t->object_count, t->scene_prompt);
    world_trip_unlock();
    return 0;
}

static int cmd_clock(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "reset")) world_clock_reset();
    else if (argc > 1) world_clock_shift((int64_t)(atof(argv[1]) * 3600.0));
    time_t now = world_now(); struct tm tm; localtime_r(&now, &tm);
    printf("now %04d-%02d-%02d %02d:%02d:%02d (offset %+.1f h)\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, world_clock_offset() / 3600.0);
    return 0;
}

static int cmd_tick(int argc, char **argv) { (void)argc; (void)argv; printf("tick: %s\n", esp_err_to_name(life_tick())); return 0; }
static int cmd_diary(int argc, char **argv) { (void)argc; (void)argv; char why[160]; esp_err_t e = life_diary_now(why, sizeof why); printf("diary: %s: %s\n", esp_err_to_name(e), why); return 0; }
static int cmd_recap(int argc, char **argv) { (void)argc; (void)argv; char why[160]; int64_t t0 = clock_us(); esp_err_t e = life_recap_now(why, sizeof why); printf("recap: %s in %lld ms: %s\n", esp_err_to_name(e), (long long)((clock_us() - t0) / 1000), why); return 0; }
static int cmd_art(int argc, char **argv)
{
    char dir[200] = "";
    const world_trip_t *t = world_trip_lock();
    if (t) { snprintf(dir, sizeof dir, "%s/art/%s", store_data_dir(), t->id); world_trip_unlock(); }
    if (argc > 1 && !strcmp(argv[1], "retry") && dir[0]) {       // drop the .fail markers, then generate again
        DIR *d = opendir(dir); struct dirent *e;
        while (d && (e = readdir(d)) != NULL) if (strstr(e->d_name, ".fail")) { char p[300]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); remove(p); }
        if (d) closedir(d);
    }
    if (argc > 1 && (!strcmp(argv[1], "gen") || !strcmp(argv[1], "retry"))) {
        char why[160]; int64_t t0 = clock_us();
        esp_err_t err = life_art_now(why, sizeof why);
        printf("art: %s in %lld ms: %s\n", esp_err_to_name(err), (long long)((clock_us() - t0) / 1000), why);
    }
    if (!dir[0]) { printf("no trip card\n"); return 1; }
    DIR *d = opendir(dir); struct dirent *e;
    if (!d) { printf("%s: nothing generated yet\n", dir); return 0; }
    printf("%s:\n", dir);
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[300]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        art_image_t img;
        if (strstr(e->d_name, ".art") && art_load(p, &img) == ESP_OK) { printf("  %-16s %ux%u, %d colours%s\n", e->d_name, img.w, img.h, img.ncolors, img.alpha ? ", alpha" : ""); art_free(&img); }
        else printf("  %s\n", e->d_name);
    }
    closedir(d);
    return 0;
}

static int cmd_press(int argc, char **argv)   // press yellow [n] | press shake [n] | press pickup: what the child's hands do
{
    int n = argc > 2 ? atoi(argv[2]) : 1;
    if (argc > 1 && !strcmp(argv[1], "shake")) { for (int i = 0; i < n; i++) { instinct_shaken(160); clock_sleep_ms(300); } printf("shaken x%d\n", n); return 0; }
    if (argc > 1 && !strcmp(argv[1], "pickup")) { instinct_picked_up(); printf("picked up\n"); return 0; }
    if (argc < 2 || strcmp(argv[1], "yellow")) { printf("usage: press yellow [n] | shake [n] | pickup\n"); return 1; }
    for (int i = 0; i < n; i++) { instinct_yellow_tap(); clock_sleep_ms(250); }
    printf("yellow x%d\n", n);
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    net_wifi_ap_t aps[16]; int n = net_wifi_scan(aps, 16);
    for (int i = 0; i < n; i++) printf("  %-32s %4d dBm %s\n", aps[i].ssid, aps[i].rssi, aps[i].secure ? "locked" : "open");
    printf("%d networks\n", n);
    return 0;
}
static int cmd_events(int argc, char **argv)
{
    int want = argc > 1 ? atoi(argv[1]) : 30;
    char p[200]; snprintf(p, sizeof p, "%s/logs/events.log", store_data_dir());
    size_t n; char *s = file_read_all(p, &n);
    if (!s) { printf("no events yet\n"); return 0; }
    int lines = 0; const char *start = s + n;
    while (start > s && lines <= want) { start--; if (*start == '\n') lines++; }
    if (*start == '\n') start++;
    fputs(start, stdout); mem_release(s);
    return 0;
}

static int cmd_logs(int argc, char **argv)   // the same bundle the pairing page exports
{
    (void)argc; (void)argv;
    size_t n; char *s = store_export_logs(&n);
    if (!s) { printf("nothing to export\n"); return 1; }
    fwrite(s, 1, n, stdout); printf("\n"); mem_release(s);
    return 0;
}

static int cmd_selftest(int argc, char **argv)
{
    (void)argc; (void)argv;
    audio_selftest_t t; esp_err_t e = audio_selftest(&t);
    if (e != ESP_OK) { printf("selftest: %s\n", esp_err_to_name(e)); return 1; }
    printf("volume %u; quiet: %u ms, peak %d, rms %d; beep: %u ms, peak %d, rms %d (%s)\n", t.volume, (unsigned)t.ambient_ms, t.ambient_peak, t.ambient_rms,
           (unsigned)t.tone_ms, t.tone_peak, t.tone_rms, esp_err_to_name(t.sfx));
    printf("%s\n", !t.ambient_ms ? "the microphone delivers nothing" : t.tone_peak > 4 * t.ambient_peak + 800 ? "the microphone hears the speaker: both work"
                                                     : "the microphone does not hear the beep: the speaker may be silent (or the volume 0)");
    return 0;
}

static int cmd_ready(int argc, char **argv)   // ready auto|no|yes: what instinct hears when it asks whether a call could start
{
    if (argc < 2) { printf("usage: ready auto|no|yes\n"); return 1; }
    int v = !strcmp(argv[1], "no") ? 0 : !strcmp(argv[1], "yes") ? 1 : -1;
    instinct_set_ready_override(v);
    printf("ready: %s\n", v < 0 ? "asks the network and keys" : v ? "forced yes" : "forced no (puzzled after 15 s)");
    return 0;
}

static int cmd_tod(int argc, char **argv)
{
    static const char *const N[] = { "night", "sunrise", "day", "dusk" };
    int v = -1;
    if (argc > 1) for (int i = 0; i < 4; i++) if (!strcmp(argv[1], N[i])) v = i;
    if (argc > 1 && strcmp(argv[1], "auto") && v < 0) { printf("usage: tod night|sunrise|day|dusk|auto\n"); return 1; }
    instinct_set_tod_override(v);
    printf("time of day: %s\n", v < 0 ? "follows the clock" : N[v]);
    return 0;
}

static int cmd_journey(int argc, char **argv)
{
    MEM_BIG_BSS static char buf[4096];
    if (argc > 1 && !strcmp(argv[1], "forget") && argc > 2) { printf("%s %s\n", esp_err_to_name(mind_journey_forget(argv[2])), esp_err_to_name(world_trip_forget(argv[2]))); return 0; }
    if (argc > 1 && !strcmp(argv[1], "ask") && argc > 2) { size_t n = mind_journey_match(argv[2], buf, sizeof buf, 3); printf("%u match(es)\n%s", (unsigned)n, buf); return 0; }
    mind_journey_recent(buf, sizeof buf, 5); printf("%s", buf);
    size_t n = mind_book_text("about_child", buf, sizeof buf); printf("about_child: %u entries\n%s", (unsigned)n, buf);
    n = mind_book_text("canon_grown", buf, sizeof buf); printf("canon_grown: %u entries\n%s", (unsigned)n, buf);
    return 0;
}

static int cmd_parent(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "off")) { parent_exit(); printf("pairing mode off\n"); return 0; }
    esp_err_t err = parent_enter();
    printf("pairing mode %s: hotspot %s password %s page %s\n", esp_err_to_name(err), parent_hotspot_ssid(), parent_hotspot_password(), parent_hotspot_url());
    return err == ESP_OK ? 0 : 1;
}

static int cmd_sfx(int argc, char **argv) { if (argc < 2) return 1; printf("%s\n", esp_err_to_name(audio_sfx_play(argv[1]))); return 0; }
static int cmd_vol(int argc, char **argv) { if (argc < 2) return 1; audio_set_volume((uint8_t)atoi(argv[1])); store_setting_set_int("volume", atoi(argv[1])); return 0; }

typedef struct { const char *name; const char *help; int (*fn)(int, char **); } cmd_t;
static const cmd_t CMDS[] = {
    { "help", "list commands", cmd_help },
    { "wifi", "wifi <ssid> [password] — save and connect", cmd_wifi },
    { "key", "key <vendor> <api_key> — store a vendor key", cmd_key },
    { "set", "set <key> <value> — settings.json", cmd_set },
    { "get", "get <key>", cmd_get },
    { "call", "dial a call (blue key tap)", cmd_call },
    { "hang", "hang up", cmd_hang },
    { "replay", "replay <wav> [turns|early] — feed a recording as the microphone (early: let go before the line is open)", cmd_replay },
    { "hold", "hold <ms> [wav] — hold the blue key with the real microphone; a WAV is played through the speaker meanwhile", cmd_hold },
    { "mic", "mic [seconds] — microphone level meter", cmd_mic },
    { "sleep", "sleep <ms> — wait (scripts: let a turn finish)", cmd_sleep },
    { "stat", "memory, battery, network, call state", cmd_stat },
    { "mem", "heap regions: free, largest block, low-water mark", cmd_mem },
    { "ls", "ls [dir]", cmd_ls },
    { "cat", "cat <path>", cmd_cat },
    { "parent", "parent [off] — pairing mode: the hotspot and the page (yellow key held 3 s does the same)", cmd_parent },
    { "trip", "trip [new [kind] [place] [nights] | clear | json] — the trip card and where Gugu is", cmd_trip },
    { "clock", "clock [+hours|-hours|reset] — shift the clock to walk through a trip", cmd_clock },
    { "tick", "run the daily life tick now (diary, recap, then a trip when one is due)", cmd_tick },
    { "diary", "write the journey's day entries for the trip so far (no model call)", cmd_diary },
    { "recap", "write the recap of the finished trip into the journey (one model call)", cmd_recap },
    { "journey", "journey [ask <words> | forget <trip id>] — the three memory books", cmd_journey },
    { "art", "art [gen|retry] — the generated pictures of the current trip (backdrop, props, friend)", cmd_art },
    { "tod", "tod night|sunrise|day|dusk|auto — force the time of day the scene shows", cmd_tod },
    { "press", "press yellow [n] | shake [n] | pickup — the child's hands, from the console", cmd_press },
    { "scan", "nearby Wi-Fi networks (what the pairing page offers)", cmd_scan },
    { "events", "events [n] — the last n lines of the persistent event log (keys, call states, failures)", cmd_events },
    { "logs", "the diagnostic bundle (boot record, events, console log, last calls), secrets masked", cmd_logs },
    { "ready", "ready auto|no|yes — force what the puzzled face hears (no: puzzled after 15 s)", cmd_ready },
    { "selftest", "speaker → microphone loopback: does the microphone hear a beep?", cmd_selftest },
    { "sfx", "sfx <name> — play a sound effect", cmd_sfx },
    { "vol", "vol <0-100>", cmd_vol },
};

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++) printf("  %-8s %s\n", CMDS[i].name, CMDS[i].help);
    return 0;
}

int console_dispatch(char *line)
{
    char *argv[8]; int argc = 0;
    for (char *tok = strtok(line, " \t\r\n"); tok && argc < 8; tok = strtok(NULL, " \t\r\n")) argv[argc++] = tok;
    if (argc == 0) return 0;
    for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++) if (!strcmp(CMDS[i].name, argv[0])) return CMDS[i].fn(argc, argv);
    printf("unknown command: %s (try help)\n", argv[0]);
    return 1;
}
