/*
 * Stable public facade for the bounded job system. The internal worker pool
 * stays private so its synchronization structures can change without ABI
 * churn in consumers that reuse Backend services.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/concurrency.md
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 */
#include "pool.h"

int cdin_backend_job_pool_create(size_t worker_count, size_t capacity,
                                 cdin_backend_job_pool **out_pool)
{
    return cdin_job_pool_create(worker_count, capacity, NULL, NULL, (cdin_job_pool **)out_pool);
}

int cdin_backend_job_submit(cdin_backend_job_pool *pool,
                            const cdin_backend_job_options *options,
                            cdin_backend_job_token **out_token)
{
    return cdin_job_submit((cdin_job_pool *)pool, options, (cdin_job_token **)out_token);
}

void cdin_backend_job_cancel(cdin_backend_job_token *token)
{
    cdin_job_cancel((cdin_job_token *)token);
}

size_t cdin_backend_job_pump(cdin_backend_job_pool *pool, size_t max_completions)
{
    return cdin_job_pump((cdin_job_pool *)pool, max_completions);
}

size_t cdin_backend_job_pending(const cdin_backend_job_pool *pool)
{
    return cdin_job_pending((const cdin_job_pool *)pool);
}

int cdin_backend_job_pool_shutdown(cdin_backend_job_pool *pool, int abort_pending)
{
    return cdin_job_pool_shutdown((cdin_job_pool *)pool, abort_pending);
}

void cdin_backend_job_pool_destroy(cdin_backend_job_pool *pool)
{
    cdin_job_pool_destroy((cdin_job_pool *)pool);
}
