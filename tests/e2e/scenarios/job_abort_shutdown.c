#include "cdin/backend/jobs.h"
#include "platform/thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "scenario.h"

typedef struct abort_context {
    atomic_bool *started;
    atomic_bool *release_gate;
    atomic_int *destroyed;
} abort_context;

typedef struct release_context {
    atomic_bool *release_gate;
} release_context;

static cdin_backend_job_result wait_until_released(void *opaque,
                                                   const atomic_bool *cancelled,
                                                   void **out_result)
{
    abort_context *context = (abort_context *)opaque;
    (void)out_result;
    atomic_store_explicit(context->started, 1, memory_order_release);
    while (!atomic_load_explicit(context->release_gate, memory_order_acquire) &&
           !atomic_load_explicit(cancelled, memory_order_relaxed)) {
        scenario_sleep_ms(1u);
    }
    return atomic_load_explicit(cancelled, memory_order_relaxed)
        ? CDIN_BACKEND_JOB_CANCELLED : CDIN_BACKEND_JOB_OK;
}

static void release_job_context(void *opaque)
{
    abort_context *context = (abort_context *)opaque;
    if (context != NULL) {
        atomic_fetch_add_explicit(context->destroyed, 1, memory_order_relaxed);
        free(context);
    }
}

static void never_called(void *opaque, cdin_backend_job_result status, void *result)
{
    atomic_int *completed = (atomic_int *)opaque;
    (void)status;
    (void)result;
    atomic_fetch_add_explicit(completed, 1, memory_order_relaxed);
}

static CDIN_THREAD_RETURN CDIN_THREAD_CALL release_worker(void *opaque)
{
    release_context *context = (release_context *)opaque;
    scenario_sleep_ms(40u);
    atomic_store_explicit(context->release_gate, 1, memory_order_release);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(void)
{
    enum { JOB_COUNT = 4 };
    cdin_backend_job_pool *pool = NULL;
    cdin_backend_job_options options;
    cdin_backend_job_token *tokens[JOB_COUNT];
    atomic_bool worker_started;
    atomic_bool release_gate;
    atomic_int destroyed;
    atomic_int completed;
    release_context controller;
    cdin_thread release_thread;
    int waited;
    int index;

    memset(tokens, 0, sizeof(tokens));
    atomic_init(&worker_started, 0);
    atomic_init(&release_gate, 0);
    atomic_init(&destroyed, 0);
    atomic_init(&completed, 0);
    controller.release_gate = &release_gate;

    SCENARIO_STATUS(cdin_backend_job_pool_create(1u, JOB_COUNT, &pool), 0);
    memset(&options, 0, sizeof(options));
    options.run = wait_until_released;
    options.complete = never_called;
    options.destroy_context = release_job_context;
    options.owner = &completed;

    for (index = 0; index < JOB_COUNT; index++) {
        abort_context *context = (abort_context *)calloc(1u, sizeof(*context));
        SCENARIO_CHECK(context != NULL);
        context->started = &worker_started;
        context->release_gate = &release_gate;
        context->destroyed = &destroyed;
        options.context = context;
        SCENARIO_STATUS(cdin_backend_job_submit(pool, &options, &tokens[index]), 0);
    }
    for (waited = 0; waited < 2000 &&
         !atomic_load_explicit(&worker_started, memory_order_acquire); waited++) {
        scenario_sleep_ms(1u);
    }
    SCENARIO_CHECK(atomic_load_explicit(&worker_started, memory_order_acquire));

    /* Release the running worker from a separate thread while the owner is in
     * abort shutdown. This exercises cleanup without relying on a test sleep. */
    SCENARIO_STATUS(cdin_thread_create(&release_thread, release_worker, &controller), 0);
    SCENARIO_STATUS(cdin_backend_job_pool_shutdown(pool, 1), 0);
    cdin_thread_join(release_thread);

    SCENARIO_CHECK(cdin_backend_job_pending(pool) == 0u);
    SCENARIO_CHECK(atomic_load_explicit(&destroyed, memory_order_relaxed) == JOB_COUNT);
    SCENARIO_CHECK(atomic_load_explicit(&completed, memory_order_relaxed) == 0);

    options.context = NULL;
    {
        cdin_backend_job_token *rejected = NULL;
        SCENARIO_STATUS(cdin_backend_job_submit(pool, &options, &rejected), -2);
        SCENARIO_CHECK(rejected == NULL);
    }
    cdin_backend_job_pool_destroy(pool);
    (void)puts("PASS job_abort_shutdown: in-flight worker exit, queued-job disposal, callback suppression, destructor completion and post-shutdown rejection");
    return 0;
}
