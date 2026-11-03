// instinct.c — the pet when nobody is calling: which scene is up (home, or the trip's place with its generated
// backdrop, props and friend), the time of day there, what a touch does (an object reacts, Gugu reacts), the
// idle presentation. Behaviour only; it never talks to vendors.
#define LOG_TAG "instinct"
#include "instinct/instinct.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/task.h"
#include "foundation/atomic_file.h"
#include "ui/ui.h"
#include "audio/audio.h"
#include "board/board.h"
#include "store/store.h"
#include "world/world.h"
#include "art/art.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_bool s_busy;
static int64_t s_last_touch;
static atomic_int s_tod_override = -1;
// timed moods: the face set by play goes back to neutral by itself (checked by the instinct task every 250 ms)
static int64_t s_mood_until; static ui_emotion_t s_mood_next = UI_EMO_COUNT; static int64_t s_mood_next_until;
static int s_taps; static int64_t s_tap_last;
static int64_t s_shake_last;                 // the ball keeps bouncing while shakes keep coming

// Not ready to call (no Wi-Fi, offline, no key): the puzzled face is the resting face, with a look around now and then.
#define PUZZLED_AFTER_US  (15 * 1000000LL)   // the answer must stay "no" this long: the Wi-Fi joining at boot is not a reason
#define LOOK_EVERY_US     (12 * 1000000LL)
static instinct_ready_fn s_ready_fn; static void *s_ready_user;
static atomic_int s_ready_override = -1;
static int64_t s_unready_since, s_last_look;
static bool s_puzzled;
static atomic_int s_rest = UI_EMO_NEUTRAL;  // the face a timed mood returns to

// the trip's pictures, loaded once per trip
static char s_art_trip[40];
static art_image_t s_backdrop; static bool s_backdrop_loaded;
static art_image_t s_sprites[UI_MAX_OBJECTS]; static int s_sprite_n;
static ui_object_t s_objects[UI_MAX_OBJECTS]; static int s_object_n;
static bool s_objects_built;

static world_tod_t tod_of(const world_state_t *st)
{
    int o = atomic_load(&s_tod_override);
    return o >= 0 && o <= WORLD_TOD_DUSK ? (world_tod_t)o : st->tod;
}

static void drop_trip_art(void)
{
    if (s_backdrop_loaded) art_free(&s_backdrop);
    for (int i = 0; i < s_sprite_n; i++) art_free(&s_sprites[i]);
    s_backdrop_loaded = false; s_sprite_n = 0; s_object_n = 0; s_objects_built = false;
}

static ui_obj_kind_t kind_of(const char *kind) { return !strcmp(kind, "living") ? UI_OBJ_LIVING : !strcmp(kind, "light") ? UI_OBJ_LIGHT : UI_OBJ_STATIC; }
static ui_behave_t behave_of(ui_obj_kind_t k) { return k == UI_OBJ_LIVING ? UI_BEHAVE_BOUNCE : k == UI_OBJ_LIGHT ? UI_BEHAVE_TOGGLE : UI_BEHAVE_WIGGLE; }
static bool is_accessory(const char *id) { return !strcmp(id, "sun_hat") || !strcmp(id, "sunglasses") || !strcmp(id, "camera"); }

