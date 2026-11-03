// foundation/motion.c — a shake is the magnitude of acceleration leaving 1 g with some force, again and
// again, within a short window (one-sided pumping counts: a hand-shaken device often only spikes); a pick-up
// is a lasting change of orientation that happens while nothing is shaking.
#include "foundation/motion.h"
#include <math.h>
#include <string.h>

#define SHAKE_THRESHOLD_G   0.45f   // |a| must get this far from 1 g (hand-shaking a 60 g device: 0.6–2 g)
#define SHAKE_EDGES         3       // excursions past the threshold inside the window that make a shake
#define SHAKE_WINDOW_MS     700
#define SHAKE_REPEAT_MS     300
#define PICKUP_TILT         0.35f   // change of the gravity direction (per axis, in g) that counts as a move
#define PICKUP_SETTLE_MS    1000

void motion_init(motion_t *m)
{
    memset(m, 0, sizeof *m);
    m->g_mag_lp = 1.0f;
}

motion_event_t motion_feed(motion_t *m, float ax, float ay, float az, int64_t t_ms, int *intensity)
{
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (!m->primed) { m->ax_lp = ax; m->ay_lp = ay; m->az_lp = az; m->g_mag_lp = mag; m->primed = true; m->window_start_ms = t_ms; return MOTION_NONE; }
    const float a = 0.08f;                 // the slow filters follow orientation; the rest is what the hand does
    m->g_mag_lp += (mag - m->g_mag_lp) * a;
    float hp = mag - 1.0f, dev = fabsf(hp);
    bool above = dev > SHAKE_THRESHOLD_G;
    if (t_ms - m->window_start_ms > SHAKE_WINDOW_MS) { m->window_start_ms = t_ms; m->swings = 0; m->peak = 0; }
    if (above) {
        m->last_move_ms = t_ms;
        if (dev > m->peak) m->peak = dev;
        if (m->last_hp <= SHAKE_THRESHOLD_G) m->swings++;      // a rising edge of |a - 1 g|
    }
    m->last_hp = dev;
    if (m->swings >= SHAKE_EDGES) {
        float peak = m->peak;
        m->swings = 0; m->window_start_ms = t_ms; m->peak = 0;
        if (t_ms - m->last_shake_ms >= SHAKE_REPEAT_MS) {
            m->last_shake_ms = t_ms;
            if (intensity) { int v = (int)(peak * 120.0f); *intensity = v > 255 ? 255 : v < 40 ? 40 : v; }
            return MOTION_SHAKE;
        }
        return MOTION_NONE;
    }
    // orientation: a lasting tilt, arriving in calm, means the device was picked up or put down
    float dx = ax - m->ax_lp, dy = ay - m->ay_lp, dz = az - m->az_lp;
    bool moved = fabsf(dx) > PICKUP_TILT || fabsf(dy) > PICKUP_TILT || fabsf(dz) > PICKUP_TILT;
    bool calm_now = dev < 0.25f;
    bool calm_before = t_ms - m->last_move_ms > PICKUP_SETTLE_MS && t_ms - m->last_shake_ms > PICKUP_SETTLE_MS;
    m->ax_lp += dx * a; m->ay_lp += dy * a; m->az_lp += dz * a;
    if (moved) {
        bool pickup = calm_before && calm_now && t_ms - m->last_pickup_ms > PICKUP_SETTLE_MS;
        m->last_move_ms = t_ms;
        if (pickup) { m->last_pickup_ms = t_ms; return MOTION_PICKUP; }
    }
    return MOTION_NONE;
}
