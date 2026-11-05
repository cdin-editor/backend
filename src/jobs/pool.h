#ifndef CDIN_BACKEND_JOBS_POOL_H
#define CDIN_BACKEND_JOBS_POOL_H

#include "cdin/backend/jobs.h"
#include "../platform/thread.h"

/* Private names keep implementation files short without duplicating the ABI. */
typedef cdin_backend_job_pool cdin_job_pool;
typedef cdin_backend_job_token cdin_job_token;
typedef cdin_backend_job_result cdin_job_result;
typedef cdin_backend_job_run_fn cdin_job_run_fn;
typedef cdin_backend_job_complete_fn cdin_job_complete_fn;
typedef cdin_backend_job_destroy_fn cdin_job_destroy_fn;
typedef cdin_backend_job_options cdin_job_options;

#define CDIN_JOB_OK CDIN_BACKEND_JOB_OK
#define CDIN_JOB_FAILED CDIN_BACKEND_JOB_FAILED
#define CDIN_JOB_CANCELLED CDIN_BACKEND_JOB_CANCELLED

typedef void (*cdin_job_wakeup_fn)(void *context);

int cdin_job_pool_create(size_t thread_count, size_t capacity,
                         cdin_job_wakeup_fn wakeup, void *wakeup_context,
                         cdin_job_pool **out_pool);
int cdin_job_submit(cdin_job_pool *pool, const cdin_job_options *options, cdin_job_token **out_token);
void cdin_job_cancel(cdin_job_token *token);
size_t cdin_job_pump(cdin_job_pool *pool, size_t max_completions);
size_t cdin_job_pending(const cdin_job_pool *pool);
int cdin_job_pool_shutdown(cdin_job_pool *pool, int abort_pending);
void cdin_job_pool_destroy(cdin_job_pool *pool);

#endif
