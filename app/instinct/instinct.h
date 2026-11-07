// instinct.h — the pet when nobody is calling: idle presentation (blink, breathe, day/night room),
// touch and pick-up reactions. Behaviour only; it never talks to vendors.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t instinct_init(void);
void instinct_set_busy(bool in_turn);       // a turn is on (the line opening, listening, thinking, speaking): play and touches wait
void instinct_set_line_open(bool open);     // the conversation window, the 12 s after an answer included: no bubbles meanwhile
void instinct_touched(int x, int y);        // app forwards touch events
void instinct_set_tod_override(int tod);    // -1 follows the clock; else a world_tod_t, for screenshots and tests
// The yellow key feeds Gugu, whatever is on screen: something falls (corn, a nut, a burger … a coin, a sock), Gugu pecks
// it up and reacts (happy, laughing, an eye-roll, cross …); five in a minute and it is full; at night too, then it dozes
// off again. Presses queue (three at most). A bubble and the scene's objects answer touches, never this key.
void instinct_yellow_tap(void);
// Tests and screenshots: the next drops are this item (a FOOD id: corn, nut, burger, cake, broccoli, chili, coin, sock) with
// its first or second plan (0 / 1; -1 by chance); NULL goes back to chance.
void instinct_feed_override(const char *food, int plan);
// Idle life (looking around, preening, reading the paper, a stretch) comes by itself when nobody is about; this starts one
// in a second, whatever the hour (look, preen, read, stretch; console `idle`, screenshots). False for an unknown name.
bool instinct_idle_now(const char *kind);
void instinct_picked_up(void);              // IMU: looks up at you
void instinct_refuse(const char *why);      // a call that cannot start or a turn that failed: a buzz, a head shake, then the bubble says why (a sad face only if nothing known is wrong)
// Can Gugu make a call right now? The app answers (only it sees the network and the keys); NULL = always yes.
// While the answer stays no for PUZZLED_AFTER, a thought bubble with a settings gear sits at Gugu's upper right and
// Gugu, worried, looks at you and then hard at the bubble, again and again (two quick stamps each time its eyes go
// back to it): the gaze tells you where to look. When the answer turns yes, the bubble goes, a happy hop, back to normal.
typedef enum { INSTINCT_READY = 0, INSTINCT_NEED_SETUP, INSTINCT_WIFI_DOWN, INSTINCT_SERVICE_DOWN } instinct_ready_t;   // the bubble's icon: gear, Wi-Fi crossed out, ⚠
typedef instinct_ready_t (*instinct_ready_fn)(char *why, size_t cap, void *user);
void instinct_set_ready_probe(instinct_ready_fn fn, void *user);
void instinct_set_ready_override(int ready);  // -1 asks the probe; an instinct_ready_t forces the answer (console, tests)
void instinct_set_bubble_cb(void (*tapped)(void *user), void *user);   // the bubble was tapped: the app opens pairing mode

// ---- the friendly bubbles: "Hi!" (the self-introduction) and the light bulb (something to share, or a question) ----
// The app makes the lines (life) and hands each over when it is ready; instinct shows its bubble when no problem
// bubble is up, glances at it now and then, and plays the line when the bubble is tapped (the beak moving with it).
// A light bulb is asked for in the day, after a quiet spell (setting idea_idle_min, 20), at most ideas_per_day (3).
// The moon-and-book bubble (INSTINCT_OFFER_STORY, 2026-11-06) comes by itself at bedtime (setting bedtime_min, minutes
// after midnight, 1200 = 20:00; -1 never), once an evening, and a tap asks for a bedtime story (instinct_set_story_cb).
typedef enum { INSTINCT_OFFER_NONE = 0, INSTINCT_OFFER_HELLO, INSTINCT_OFFER_IDEA, INSTINCT_OFFER_STORY } instinct_offer_t;
void instinct_offer(instinct_offer_t kind, const char *wav_path, const char *text, bool question);
typedef struct {
    void (*want_idea)(void *user);                                                       // time for a light bulb: make a line
    void (*heard)(instinct_offer_t kind, const char *text, bool question, void *user);   // tapped and played
    void *user;
} instinct_offer_cbs_t;
void instinct_set_offer_cbs(const instinct_offer_cbs_t *cbs);
void instinct_hush(void);   // stop a line mid-way (the blue key went down)
// Stories asked for without words: the shelf at home (a book), the bedtime bubble. The app opens the line (call_story).
void instinct_set_story_cb(void (*want)(const char *request, bool bedtime, void *user), void *user);
// A bedtime story is over: Gugu falls asleep and the screen dims slowly; a touch, a key or a call wakes it all.
void instinct_goodnight(void);
void instinct_wake(void);                   // any key: the screen comes back (the pairing card must be readable)
