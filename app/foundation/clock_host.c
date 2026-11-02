#include "foundation/clock.h"
#include <time.h>

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t clock_us(void)
{
    static int64_t t0 = 0;
    if (t0 == 0) t0 = now_us();
    return now_us() - t0;
}

void clock_sleep_ms(uint32_t ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
