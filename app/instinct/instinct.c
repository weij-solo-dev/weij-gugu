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

static atomic_bool s_busy;                   // a turn is on: the child talks or Gugu answers
static atomic_bool s_line_open;              // the conversation window is open (s_busy, or the 12 s after an answer)
static int64_t s_last_touch;
static atomic_int s_tod_override = -1;
// timed moods: the face set by play goes back to neutral by itself (checked by the instinct task every 250 ms)
static int64_t s_mood_until; static ui_emotion_t s_mood_next = UI_EMO_COUNT; static int64_t s_mood_next_until;
static int s_taps; static int64_t s_tap_last;
static int64_t s_shake_last;                 // the ball keeps bouncing while shakes keep coming

// Not ready to call (no Wi-Fi, offline, no key): a gear in a thought bubble at Gugu's upper right; Gugu, in a hurry,
// looks at you, then hard at the bubble, and back (2026-11-03, owner: "云泡泡，里面有个配置 icon … 很着急"; then "不需要
// 用手指指它，可以眼睛看着它", "嘴巴也不用动，眼睛可以暗示用户看右上角的东西"). A gaze that leaves you for something is
// the hint people follow.
#define PUZZLED_AFTER_US  (15 * 1000000LL)   // the answer must stay "no" this long: the Wi-Fi joining at boot is not a reason
#define GAZE_YOU_MS       1200               // eyes on you …
#define GAZE_BUBBLE_MS    2600               // … then on the bubble
static instinct_ready_fn s_ready_fn; static void *s_ready_user;
static void (*s_bubble_cb)(void *user); static void *s_bubble_user;
static instinct_ready_t s_reason;            // why it is not ready, while puzzled: the bubble's icon

// The friendly bubbles. An offer is handed over by the app (from life's worker); the instinct task shows and plays it.
#define IDEA_EXPIRE_US  (10 * 60 * 1000000LL)  // a light bulb nobody tapped fades after this long
#define IDEA_RETRY_US   (2 * 60 * 1000000LL)   // asked for one and nothing came: ask again after this long
#define PEEK_EVERY_US   (5 * 1000000LL)        // a glance at the bubble …
#define PEEK_MS         1500                   // … this long
static instinct_offer_cbs_t s_offer_cbs;
static mutex_t s_offer_lock;
static struct { instinct_offer_t kind; char path[160]; char text[240]; bool question; int64_t since; } s_offer;
static bool s_speaking, s_speaking_question;
static int64_t s_last_interaction, s_idea_asked, s_peek_next;
static atomic_int s_ready_override = -1;
static int64_t s_unready_since, s_gaze_next;
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
void instinct_set_ready_override(int ready) { atomic_store(&s_ready_override, ready < 0 || ready > INSTINCT_SERVICE_DOWN ? -1 : ready); s_unready_since = 0; }
void instinct_set_bubble_cb(void (*tapped)(void *user), void *user) { s_bubble_user = user; s_bubble_cb = tapped; }

static instinct_ready_t ready_now(char *why, size_t cap)
{
    int o = atomic_load(&s_ready_override);
    if (o >= 0) { snprintf(why, cap, "override"); return (instinct_ready_t)o; }
    why[0] = '\0';
    return s_ready_fn ? s_ready_fn(why, cap, s_ready_user) : INSTINCT_READY;
}

static ui_bubble_t bubble_of(instinct_ready_t r)
{
    return r == INSTINCT_WIFI_DOWN ? UI_BUBBLE_WIFI : r == INSTINCT_SERVICE_DOWN ? UI_BUBBLE_SERVICE : UI_BUBBLE_SETUP;
}
static const char *reason_name(instinct_ready_t r) { return r == INSTINCT_WIFI_DOWN ? "wifi" : r == INSTINCT_SERVICE_DOWN ? "service" : "setup"; }

static bool resting(void) { return !atomic_load(&s_busy) && !s_mood_until && !s_shake_last; }   // no call, no timed mood, not a ball

