#define LOG_TAG "instinct"
#include "instinct/instinct.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/task.h"
#include "ui/ui.h"
#include "audio/audio.h"
#include "board/board.h"
#include <stdatomic.h>
#include <time.h>

static atomic_bool s_busy;
static int64_t s_last_touch;

static bool is_night(void)
{
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    return tm.tm_hour >= 19 || tm.tm_hour < 7;
}

static void instinct_task(void *arg)
{
    (void)arg;
    bool night = is_night();
    for (;;) {
        clock_sleep_ms(1000);
        bool n = is_night();
        if (n != night) { night = n; ui_show_scene(UI_SCENE_HOME, night); }
    }
}

esp_err_t instinct_init(void)
{
    ui_set_idle(true);
    if (!task_spawn("gugu_instinct", instinct_task, NULL, 4096, 1, 0)) return ESP_FAIL;
    return ESP_OK;
}

void instinct_set_busy(bool in_call)
{
    atomic_store(&s_busy, in_call);
    ui_set_idle(!in_call);
}

void instinct_touched(int x, int y)
{
    (void)x; (void)y;
    if (atomic_load(&s_busy)) return;
    int64_t now = clock_us();
    if (now - s_last_touch < 1500000) return;     // one reaction per 1.5 s
    s_last_touch = now;
    board_vibrate(30);
    audio_sfx_play("coo");
    ui_play_action(UI_ACT_COO);
    ui_set_emotion(UI_EMO_HAPPY);
}
