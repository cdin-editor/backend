#ifndef CDIN_BACKEND_JOBS_H
#define CDIN_BACKEND_JOBS_H

#include <stdatomic.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdin_backend_job_pool cdin_backend_job_pool;
typedef struct cdin_backend_job_token cdin_backend_job_token;

typedef enum cdin_backend_job_result {
    CDIN_BACKEND_JOB_OK = 0,
    CDIN_BACKEND_JOB_FAILED,
    CDIN_BACKEND_JOB_CANCELLED
} cdin_backend_job_result;

typedef cdin_backend_job_result (*cdin_backend_job_run_fn)(
    void *context, const atomic_bool *cancelled, void **out_result);
typedef void (*cdin_backend_job_complete_fn)(
    void *owner, cdin_backend_job_result status, void *result);
typedef void (*cdin_backend_job_destroy_fn)(void *value);

typedef struct cdin_backend_job_options {
    cdin_backend_job_run_fn run;
    cdin_backend_job_complete_fn complete;
    cdin_backend_job_destroy_fn destroy_context;
    cdin_backend_job_destroy_fn destroy_result;
    void *context;
    void *owner;
} cdin_backend_job_options;

/*
 * The creating thread owns callbacks and must call pump(). The worker only
 * receives plain C data; it must not call Lua, Kernel or UI code.
 */
int cdin_backend_job_pool_create(size_t worker_count, size_t capacity,
                                 cdin_backend_job_pool **out_pool);
int cdin_backend_job_submit(cdin_backend_job_pool *pool,
                            const cdin_backend_job_options *options,
                            cdin_backend_job_token **out_token);

/* A token is valid only until its completion has been pumped or discarded. */
void cdin_backend_job_cancel(cdin_backend_job_token *token);
size_t cdin_backend_job_pump(cdin_backend_job_pool *pool, size_t max_completions);
size_t cdin_backend_job_pending(const cdin_backend_job_pool *pool);

/* Graceful shutdown delivers all accepted completions on the owner thread. */
int cdin_backend_job_pool_shutdown(cdin_backend_job_pool *pool, int abort_pending);
void cdin_backend_job_pool_destroy(cdin_backend_job_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
