// foundation/lipsync.h — the beak follows a voice. In: the voice's loudness (0–255, a 20 ms frame's RMS / 32 before the
// volume; audio_play_level) every 40 ms or so; out: the beak open or shut. Loud is measured against the voice itself, so
// a quiet voice moves the beak as much as a loud one (2026-11-06, the owner: "gugu 回复时，嘴巴可以动起来": Gugu's own voice
// read about 8 against a fixed threshold of 28, so the beak never moved): a third of its recent loudest, which fades by
// half in about a second, and above a floor that pauses and breath stay under. A syllable after a pause opens the beak at
// once; while the voice goes on it flaps, open 120 ms, shut 80 ms (five times a second, about the pace of syllables); a
// pause shuts it once it has been open 60 ms. Pure arithmetic, so the tests drive it with made-up voices.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LIPSYNC_FLOOR 8          // RMS 256, −42 dBFS: quieter than this is a pause
typedef struct { uint8_t peak; bool loud, open; uint32_t since; } lipsync_t;

void lipsync_init(lipsync_t *s);
bool lipsync_step(lipsync_t *s, uint8_t level, uint32_t now_ms);   // true: the beak is open now
