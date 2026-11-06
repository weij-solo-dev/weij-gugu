// lipsync.c — see lipsync.h.
#include "foundation/lipsync.h"

#define OPEN_MS     120          // while the voice goes on: open this long …
#define SHUT_MS      80          // … then shut this long
#define MIN_OPEN_MS  60          // a syllable shows, however short

void lipsync_init(lipsync_t *s) { s->peak = 0; s->loud = false; s->open = false; s->since = 0; }

bool lipsync_step(lipsync_t *s, uint8_t level, uint32_t now_ms)
{
    int fade = s->peak >= 32 ? s->peak / 32 : s->peak ? 1 : 0;      // about 3 % a step: half gone in a second at 40 ms a step
    int peak = s->peak - fade;
    s->peak = (uint8_t)(level > peak ? level : peak);
    bool loud = level >= LIPSYNC_FLOOR && (s->loud ? level * 5 >= s->peak : level * 3 >= s->peak);   // in at a third, out under a fifth
    uint32_t held = now_ms - s->since;
    if (loud && !s->loud) { s->open = true; s->since = now_ms; }   // a syllable after a pause: at once
    else if (loud) {
        if (s->open && held >= OPEN_MS) { s->open = false; s->since = now_ms; }
        else if (!s->open && held >= SHUT_MS) { s->open = true; s->since = now_ms; }
    } else if (s->open && held >= MIN_OPEN_MS) { s->open = false; s->since = now_ms; }
    s->loud = loud;
    return s->open;
}