// The object list for the trip: generated sprites where the night job made them, built-in ones otherwise.
// Props stand on the ground line left of Gugu, the friend to its right; whatever does not fit is left out.
static void build_objects(const char *trip_id)
{
    s_object_n = 0; s_sprite_n = 0;
    char ids[4][24]; int n = 0; char animal[32] = ""; const char *acc = NULL;
    const world_trip_t *t = world_trip_lock();
    if (!t) return;
    for (int i = 0; i < t->object_count && n < 3; i++) {
        if (!t->objects[i][0]) continue;
        if (is_accessory(t->objects[i])) { static char a[24]; snprintf(a, sizeof a, "%s", t->objects[i]); acc = a; continue; }
        snprintf(ids[n++], sizeof ids[0], "%s", t->objects[i]);
    }
    snprintf(animal, sizeof animal, "%s", t->friend_animal);
    world_trip_unlock();
    ui_set_accessory(acc);
    int left = 4, right = 84;
    for (int k = 0; k <= n && s_object_n < UI_MAX_OBJECTS; k++) {
        int i = k == 0 ? n : k - 1;                 // the friend first (it owns the right side), then the props
        bool friend = i == n;
        if (friend && !animal[0]) continue;
        const char *id = friend ? "friend" : ids[i];
        art_catalogue_t e;
        if (!friend) { art_catalogue_lookup(store_content_dir(), id, &e); if (e.builtin) continue; }
        ui_object_t *o = &s_objects[s_object_n];
        memset(o, 0, sizeof *o);
        snprintf(o->id, sizeof o->id, "%s", id);
        o->kind = friend ? UI_OBJ_LIVING : kind_of(e.kind);
        o->behave = friend ? UI_BEHAVE_WAVE : behave_of(o->kind);
        char p[200]; snprintf(p, sizeof p, "%s/art/%s/%s.art", store_data_dir(), trip_id, id);
        if (file_exists(p) && art_load(p, &s_sprites[s_sprite_n]) == ESP_OK) o->art = &s_sprites[s_sprite_n++];
        int w = o->art ? o->art->w : (friend ? 18 : e.size);
        if (friend) { o->x = right; right += w + 2; }
        else if (left + w <= 32) { o->x = left; left += w + 2; }
        else if (right + w <= 114) { o->x = right; right += w + 2; }
        else continue;
        o->y = ART_GROUND_TOP + 1;
        s_object_n++;
    }
    s_objects_built = true;
    LOGI("objects for %s: %d placed, %d generated sprites%s%s", trip_id, s_object_n, s_sprite_n, acc ? ", wearing " : "", acc ? acc : "");
}

// The scene follows the world: home, or the destination (its generated backdrop when the night job has made
// one, the built-in away scene until then), at the time of day where Gugu is.
static void show(const world_state_t *st)
{
    world_tod_t tod = tod_of(st);
    if (st->away) {
        char id[40] = "";
        const world_trip_t *t = world_trip_lock();
        if (t) { snprintf(id, sizeof id, "%s", t->id); world_trip_unlock(); }
        if (strcmp(id, s_art_trip)) { drop_trip_art(); snprintf(s_art_trip, sizeof s_art_trip, "%s", id); }
        if (!s_backdrop_loaded && id[0]) {
            char p[200]; snprintf(p, sizeof p, "%s/art/%s/backdrop.art", store_data_dir(), id);
            if (file_exists(p) && art_load(p, &s_backdrop) == ESP_OK) { s_backdrop_loaded = true; LOGI("backdrop %s loaded (%d colours)", p, s_backdrop.ncolors); }
        }
        if (!s_objects_built && id[0]) build_objects(id);
        if (s_backdrop_loaded) {
            art_rgb_t pal[ART_MAX_COLORS];
            art_palette_tod(&s_backdrop, (art_tod_t)tod, pal);
            ui_show_backdrop(&s_backdrop, pal);
        } else ui_show_scene(UI_SCENE_AWAY, tod == WORLD_TOD_NIGHT);
        ui_scene_set_objects(s_objects, s_object_n, (art_tod_t)tod);
        return;
    }
    if (s_art_trip[0]) { drop_trip_art(); s_art_trip[0] = '\0'; ui_set_accessory(NULL); }
    ui_scene_set_objects(NULL, 0, (art_tod_t)tod);
    ui_show_scene(UI_SCENE_HOME, tod == WORLD_TOD_NIGHT);
    ui_scene_set_tod((art_tod_t)tod);
}

static void mood(ui_emotion_t e, int ms) { ui_set_emotion(e); s_mood_until = clock_us() + (int64_t)ms * 1000; }

void instinct_set_ready_probe(instinct_ready_fn fn, void *user) { s_ready_user = user; s_ready_fn = fn; }
void instinct_set_ready_override(int ready) { atomic_store(&s_ready_override, ready < 0 ? -1 : ready ? 1 : 0); s_unready_since = 0; }

static bool ready_now(char *why, size_t cap)
{
    int o = atomic_load(&s_ready_override);
    if (o >= 0) { snprintf(why, cap, "override"); return o == 1; }
    why[0] = '\0';
    return !s_ready_fn || s_ready_fn(why, cap, s_ready_user);
}

