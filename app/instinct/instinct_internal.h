// instinct_internal.h — private to the instinct module: what instinct.c lends the show (instinct_show.c) and the show's
// hooks into instinct.c's keys, touches and loop.
#pragma once
#include "instinct/instinct.h"
#include "ui/ui.h"
#include <stdbool.h>
#include <stdint.h>

// instinct.c
void instinct_feed_stop(void);              // the food and its steps end on the spot, Gugu back home
void instinct_scene_back(void);             // the trip's (or home's) scene again, after the stage
void instinct_face(ui_emotion_t e, int ms); // a face for ms, then the resting one
bool instinct_night(void);                  // night where Gugu is (the time of day there, or the screenshot override)

// instinct_show.c
bool show_press(void);                      // a gift key press: true when it was the show's (every fifth starts one; during one it is a cheer)
void show_tick(int64_t now);                // the instinct task, every 40 ms
bool show_on(void);
void show_stop(const char *why);            // a call: the stage comes down at once
bool show_tap(void);                        // a touch during the show: applause; true when the show took it
