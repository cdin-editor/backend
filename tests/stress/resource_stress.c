/*
 * Resource-pressure and memory-bound test for the portable Backend job pool.
 *
 * Requirement references:
 * - BE-008 / BE-009: https://github.com/cdin-editor/spec/blob/c4de9bf3d506b4e1f07b8986531cd056341708b0/docs/components/backend.md#L71-L74
 * - ARCH-021 / ARCH-023: https://github.com/cdin-editor/spec/blob/c4de9bf3d506b4e1f07b8986531cd056341708b0/docs/architecture/overview.md#L131-L133
 * - TEST-010 / TEST-011 / TEST-013: https://github.com/cdin-editor/spec/blob/c4de9bf3d506b4e1f07b8986531cd056341708b0/docs/testing/conformance.md#L79-L82
 *
 * This test deliberately submits faster than workers can finish. It verifies
 * that outstanding work never exceeds the configured capacity, overload is
 * reported as backpressure, all accepted jobs are eventually completed, and
 * memory returns close to its steady-state level after the pool is drained.
 */
#include "cdin/backend/jobs.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#  include <errno.h>
#endif

#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach/mach.h>
#  endif
#endif

typedef struct stress_context {
    uint64_t sequence;
} stress_context;

typedef struct stress_result {
    uint64_t checksum;
} stress_result;

typedef struct stress_stats {
    atomic_uint_fast64_t completed;
    atomic_uint_fast64_t failed;
    atomic_uint_fast64_t cancelled;
} stress_stats;

typedef struct options {
    uint64_t seconds;
    size_t workers;
    size_t capacity;
    uint64_t max_rss_mib;
    const char *output_path;
} options;

static uint64_t monotonic_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter)) return 0u;
    {
        uint64_t ticks = (uint64_t)counter.QuadPart;
        uint64_t hz = (uint64_t)frequency.QuadPart;
        return (ticks / hz) * UINT64_C(1000000000) +
               ((ticks % hz) * UINT64_C(1000000000)) / hz;
    }
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0u;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
#endif
}

static uint64_t current_rss_bytes(void)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = (DWORD)sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters,
                              (DWORD)sizeof(counters))) return 0u;
    return (uint64_t)counters.WorkingSetSize;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    memset(&info, 0, sizeof(info));
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) return 0u;
    return (uint64_t)info.resident_size;
#else
    FILE *stream = fopen("/proc/self/statm", "r");
    unsigned long total_pages = 0ul, resident_pages = 0ul;
    long page_size;
    if (stream != NULL) {
        int read_ok = fscanf(stream, "%lu %lu", &total_pages, &resident_pages);
        (void)fclose(stream);
        page_size = sysconf(_SC_PAGESIZE);
        if (read_ok == 2 && page_size > 0) return (uint64_t)resident_pages * (uint64_t)page_size;
    }
    {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) != 0) return 0u;
#  if defined(__APPLE__)
        return (uint64_t)usage.ru_maxrss;
#  else
        return (uint64_t)usage.ru_maxrss * 1024u;
#  endif
    }
#endif
}

static void sleep_one_millisecond(void)
{
#ifdef _WIN32
    Sleep(1u);
#else
    struct timespec pause = { 0, 1000000L };
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) { }
#endif
}

static cdin_backend_job_result run_work(void *opaque, const atomic_bool *cancelled,
                                        void **out_result)
{
    const stress_context *context = (const stress_context *)opaque;
    const size_t payload_size = 32u * 1024u;
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    stress_result *result;
    uint64_t checksum = UINT64_C(1469598103934665603) ^ context->sequence;
    size_t i;
    unsigned int round;

    if (payload == NULL) return CDIN_BACKEND_JOB_FAILED;
    for (round = 0u; round < 8u; round++) {
        if (atomic_load_explicit(cancelled, memory_order_relaxed)) {
            free(payload);
            return CDIN_BACKEND_JOB_CANCELLED;
        }
        for (i = 0u; i < payload_size; i++) {
            payload[i] = (uint8_t)((i + (size_t)context->sequence + round) & 0xffu);
            checksum ^= payload[i];
            checksum *= UINT64_C(1099511628211);
        }
    }
    free(payload);

    result = (stress_result *)malloc(sizeof(*result));
    if (result == NULL) return CDIN_BACKEND_JOB_FAILED;
    result->checksum = checksum;
    *out_result = result;
    return CDIN_BACKEND_JOB_OK;
}

