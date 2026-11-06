// instinct_show.c — the gift key's show (2026-11-06, the owner: "如果连续多次按压，gugu 将变成歌手表演一段歌曲，对应场景
// 也可以临时变化（比如 disco 等）"). Five presses within 2.5 s: what was falling or in hand is dropped with a "ding" and a
// microphone stands there; the stage comes up (the disco by day, a starry lullaby after dark), a song plays (audio_song:
// a public-domain tune or one made up on the spot), the lights and Gugu move on its beats and the face changes every bar;
// a tap is applause, another press a cheer. Then a bow, applause, the scene comes back, and Gugu rests its voice for three
// minutes (presses then just feed it, like the canon's "嗓子哑了"). A call or a shake ends the show at once.
#define LOG_TAG "instinct"
#include "instinct_internal.h"
#include "audio/audio.h"
#include "store/store.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include <stdatomic.h>
#include <string.h>

#define BURST_N        5
#define BURST_US       (2500 * 1000LL)
#define REST_US        (3 * 60 * 1000000LL)
#define INTRO_US       (1200 * 1000LL)
#define BOW_US         (2600 * 1000LL)
typedef enum { SHOW_OFF = 0, SHOW_ASKED, SHOW_INTRO, SHOW_SING, SHOW_BOW } show_phase_t;
static atomic_int s_phase = SHOW_OFF;
static int64_t s_presses[BURST_N]; static int s_press_i;
static int64_t s_phase_until, s_rest_until;
static bool s_night;
static char s_last_song[24];
static const char *const DAY_SONGS[] = { "twinkle", "frere_jacques", "london_bridge", "mary_lamb", "old_macdonald", "ode_to_joy", "jingle_bells" };
#define N_DAY_SONGS (sizeof DAY_SONGS / sizeof DAY_SONGS[0])
static const ui_emotion_t BAR_FACES[] = { UI_EMO_HAPPY, UI_EMO_WAVE, UI_EMO_LAUGHING, UI_EMO_PROUD };
static uint32_t s_rng = 0x6C8E9CF5u;
static uint32_t rnd(void) { uint32_t x = s_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s_rng = x ? x : 0x6C8E9CF5u; }

bool show_on(void) { return atomic_load(&s_phase) != SHOW_OFF; }

bool show_press(int64_t now)
{
    if (show_on()) { if (atomic_load(&s_phase) == SHOW_SING) { audio_sfx_play("clap"); ui_stage_cheer(); } return true; }   // a cheer from the front row
    s_presses[s_press_i++ % BURST_N] = now;
    int64_t first = s_presses[s_press_i % BURST_N];
    if (s_press_i < BURST_N || now - first > BURST_US) return false;
    if (now < s_rest_until) { LOGI("show: the voice is still resting (%lld s more)", (long long)((s_rest_until - now) / 1000000)); return false; }
    if (!instinct_can_play()) return false;
    memset(s_presses, 0, sizeof s_presses); s_press_i = 0;
    atomic_store(&s_phase, SHOW_ASKED);                            // the instinct task puts it on (show_tick)
    return true;
}

static void on_beat(int beat, int per_bar, void *user)             // the song's task: a beat is heard
{
    (void)user;
    if (atomic_load(&s_phase) != SHOW_SING) return;
    ui_stage_beat(beat, per_bar);
    if (per_bar > 0 && beat % per_bar == 0) {
        int bar = beat / per_bar;
        ui_set_emotion(s_night ? (bar % 2 ? UI_EMO_PUFF : UI_EMO_SLEEPY) : BAR_FACES[bar % (int)(sizeof BAR_FACES / sizeof BAR_FACES[0])]);   // a lullaby: drowsy, snug
    }
}

static void stage_down(void)
{
    audio_song_stop();
    ui_set_mouth(0);
    ui_pose(UI_POSE_NONE);
    ui_stage(UI_STAGE_NONE);
    instinct_scene_back();
}

void show_stop(const char *why)
{
    int ph = atomic_exchange(&s_phase, SHOW_OFF);
    if (ph == SHOW_OFF || ph == SHOW_ASKED) return;
    LOGI("show stopped: %s", why);
    store_event("show stopped: %s", why);
    stage_down();
    s_rest_until = clock_us() + REST_US / 3;                       // cut short: a shorter rest
}

bool show_tap(void)
{
    if (atomic_load(&s_phase) != SHOW_SING) return show_on();
    audio_sfx_play("clap");
    ui_stage_cheer();
    return true;
}

void show_tick(int64_t now)
{
    switch ((show_phase_t)atomic_load(&s_phase)) {
    case SHOW_OFF: return;
    case SHOW_ASKED:                                               // 1. the food becomes a microphone, the stage comes up
        instinct_feed_stop();
        s_night = instinct_night();
        audio_sfx_play("bell");
        ui_stage(s_night ? UI_STAGE_NIGHT : UI_STAGE_DISCO);
        ui_set_emotion(s_night ? UI_EMO_SLEEPY : UI_EMO_SURPRISED);
        s_phase_until = now + INTRO_US;
        atomic_store(&s_phase, SHOW_INTRO);
        break;
    case SHOW_INTRO:                                               // 2. the song
        if (now < s_phase_until) return;
        {
            s_rng ^= (uint32_t)now;
            const char *song = s_night ? "lullaby" : NULL;
            if (!song) {                                           // a tune it knows, or (two times in five) one it makes up; never the same twice running
                do song = rnd() % 5 < 2 ? "improvise" : DAY_SONGS[rnd() % N_DAY_SONGS]; while (!strcmp(song, s_last_song) && strcmp(song, "improvise"));
            }
            snprintf(s_last_song, sizeof s_last_song, "%s", song);
            atomic_store(&s_phase, SHOW_SING);
            if (audio_song_play(song, on_beat, NULL) != ESP_OK) { show_stop("the song would not start"); return; }
            store_event("show: %s%s", audio_song_title(), s_night ? " (a lullaby, after dark)" : "");
            ui_set_emotion(s_night ? UI_EMO_PUFF : UI_EMO_HAPPY);
        }
        break;
    case SHOW_SING:                                                // 3. the beak sings along; the lights follow on_beat
        ui_set_mouth(audio_play_level());
        if (audio_song_playing()) return;
        ui_set_mouth(0);
        ui_pose(UI_POSE_REACH_R);                                  // 4. a bow, and applause
        audio_sfx_play(s_night ? "coo" : "clap");
        s_phase_until = now + BOW_US;
        atomic_store(&s_phase, SHOW_BOW);
        break;
    case SHOW_BOW:
        if (now < s_phase_until) return;
        atomic_store(&s_phase, SHOW_OFF);                          // 5. the scene back; the voice rests
        stage_down();
        instinct_face(s_night ? UI_EMO_SLEEPY : UI_EMO_PROUD, 1800);
        s_rest_until = now + REST_US;
        LOGI("show over");
        break;
    }
}