// Once a second: enter the puzzled rest after PUZZLED_AFTER of "not ready", leave it the moment it is ready.
static void readiness_tick(void)
{
    char why[96]; instinct_ready_t r = ready_now(why, sizeof why);
    int64_t now = clock_us();
    if (r != INSTINCT_READY) {
        if (!s_unready_since) s_unready_since = now;
        if (!s_puzzled && now - s_unready_since >= PUZZLED_AFTER_US) {
            s_puzzled = true; s_reason = r; atomic_store(&s_rest, UI_EMO_URGE); s_gaze_next = now + GAZE_BUBBLE_MS * 1000LL;
            store_event("puzzled (%s): %s", reason_name(r), why);
            LOGI("puzzled (%s): %s", reason_name(r), why);
            if (resting()) ui_set_emotion(UI_EMO_URGE);
        } else if (s_puzzled && r != s_reason) {             // e.g. Wi-Fi now set but not joining: the icon follows
            s_reason = r;
            store_event("bubble now %s: %s", reason_name(r), why);
            LOGI("bubble now %s: %s", reason_name(r), why);
        }
        return;
    }
    s_unready_since = 0;
    if (!s_puzzled) return;
    s_puzzled = false; atomic_store(&s_rest, UI_EMO_NEUTRAL);
    ui_show_bubble(UI_BUBBLE_NONE);
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
    s_last_interaction = clock_us();
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
    s_last_interaction = clock_us();
    if (atomic_load(&s_busy)) return;
    int64_t now = clock_us();
    if (!s_shake_last || now - s_shake_last > 1200000) { ui_bounce_start(); LOGI("shaken: into a ball"); }
    else ui_bounce_kick(intensity);
    s_shake_last = now;
    s_mood_until = 0;
}