static void on_complete(void *opaque, cdin_backend_job_result result, void *value)
{
    stress_stats *stats = (stress_stats *)opaque;
    if (result == CDIN_BACKEND_JOB_OK && value != NULL) {
        (void)((stress_result *)value)->checksum;
    } else if (result == CDIN_BACKEND_JOB_CANCELLED) {
        atomic_fetch_add_explicit(&stats->cancelled, 1u, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&stats->failed, 1u, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&stats->completed, 1u, memory_order_release);
}

static void release_value(void *value) { free(value); }

static int parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;
    if (text == NULL || *text == '\0' || out == NULL) return 0;
    value = strtoull(text, &end, 10);
    if (end == text || *end != '\0') return 0;
    *out = (uint64_t)value;
    return 1;
}

static int parse_size(const char *text, size_t *out)
{
    uint64_t value;
    if (!parse_u64(text, &value) || value == 0u || value > (uint64_t)SIZE_MAX) return 0;
    *out = (size_t)value;
    return 1;
}

static int parse_options(int argc, char **argv, options *out)
{
    int i;
    out->seconds = 10u;
    out->workers = 4u;
    out->capacity = 128u;
    out->max_rss_mib = 0u;
    out->output_path = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &out->seconds) || out->seconds == 0u || out->seconds > 3600u) return 0;
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &out->workers) || out->workers > 16u) return 0;
        } else if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &out->capacity) || out->capacity > 65536u) return 0;
        } else if (strcmp(argv[i], "--max-rss-mib") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &out->max_rss_mib) || out->max_rss_mib > 1048576u) return 0;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            out->output_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            (void)puts("Usage: resource_stress [--seconds N] [--workers N] [--capacity N] [--max-rss-mib N] [--output FILE]");
            exit(0);
        } else {
            return 0;
        }
    }
    return 1;
}

static int write_report(const char *path, uint64_t duration_ns, uint64_t submitted,
                        uint64_t completed, uint64_t backpressure, uint64_t high_water,
                        uint64_t peak_rss, uint64_t final_rss, uint64_t initial_rss, size_t workers,
                        size_t capacity, uint64_t failures)
{
    FILE *stream;
    int close_status;
    if (path == NULL) stream = stdout;
    else stream = fopen(path, "w");
    if (stream == NULL) return 0;
    (void)fprintf(stream,
        "{\n"
        "  \"duration_seconds\": %.3f,\n"
        "  \"workers\": %zu,\n"
        "  \"queue_capacity\": %zu,\n"
        "  \"submitted\": \"%" PRIu64 "\",\n"
        "  \"completed\": \"%" PRIu64 "\",\n"
        "  \"jobs_per_second\": %.3f,\n"
        "  \"backpressure_events\": \"%" PRIu64 "\",\n"
        "  \"max_outstanding\": \"%" PRIu64 "\",\n"
        "  \"initial_rss_bytes\": \"%" PRIu64 "\",\n"
        "  \"peak_rss_bytes\": \"%" PRIu64 "\",\n"
        "  \"final_rss_bytes\": \"%" PRIu64 "\",\n"
        "  \"failed_jobs\": \"%" PRIu64 "\"\n"
        "}\n",
        (double)duration_ns / 1e9, workers, capacity,
        submitted, completed,
        duration_ns == 0u ? 0.0 : (double)completed * 1e9 / (double)duration_ns,
        backpressure, high_water, initial_rss, peak_rss, final_rss, failures);
    close_status = path == NULL ? 0 : fclose(stream);
    return close_status == 0;
}

