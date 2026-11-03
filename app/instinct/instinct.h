// instinct.h — the pet when nobody is calling: idle presentation (blink, breathe, day/night room),
// touch and pick-up reactions. Behaviour only; it never talks to vendors.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t instinct_init(void);
void instinct_set_busy(bool in_call);       // app wires the call state here
void instinct_touched(int x, int y);        // app forwards touch events
void instinct_set_tod_override(int tod);    // -1 follows the clock; else a world_tod_t, for screenshots and tests
void instinct_yellow_tap(void);             // the yellow key's play: a pigeon whistle it answers; press it a lot and it gets dizzy, then cross
void instinct_shaken(int intensity);        // IMU: rolls into a ball and bounces off the rim; when it stops, dizzy, then cross, then fine
void instinct_picked_up(void);              // IMU: looks up at you
void instinct_refuse(const char *why);      // the blue key when a call cannot start (offline, no key): a buzz, a head shake, a sad face
// Can Gugu make a call right now? The app answers (only it sees the network and the keys); NULL = always yes.
// While the answer stays no for PUZZLED_AFTER, a thought bubble with a settings gear sits at Gugu's upper right and
// Gugu, worried, looks at you and then hard at the bubble, again and again (two quick stamps each time its eyes go
// back to it): the gaze tells you where to look. When the answer turns yes, the bubble goes, a happy hop, back to normal.
typedef enum { INSTINCT_READY = 0, INSTINCT_NEED_SETUP, INSTINCT_WIFI_DOWN, INSTINCT_SERVICE_DOWN } instinct_ready_t;   // the bubble's icon: gear, Wi-Fi crossed out, ⚠
typedef instinct_ready_t (*instinct_ready_fn)(char *why, size_t cap, void *user);
void instinct_set_ready_probe(instinct_ready_fn fn, void *user);
void instinct_set_ready_override(int ready);  // -1 asks the probe; an instinct_ready_t forces the answer (console, tests)
void instinct_set_bubble_cb(void (*tapped)(void *user), void *user);   // the bubble was tapped: the app opens pairing mode
