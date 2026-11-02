#ifndef CDIN_BACKEND_CLOCK_H
#define CDIN_BACKEND_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic nanoseconds are for elapsed-time measurements, not timestamps. */
uint64_t cdin_clock_monotonic_ns(void);
/* Wall-clock seconds are suitable for persisted timestamps and user display. */
int64_t cdin_clock_wall_seconds(void);

#ifdef __cplusplus
}
#endif

#endif
