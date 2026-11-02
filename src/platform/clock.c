/*
 * Backend OS-services contract exposes monotonic and wall clocks outside
 * Kernel. KERNEL-007 forbids Kernel code from reading time or the environment,
 * so the clock remains a Backend-owned platform service and Kernel stays
 * deterministic from its inputs.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/kernel/overview.md#determinism
 */
#include "clock.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <time.h>
uint64_t cdin_clock_monotonic_ns(void)
{
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) return 0;
    {
        uint64_t ticks = (uint64_t)counter.QuadPart;
        uint64_t ticks_per_second = (uint64_t)frequency.QuadPart;
        uint64_t seconds = ticks / ticks_per_second;
        uint64_t remainder = ticks % ticks_per_second;
        return seconds * UINT64_C(1000000000) +
               (remainder * UINT64_C(1000000000)) / ticks_per_second;
    }
}
int64_t cdin_clock_wall_seconds(void)
{
    return (int64_t)time(NULL);
}
#else
#include <time.h>
#include <sys/time.h>
uint64_t cdin_clock_monotonic_ns(void)
{
    struct timespec value;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
#else
    if (timespec_get(&value, TIME_UTC) != TIME_UTC) return 0;
#endif
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
int64_t cdin_clock_wall_seconds(void)
{
    return (int64_t)time(NULL);
}
#endif
