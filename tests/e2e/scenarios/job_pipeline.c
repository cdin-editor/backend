#include "cdin/backend/jobs.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "scenario.h"

typedef struct work_context {
    atomic_bool *release_gate;
    atomic_int *started;
    int value;
} work_context;

typedef struct completion_state {
    atomic_int completed;
    atomic_int cancelled;
    atomic_int failed;
    atomic_int successful;
    atomic_int result_sum;
} completion_state;

static cdin_backend_job_result calculate(void *opaque, const atomic_bool *cancelled,
                                         void **out_result)
{
    work_context *context = (work_context *)opaque;
    int *result;

    atomic_fetch_add_explicit(context->started, 1, memory_order_relaxed);
    while (!atomic_load_explicit(context->release_gate, memory_order_acquire) &&
           !atomic_load_explicit(cancelled, memory_order_relaxed)) {
        scenario_sleep_ms(1u);
    }
    if (atomic_load_explicit(cancelled, memory_order_relaxed)) {
        return CDIN_BACKEND_JOB_CANCELLED;
    }

    /* Value 7 models an application-level failure after work has started. */
    if (context->value == 7) return CDIN_BACKEND_JOB_FAILED;
    result = (int *)malloc(sizeof(*result));
    if (result == NULL) return CDIN_BACKEND_JOB_FAILED;
    *result = context->value * 2;
    *out_result = result;
    return CDIN_BACKEND_JOB_OK;
}

static void completed(void *opaque, cdin_backend_job_result status, void *result)
{
    completion_state *state = (completion_state *)opaque;

    if (status == CDIN_BACKEND_JOB_CANCELLED) {
        atomic_fetch_add_explicit(&state->cancelled, 1, memory_order_relaxed);
    } else if (status == CDIN_BACKEND_JOB_FAILED) {
        atomic_fetch_add_explicit(&state->failed, 1, memory_order_relaxed);
    } else if (status == CDIN_BACKEND_JOB_OK && result != NULL) {
        atomic_fetch_add_explicit(&state->successful, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&state->result_sum, *(int *)result, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&state->completed, 1, memory_order_release);
}

static void release_result(void *value)
{
    free(value);
}

static void initialize_state(completion_state *state)
{
    atomic_init(&state->completed, 0);
    atomic_init(&state->cancelled, 0);
    atomic_init(&state->failed, 0);
    atomic_init(&state->successful, 0);
    atomic_init(&state->result_sum, 0);
}

int main(void)
{
    enum { JOB_COUNT = 8, WORKER_COUNT = 3 };
    cdin_backend_job_pool *pool = NULL;
    cdin_backend_job_options options;
    cdin_backend_job_token *tokens[JOB_COUNT];
    atomic_bool release_gate;
    atomic_int started;
    completion_state completions;
    int waited;
    int index;

    memset(tokens, 0, sizeof(tokens));
    atomic_init(&release_gate, 0);
    atomic_init(&started, 0);
    initialize_state(&completions);

    /* A bounded multi-worker queue simulates concurrent indexing requests. */
    SCENARIO_STATUS(cdin_backend_job_pool_create(WORKER_COUNT, JOB_COUNT, &pool), 0);
    memset(&options, 0, sizeof(options));
    options.run = calculate;
    options.complete = completed;
    options.destroy_context = free;
    options.destroy_result = release_result;
    options.owner = &completions;

    for (index = 0; index < JOB_COUNT; index++) {
        work_context *context = (work_context *)calloc(1u, sizeof(*context));
        SCENARIO_CHECK(context != NULL);
        context->release_gate = &release_gate;
        context->started = &started;
        context->value = index + 1;
        options.context = context;
        SCENARIO_STATUS(cdin_backend_job_submit(pool, &options, &tokens[index]), 0);
        SCENARIO_CHECK(tokens[index] != NULL);
    }

    /* Queue capacity is a hard memory bound, not an advisory threshold. */
    options.context = NULL;
    {
        cdin_backend_job_token *rejected = NULL;
        SCENARIO_STATUS(cdin_backend_job_submit(pool, &options, &rejected), -3);
        SCENARIO_CHECK(rejected == NULL);
    }

    for (waited = 0; waited < 2000 &&
         atomic_load_explicit(&started, memory_order_acquire) < WORKER_COUNT; waited++) {
        scenario_sleep_ms(1u);
    }
    SCENARIO_CHECK(atomic_load_explicit(&started, memory_order_acquire) == WORKER_COUNT);

    /* Cancel one running and one queued job before releasing the worker gate. */
    cdin_backend_job_cancel(tokens[0]);
    cdin_backend_job_cancel(tokens[5]);
    atomic_store_explicit(&release_gate, 1, memory_order_release);

    /*
     * The done queue is smaller than the total accepted workload. Shutdown
     * must pump callbacks while workers drain; joining first would deadlock.
     */
    SCENARIO_STATUS(cdin_backend_job_pool_shutdown(pool, 0), 0);
    SCENARIO_CHECK(atomic_load_explicit(&completions.completed, memory_order_acquire) == JOB_COUNT);
    SCENARIO_CHECK(atomic_load_explicit(&completions.cancelled, memory_order_relaxed) == 2);
    SCENARIO_CHECK(atomic_load_explicit(&completions.failed, memory_order_relaxed) == 1);
    SCENARIO_CHECK(atomic_load_explicit(&completions.successful, memory_order_relaxed) == 5);
    SCENARIO_CHECK(atomic_load_explicit(&completions.result_sum, memory_order_relaxed) == 44);
    SCENARIO_CHECK(cdin_backend_job_pending(pool) == 0u);
    cdin_backend_job_pool_destroy(pool);

    (void)puts("PASS job_pipeline: concurrent jobs, capacity rejection, running/queued cancellation, worker failure and graceful completion drain");
    return 0;
}