static bool resting(void) { return !atomic_load(&s_busy) && !s_mood_until && !s_shake_last; }   // no call, no timed mood, not a ball

// Once a second: enter the puzzled rest after PUZZLED_AFTER of "not ready", leave it the moment it is ready.
static void readiness_tick(void)
{
    char why[64]; bool ready = ready_now(why, sizeof why);
    int64_t now = clock_us();
    if (!ready) {
        if (!s_unready_since) s_unready_since = now;
        if (!s_puzzled && now - s_unready_since >= PUZZLED_AFTER_US) {
            s_puzzled = true; atomic_store(&s_rest, UI_EMO_CURIOUS); s_last_look = now;
            store_event("puzzled: %s", why);
            LOGI("puzzled: %s", why);
            if (resting()) { ui_set_emotion(UI_EMO_CURIOUS); ui_play_action(UI_ACT_LOOK_AROUND); }
        } else if (s_puzzled && resting() && now - s_last_look >= LOOK_EVERY_US) {
            s_last_look = now;
            if (ui_get_emotion() != UI_EMO_CURIOUS) ui_set_emotion(UI_EMO_CURIOUS);
            ui_play_action(UI_ACT_LOOK_AROUND);
        }
        return;
    }
    s_unready_since = 0;
    if (!s_puzzled) return;
    s_puzzled = false; atomic_store(&s_rest, UI_EMO_NEUTRAL);
    store_event("ready again");
    LOGI("ready again");
    if (!atomic_load(&s_busy) && !s_shake_last) mood(UI_EMO_HAPPY, 1500);   // a hop: whoever just fixed it sees it worked
}

// The pigeon whistle: Gugu answers one (a whistle back and a wave), flies a loop for a couple in a row,
// and after five or more in quick succession gets dizzy, then cross for a moment, then is fine again.
// At night it only stirs in its sleep.
void instinct_yellow_tap(void)
{
    if (atomic_load(&s_busy)) return;
    int64_t now = clock_us();
    if (now - s_tap_last > 4 * 1000000LL) s_taps = 0;
    s_tap_last = now; s_taps++;
    world_state_t st; world_state(&st);
    if (tod_of(&st) == WORLD_TOD_NIGHT) {
        audio_sfx_play("coo"); ui_play_action(UI_ACT_NOD); mood(UI_EMO_SLEEPY, 2500);
        LOGI("yellow tap at night: stirs");
        return;
    }
    board_vibrate(20);
    if (s_taps >= 5) {
        audio_sfx_play("pop");
        mood(UI_EMO_DIZZY, 1800); s_mood_next = UI_EMO_ANGRY; s_mood_next_until = 2000;
        LOGI("yellow tap x%d: dizzy, then cross", s_taps);
    } else if (s_taps >= 2) {
        audio_sfx_play("whistle"); ui_play_action(UI_ACT_FLY); mood(UI_EMO_HAPPY, 2000);
        LOGI("yellow tap x%d: a loop", s_taps);
    } else {
        audio_sfx_play("whistle"); ui_play_action(UI_ACT_WAVE); mood(UI_EMO_HAPPY, 1500);
        LOGI("yellow tap: whistles back");
    }
}

void instinct_shaken(int intensity)
{
    if (atomic_load(&s_busy)) return;
    int64_t now = clock_us();
    if (!s_shake_last || now - s_shake_last > 1200000) { ui_bounce_start(); LOGI("shaken: into a ball"); }
    else ui_bounce_kick(intensity);
    s_shake_last = now;
    s_mood_until = 0;
}

void instinct_picked_up(void)
{
    if (atomic_load(&s_busy) || s_shake_last) return;
    audio_sfx_play("chirp");
    ui_play_action(UI_ACT_WAVE);
    mood(UI_EMO_CURIOUS, 2000);
    LOGI("picked up");
}

void instinct_refuse(const char *why)
{
    board_vibrate(90);
    audio_sfx_play("nosignal");
    ui_play_action(UI_ACT_SHAKE_HEAD);
    mood(UI_EMO_SAD, 2500);
    LOGW("refused: %s", why ? why : "");
    char w[64];
    if (!s_puzzled && !ready_now(w, sizeof w)) s_unready_since = clock_us() - PUZZLED_AFTER_US;   // known now: puzzled as soon as the sad face is over
}

