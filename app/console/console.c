#define LOG_TAG "console"
#include "console/console.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/clock.h"
#include "foundation/atomic_file.h"
#include "store/store.h"
#include "provider/provider.h"
#include "net/net.h"
#include "call/call.h"
#include "audio/audio.h"
#include "board/board.h"
#include "parent/parent.h"
#include <string.h>
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
    if (argc < 2) { printf("usage: replay <wav path> [turns]\n"); return 1; }
    int turns = argc > 2 ? atoi(argv[2]) : 1;
    for (int i = 0; i < turns; i++) {
        esp_err_t err = call_replay_wav(argv[1], 60000);
        printf("replay turn %d: %s\n", i + 1, esp_err_to_name(err));
        if (err != ESP_OK) return 1;
    }
    return 0;
}

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

static int cmd_parent(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "off")) { parent_exit(); printf("parent mode off\n"); return 0; }
    esp_err_t err = parent_enter();
    printf("parent mode %s: hotspot %s password %s\n", esp_err_to_name(err), parent_hotspot_ssid(), parent_hotspot_password());
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
    { "replay", "replay <wav> [turns] — feed a recording as the microphone", cmd_replay },
    { "stat", "memory, battery, network, call state", cmd_stat },
    { "mem", "heap regions: free, largest block, low-water mark", cmd_mem },
    { "ls", "ls [dir]", cmd_ls },
    { "cat", "cat <path>", cmd_cat },
    { "parent", "parent [off] — hotspot + web page for the parent", cmd_parent },
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
