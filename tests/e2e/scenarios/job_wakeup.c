#include "pool.h"
#include "scenario.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static cdin_job_result complete_work(void *context, const atomic_bool *cancelled,
                                     void **out_result)
{
    (void)context;
    (void)cancelled;
    *out_result = NULL;
    return CDIN_JOB_OK;
}

static void signal_wakeup(void *context)
{
    atomic_int *count = (atomic_int *)context;
    atomic_fetch_add_explicit(count, 1, memory_order_release);
}

int main(void)
{
    cdin_job_pool *pool = NULL;
    cdin_job_options options;
    cdin_job_token *token = NULL;
    atomic_int wakeups;
    int waited;

    atomic_init(&wakeups, 0);
    SCENARIO_STATUS(cdin_job_pool_create(1u, 1u, signal_wakeup, &wakeups, &pool), 0);

    memset(&options, 0, sizeof(options));
    options.run = complete_work;
    SCENARIO_STATUS(cdin_job_submit(pool, &options, &token), 0);
    SCENARIO_CHECK(token != NULL);

    /* Worker completion must wake a sleeping owner without polling the pool. */
    for (waited = 0; waited < 2000 &&
         atomic_load_explicit(&wakeups, memory_order_acquire) == 0; waited++) {
        scenario_sleep_ms(1u);
    }
    SCENARIO_CHECK(atomic_load_explicit(&wakeups, memory_order_acquire) == 1);
    SCENARIO_CHECK(cdin_job_pending(pool) == 1u);
    SCENARIO_CHECK(cdin_job_pump(pool, 1u) == 1u);
    SCENARIO_CHECK(cdin_job_pending(pool) == 0u);
    SCENARIO_STATUS(cdin_job_pool_shutdown(pool, 0), 0);
    cdin_job_pool_destroy(pool);

    (void)puts("PASS job_wakeup: worker completion signals the owner after queue publication");
    return 0;
}