void instinct_picked_up(void)
{
    s_last_interaction = clock_us();
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
    char w[96];
    if (!s_puzzled && ready_now(w, sizeof w) != INSTINCT_READY) s_unready_since = clock_us() - PUZZLED_AFTER_US;   // known now: puzzled as soon as the sad face is over
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

void instinct_set_offer_cbs(const instinct_offer_cbs_t *cbs) { if (cbs) s_offer_cbs = *cbs; }

void instinct_offer(instinct_offer_t kind, const char *wav_path, const char *text, bool question)
{
    if (!s_offer_lock) return;
    mutex_lock(s_offer_lock);
    s_offer.kind = kind; s_offer.question = question; s_offer.since = clock_us();
    snprintf(s_offer.path, sizeof s_offer.path, "%s", wav_path ? wav_path : "");
    snprintf(s_offer.text, sizeof s_offer.text, "%s", text ? text : "");
    mutex_unlock(s_offer_lock);
    s_idea_asked = 0;
    if (kind == INSTINCT_OFFER_IDEA) store_setting_set_int("idea_count", store_setting_int("idea_count", 0) + 1);
    store_event("bubble offered: %s", kind == INSTINCT_OFFER_HELLO ? "hello" : question ? "question" : "share");
    LOGI("offer %s: %s", kind == INSTINCT_OFFER_HELLO ? "hello" : "idea", text ? text : "");
}

void instinct_hush(void)
{
    if (!s_speaking) return;
    audio_play_flush(); ui_set_mouth(0);
    s_speaking = false;
    LOGI("line hushed");
}

static void speak_tick(void)   // every 40 ms while a line plays: the beak follows the voice
{
    if (!s_speaking) return;
    if (audio_play_idle()) {
        s_speaking = false; ui_set_mouth(0);
        if (s_speaking_question) mood(UI_EMO_CURIOUS, 8000);   // the "?" face: waiting for an answer (the blue key)
        else mood(UI_EMO_HAPPY, 1500);
        return;
    }
    ui_set_mouth(audio_play_level());
}

static void offer_tick(int64_t now)   // every 250 ms, when no problem bubble is up
{
    if (s_puzzled || !s_offer_lock) return;
    mutex_lock(s_offer_lock);
    if (s_offer.kind == INSTINCT_OFFER_IDEA && now - s_offer.since > IDEA_EXPIRE_US) { s_offer.kind = INSTINCT_OFFER_NONE; LOGI("light bulb faded"); }
    instinct_offer_t kind = s_offer.kind;
    mutex_unlock(s_offer_lock);
    bool show = kind != INSTINCT_OFFER_NONE && !atomic_load(&s_busy) && !atomic_load(&s_line_open) && !s_shake_last && !s_speaking;
    ui_show_bubble(show ? (kind == INSTINCT_OFFER_HELLO ? UI_BUBBLE_HELLO : UI_BUBBLE_IDEA) : UI_BUBBLE_NONE);
    if (show && resting() && now >= s_peek_next) {       // a glance up at the bubble now and then
        s_peek_next = now + PEEK_EVERY_US;
        mood(UI_EMO_PEEK, PEEK_MS);
    }
}

static void idea_schedule(int64_t now)   // every second: time to ask for a light bulb?
{
    if (s_puzzled || !s_offer_cbs.want_idea || s_speaking || atomic_load(&s_busy) || atomic_load(&s_line_open)) return;
    if (s_offer.kind != INSTINCT_OFFER_NONE) return;
    if (s_idea_asked && now - s_idea_asked < IDEA_RETRY_US) return;
    world_state_t st; world_state(&st);
    if (tod_of(&st) == WORLD_TOD_NIGHT) return;
    if (now - s_last_interaction < (int64_t)store_setting_int("idea_idle_min", 20) * 60 * 1000000LL) return;
    char today[11]; world_today(today, sizeof today);
    if (strcmp(store_setting_str("idea_day", ""), today)) { store_setting_set_str("idea_day", today); store_setting_set_int("idea_count", 0); }
    if (store_setting_int("idea_count", 0) >= store_setting_int("ideas_per_day", 3)) return;
    s_idea_asked = now;
    LOGI("asking for a light bulb");
    s_offer_cbs.want_idea(s_offer_cbs.user);
}

static void urge_tick(void)
{
    if (!s_puzzled) return;
    bool show = !atomic_load(&s_busy) && !atomic_load(&s_line_open) && !s_shake_last;
    ui_show_bubble(show ? bubble_of(s_reason) : UI_BUBBLE_NONE);
    int64_t now = clock_us();
    if (!resting()) { s_gaze_next = now + 800000; return; }        // a timed mood is on: pick the gaze up after it
    if (now < s_gaze_next) return;
    bool to_bubble = atomic_load(&s_rest) != UI_EMO_URGE;          // eyes on you → on the bubble, and back
    atomic_store(&s_rest, to_bubble ? UI_EMO_URGE : UI_EMO_URGE_YOU);
    ui_set_emotion(to_bubble ? UI_EMO_URGE : UI_EMO_URGE_YOU);
    s_gaze_next = now + (to_bubble ? GAZE_BUBBLE_MS : GAZE_YOU_MS) * 1000LL;
}

static void instinct_task(void *arg)
{
    (void)arg;
    world_state_t st; world_state(&st);
    show(&st);
    bool away = st.away; world_tod_t tod = tod_of(&st); int tick = 0;
    int64_t next250 = 0, next1s = 0;
    s_last_interaction = clock_us();
    for (;;) {
        clock_sleep_ms(40);
        int64_t now = clock_us();
        speak_tick();
        if (now < next250) continue;
        next250 = now + 250000;
        shake_tick();
        moods_tick();
        urge_tick();
        offer_tick(now);
        if (now < next1s) continue;
        next1s = now + 1000000;
        readiness_tick();
        idea_schedule(now);
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
    if (!s_offer_lock) s_offer_lock = mutex_create();
    ui_set_idle(true);
    if (!task_spawn("gugu_instinct", instinct_task, NULL, 6144, 1, 0)) return ESP_FAIL;
    return ESP_OK;
}

void instinct_set_tod_override(int tod) { atomic_store(&s_tod_override, tod); }

void instinct_set_busy(bool in_turn)
{
    bool was = atomic_exchange(&s_busy, in_turn);
    s_last_interaction = clock_us();
    ui_set_idle(!in_turn);
    if (in_turn && !was && ui_bounce_stop()) { s_shake_last = 0; s_mood_until = 0; }   // the child talks while Gugu is a ball: it lands at once
    if (!in_turn && !s_mood_until && atomic_load(&s_rest) != UI_EMO_NEUTRAL) ui_set_emotion((ui_emotion_t)atomic_load(&s_rest));   // the call put its own face on
}

void instinct_set_line_open(bool open) { atomic_store(&s_line_open, open); s_last_interaction = clock_us(); }

// A touch: an object reacts in its own way (and the finger feels it), Gugu reacts when touched itself,
// empty wall and sky do nothing.
void instinct_touched(int x, int y)
{
    if (atomic_load(&s_busy)) return;
    int64_t now = clock_us();
    if (now - s_last_touch < 600000) return;
    s_last_interaction = now;
    if (s_puzzled && ui_bubble_hit(x, y)) {                 // the bubble: straight to pairing mode, where it gets fixed
        s_last_touch = now;
        board_vibrate(30); audio_sfx_play("pop");
        LOGI("bubble tapped (%s)", reason_name(s_reason));
        if (s_bubble_cb) s_bubble_cb(s_bubble_user);
        return;
    }
    if (!s_puzzled && s_offer.kind != INSTINCT_OFFER_NONE && ui_bubble_hit(x, y)) {   // a friendly bubble: Gugu says its line
        s_last_touch = now;
        instinct_offer_t kind; char path[160], text[240]; bool q;
        mutex_lock(s_offer_lock);
        kind = s_offer.kind; q = s_offer.question; snprintf(path, sizeof path, "%s", s_offer.path); snprintf(text, sizeof text, "%s", s_offer.text);
        s_offer.kind = INSTINCT_OFFER_NONE;
        mutex_unlock(s_offer_lock);
        ui_show_bubble(UI_BUBBLE_NONE);
        board_vibrate(20);
        if (audio_play_file(path) == ESP_OK) {
            s_speaking = true; s_speaking_question = q;
            s_mood_until = 0; ui_set_emotion(UI_EMO_HAPPY);
        }
        LOGI("bubble tapped: %s", text);
        if (s_offer_cbs.heard) s_offer_cbs.heard(kind, text, q, s_offer_cbs.user);
        return;
    }
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
