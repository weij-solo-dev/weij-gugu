// instinct.h — the pet when nobody is calling: idle presentation (blink, breathe, day/night room),
// touch and pick-up reactions. Behaviour only; it never talks to vendors.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t instinct_init(void);
void instinct_set_busy(bool in_call);       // app wires the call state here
void instinct_touched(int x, int y);        // app forwards touch events
