// foundation/clock.h — monotonic time and sleeping, identical on device and host.
#pragma once
#include <stdint.h>

int64_t clock_us(void);                       // monotonic microseconds since boot / process start
static inline uint32_t clock_ms(void) { return (uint32_t)(clock_us() / 1000); }
void clock_sleep_ms(uint32_t ms);
