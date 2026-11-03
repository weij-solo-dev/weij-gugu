// foundation/motion.h — what the accelerometer samples mean: a shake (the child rattling the device), a
// pick-up (it leaves the table), rest. Pure arithmetic on a stream of samples, so the device's IMU task and
// the simulator's keys feed the same detector and the tests can drive it with made-up motion.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { MOTION_NONE = 0, MOTION_SHAKE, MOTION_PICKUP } motion_event_t;

typedef struct {
    float g_mag_lp;          // low-passed |a| in g (settles at 1.0 at rest)
    float ax_lp, ay_lp, az_lp;   // low-passed axes: the orientation
    int   swings;            // sign changes of the high-passed magnitude that were large enough, in the window
    int64_t window_start_ms, last_shake_ms, last_pickup_ms, last_move_ms;
    float last_hp;           // |a - 1 g| of the previous sample
    float peak;              // the largest excursion in the window (the shake's intensity)
    bool  primed;
} motion_t;

void motion_init(motion_t *m);
// One sample in g at time t_ms. Returns the event this sample completes (MOTION_NONE most of the time).
// *intensity (0..255) is set for a shake: how hard. A shake is reported at most every 300 ms while it goes
// on; a pick-up once per lift (the device must settle for a second before another counts).
motion_event_t motion_feed(motion_t *m, float ax, float ay, float az, int64_t t_ms, int *intensity);
