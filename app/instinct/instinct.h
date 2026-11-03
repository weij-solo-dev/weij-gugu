// instinct.h — the pet when nobody is calling: idle presentation (blink, breathe, day/night room),
// touch and pick-up reactions. Behaviour only; it never talks to vendors.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t instinct_init(void);
void instinct_set_busy(bool in_call);       // app wires the call state here
void instinct_touched(int x, int y);        // app forwards touch events
void instinct_set_tod_override(int tod);    // -1 follows the clock; else a world_tod_t, for screenshots and tests
void instinct_yellow_tap(void);             // the yellow key's play: a pigeon whistle it answers; press it a lot and it gets dizzy, then cross
void instinct_shaken(int intensity);        // IMU: rolls into a ball and bounces off the rim; when it stops, dizzy, then cross, then fine
void instinct_picked_up(void);              // IMU: looks up at you