int main(int argc, char **argv)
{
    options config;
    cdin_backend_job_pool *pool = NULL;
    cdin_backend_job_options job_options;
    stress_stats stats;
    uint64_t start_ns, end_ns, peak_rss, initial_rss, final_rss;
    uint64_t submitted = 0u, backpressure = 0u, high_water = 0u;
    uint64_t completed, failures;
    size_t outstanding;
    int exit_code = 0;

    if (!parse_options(argc, argv, &config)) {
        (void)fprintf(stderr, "Invalid arguments. Use --help.\n");
        return 2;
    }
    memset(&job_options, 0, sizeof(job_options));
    atomic_init(&stats.completed, 0u);
    atomic_init(&stats.failed, 0u);
    atomic_init(&stats.cancelled, 0u);

    if (cdin_backend_job_pool_create(config.workers, config.capacity, &pool) != 0) {
        (void)fprintf(stderr, "Could not create bounded job pool.\n");
        return 1;
    }
    job_options.run = run_work;
    job_options.complete = on_complete;
    job_options.destroy_context = release_value;
    job_options.destroy_result = release_value;
    job_options.owner = &stats;

    initial_rss = current_rss_bytes();
    peak_rss = initial_rss;
    start_ns = monotonic_ns();
    if (start_ns == 0u) {
        (void)fprintf(stderr, "Monotonic clock unavailable.\n");
        exit_code = 1;
        goto shutdown;
    }

    while (monotonic_ns() - start_ns < config.seconds * UINT64_C(1000000000)) {
        size_t pumped;
        outstanding = cdin_backend_job_pending(pool);
        if ((uint64_t)outstanding > high_water) high_water = (uint64_t)outstanding;

        /* Fill the bounded pool aggressively to exercise queue backpressure. */
        while (outstanding < config.capacity) {
            stress_context *context = (stress_context *)malloc(sizeof(*context));
            cdin_backend_job_token *token = NULL;
            int status;
            if (context == NULL) {
                (void)fprintf(stderr, "Context allocation failed.\n");
                exit_code = 1;
                goto shutdown;
            }
            context->sequence = submitted + 1u;
            job_options.context = context;
            status = cdin_backend_job_submit(pool, &job_options, &token);
            if (status == -3) {
                free(context);
                backpressure++;
                break;
            }
            if (status != 0 || token == NULL) {
                free(context);
                (void)fprintf(stderr, "Job submission failed: %d\n", status);
                exit_code = 1;
                goto shutdown;
            }
            submitted++;
            outstanding++;
            if ((uint64_t)outstanding > high_water) high_water = (uint64_t)outstanding;
        }

        /* Probe the full queue once so the test proves overload is observable. */
        if (outstanding == config.capacity) {
            stress_context *probe = (stress_context *)malloc(sizeof(*probe));
            cdin_backend_job_token *rejected = NULL;
            int status;
            if (probe == NULL) {
                (void)fprintf(stderr, "Backpressure probe allocation failed.\n");
                exit_code = 1;
                goto shutdown;
            }
            probe->sequence = submitted + 1u;
            job_options.context = probe;
            status = cdin_backend_job_submit(pool, &job_options, &rejected);
            if (status == -3 && rejected == NULL) {
                free(probe);
                backpressure++;
            } else {
                /* An accepted probe is still owned by the pool and is counted. */
                if (status == 0 && rejected != NULL) {
                    submitted++;
                    outstanding++;
                } else {
                    free(probe);
                    (void)fprintf(stderr, "Unexpected full-queue probe result: %d\n", status);
                    exit_code = 1;
                    goto shutdown;
                }
            }
        }

        pumped = cdin_backend_job_pump(pool, 32u);
        (void)pumped;
        outstanding = cdin_backend_job_pending(pool);
        if ((uint64_t)outstanding > config.capacity) {
            (void)fprintf(stderr, "Unbounded outstanding jobs: %zu > %zu\n", outstanding, config.capacity);
            exit_code = 1;
            goto shutdown;
        }
        {
            uint64_t rss = current_rss_bytes();
            if (rss > peak_rss) peak_rss = rss;
        }
        sleep_one_millisecond();
    }

shutdown:
    if (pool != NULL) {
        if (cdin_backend_job_pool_shutdown(pool, exit_code != 0) != 0) {
            (void)fprintf(stderr, "Job pool shutdown failed.\n");
            exit_code = 1;
        }
        completed = atomic_load_explicit(&stats.completed, memory_order_acquire);
        failures = atomic_load_explicit(&stats.failed, memory_order_relaxed);
        outstanding = cdin_backend_job_pending(pool);
        final_rss = current_rss_bytes();
        if (final_rss > peak_rss) peak_rss = final_rss;

        if (outstanding != 0u || completed != submitted ||
            atomic_load_explicit(&stats.cancelled, memory_order_relaxed) != 0u ||
            high_water > config.capacity || backpressure == 0u) {
            (void)fprintf(stderr,
                "Invariant failure: submitted=%" PRIu64 " completed=%" PRIu64
                " outstanding=%zu backpressure=%" PRIu64 " high_water=%" PRIu64 " capacity=%zu\n",
                submitted, completed, outstanding, backpressure, high_water, config.capacity);
            exit_code = 1;
        }
        if (failures != 0u) exit_code = 1;
        if (config.max_rss_mib != 0u && peak_rss > config.max_rss_mib * UINT64_C(1024) * 1024u) {
            (void)fprintf(stderr, "Peak RSS exceeded threshold: %" PRIu64 " bytes > %" PRIu64 " MiB\n",
                          peak_rss, config.max_rss_mib);
            exit_code = 1;
        }
        end_ns = monotonic_ns();
        if (!write_report(config.output_path, end_ns - start_ns, submitted, completed,
                          backpressure, high_water, peak_rss, final_rss, initial_rss,
                          config.workers, config.capacity, failures)) {
            (void)fprintf(stderr, "Could not write benchmark report.\n");
            exit_code = 1;
        }
        cdin_backend_job_pool_destroy(pool);
    }
    (void)initial_rss;
    return exit_code;
}