static void shake_tick(void)
{
    if (!s_shake_last || clock_us() - s_shake_last < 1200000) return;
    s_shake_last = 0;
    if (ui_bounce_stop()) {
        audio_sfx_play("pop");
        mood(UI_EMO_DIZZY, 1800); s_mood_next = UI_EMO_ANGRY; s_mood_next_until = 2000;
        LOGI("shaking stopped: dizzy, then cross");
    }
}

static void moods_tick(void)
{
    if (!s_mood_until || clock_us() < s_mood_until) return;
    if (s_mood_next != UI_EMO_COUNT) { ui_set_emotion(s_mood_next); s_mood_until = clock_us() + s_mood_next_until * 1000; s_mood_next = UI_EMO_COUNT; return; }
    s_mood_until = 0;
    if (!atomic_load(&s_busy)) ui_set_emotion((ui_emotion_t)atomic_load(&s_rest));
}

static void instinct_task(void *arg)
{
    (void)arg;
    world_state_t st; world_state(&st);
    show(&st);
    bool away = st.away; world_tod_t tod = tod_of(&st); int tick = 0, quarter = 0;
    for (;;) {
        clock_sleep_ms(250);
        shake_tick();
        moods_tick();
        if (++quarter % 4) continue;
        readiness_tick();
        world_state(&st);
        world_tod_t now_tod = tod_of(&st);
        bool changed = st.away != away || now_tod != tod;
        if (st.away && (!s_backdrop_loaded || s_sprite_n < s_object_n) && ++tick % 5 == 0) { s_objects_built = false; changed = true; }   // the night job may finish pictures while away
        if (changed) {
            away = st.away; tod = now_tod;
            LOGI("scene: %s (%s), %s", away ? "away" : "home", st.place, world_tod_name(tod));
            show(&st);
        }
    }
}

esp_err_t instinct_init(void)
{
    ui_set_idle(true);
    if (!task_spawn("gugu_instinct", instinct_task, NULL, 6144, 1, 0)) return ESP_FAIL;
    return ESP_OK;
}

void instinct_set_tod_override(int tod) { atomic_store(&s_tod_override, tod); }

void instinct_set_busy(bool in_call)
{
    atomic_store(&s_busy, in_call);
    ui_set_idle(!in_call);
    if (!in_call && !s_mood_until && atomic_load(&s_rest) != UI_EMO_NEUTRAL) ui_set_emotion((ui_emotion_t)atomic_load(&s_rest));   // the call put its own face on
}

// A touch: an object reacts in its own way (and the finger feels it), Gugu reacts when touched itself,
// empty wall and sky do nothing.
void instinct_touched(int x, int y)
{
    if (atomic_load(&s_busy)) return;
    int64_t now = clock_us();
    if (now - s_last_touch < 600000) return;
    int hit = ui_scene_hit(x, y);
    if (hit >= 0) {
        s_last_touch = now;
        const char *id = ui_scene_object_id(hit);
        ui_behave_t b = ui_scene_poke(hit);
        board_vibrate(30);
        const char *sfx = b == UI_BEHAVE_TOGGLE ? "bell" : b == UI_BEHAVE_WIGGLE ? "tap" : b == UI_BEHAVE_BOUNCE || b == UI_BEHAVE_WAVE ? "chirp" :
                          !strcmp(id, "window") ? "whistle" : !strcmp(id, "shelf") ? "pop" : "tap";
        audio_sfx_play(sfx);
        if (b == UI_BEHAVE_WAVE || b == UI_BEHAVE_BOUNCE) { ui_play_action(UI_ACT_WAVE); mood(UI_EMO_HAPPY, 2500); }
        else if (!strcmp(id, "window")) { ui_play_action(UI_ACT_WHISTLE); mood(UI_EMO_CURIOUS, 2500); }
        else ui_play_action(UI_ACT_NOD);
        LOGI("touched %s → %s", id, sfx);
        return;
    }
    if (!ui_scene_hit_gugu(x, y)) return;
    if (now - s_last_touch < 1500000) return;     // one reaction per 1.5 s
    s_last_touch = now;
    board_vibrate(30);
    audio_sfx_play("coo");
    ui_play_action(UI_ACT_COO);
    mood(UI_EMO_HAPPY, 2500);
}
