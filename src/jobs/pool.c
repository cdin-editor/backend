/*
 * BE-008 / BE-009 and ARCH-021 / ARCH-023 require expensive work to leave the
 * owner thread without turning worker queues into an unbounded memory sink.
 * Workers therefore receive immutable C job data only; completion callbacks
 * return through pump() on the creating thread, which owns Lua. Fixed queue
 * capacities make overload visible as backpressure instead of hidden growth.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md
 */
#include "pool.h"
#include <stdlib.h>
#include <string.h>

struct cdin_backend_job_token {
    atomic_bool cancelled;
    uint64_t id;
};

typedef struct cdin_job {
    cdin_job_options options;
    cdin_job_token token;
    cdin_job_result status;
    void *result;
} cdin_job;

struct cdin_backend_job_pool {
    cdin_mutex mutex;
    cdin_cond work_ready;
    cdin_cond completion_ready;
    cdin_thread *threads;
    size_t thread_count;
    cdin_job **work;
    size_t work_capacity;
    size_t work_head;
    size_t work_count;
    cdin_job **done;
    size_t done_capacity;
    size_t done_head;
    size_t done_count;
    size_t outstanding;
    uint64_t next_id;
    int stopping;
    int aborting;
    int owner_valid;
#ifdef _WIN32
    DWORD owner_thread;
#else
    pthread_t owner_thread;
#endif
};

static void cdin_job_release(cdin_job *job, int release_result)
{
    if (job == NULL) return;
    if (release_result && job->options.destroy_result != NULL && job->result != NULL) {
        job->options.destroy_result(job->result);
    }
    if (job->options.destroy_context != NULL && job->options.context != NULL) {
        job->options.destroy_context(job->options.context);
    }
    free(job);
}

#ifdef _WIN32
static int cdin_job_is_owner(const cdin_job_pool *pool)
{
    return pool->owner_valid && GetCurrentThreadId() == pool->owner_thread;
}
#else
static int cdin_job_is_owner(const cdin_job_pool *pool)
{
    return pool->owner_valid && pthread_equal(pthread_self(), pool->owner_thread) != 0;
}
#endif

static CDIN_THREAD_RETURN CDIN_THREAD_CALL cdin_job_worker(void *opaque)
{
    cdin_job_pool *pool = (cdin_job_pool *)opaque;
    for (;;) {
        cdin_job *job = NULL;
        cdin_mutex_lock(&pool->mutex);
        while (pool->work_count == 0u && !pool->stopping) cdin_cond_wait(&pool->work_ready, &pool->mutex);
        if (pool->stopping && (pool->aborting || pool->work_count == 0u)) {
            cdin_mutex_unlock(&pool->mutex);
            break;
        }
        if (pool->work_count != 0u) {
            job = pool->work[pool->work_head];
            pool->work[pool->work_head] = NULL;
            pool->work_head = (pool->work_head + 1u) % pool->work_capacity;
            pool->work_count--;
        }
        cdin_mutex_unlock(&pool->mutex);
        if (job == NULL) continue;

        if (atomic_load_explicit(&job->token.cancelled, memory_order_relaxed)) {
            job->status = CDIN_JOB_CANCELLED;
        } else {
            job->status = job->options.run(job->options.context, &job->token.cancelled, &job->result);
            if (atomic_load_explicit(&job->token.cancelled, memory_order_relaxed)) {
                job->status = CDIN_JOB_CANCELLED;
            }
        }

        cdin_mutex_lock(&pool->mutex);
        while (pool->done_count == pool->done_capacity && !pool->aborting) {
            cdin_cond_wait(&pool->completion_ready, &pool->mutex);
        }
        if (pool->aborting) {
            cdin_mutex_unlock(&pool->mutex);
            cdin_job_release(job, 1);
            cdin_mutex_lock(&pool->mutex);
            pool->outstanding--;
            cdin_cond_broadcast(&pool->completion_ready);
            cdin_mutex_unlock(&pool->mutex);
            continue;
        }
        {
            size_t tail = (pool->done_head + pool->done_count) % pool->done_capacity;
            pool->done[tail] = job;
            pool->done_count++;
        }
        cdin_cond_signal(&pool->completion_ready);
        cdin_mutex_unlock(&pool->mutex);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int cdin_job_pool_create(size_t thread_count, size_t capacity, cdin_job_pool **out_pool)
{
    cdin_job_pool *pool = NULL;
    size_t bytes = 0u;
    size_t made = 0u;
    if (out_pool == NULL || thread_count == 0u || capacity == 0u || thread_count > 16u) return -1;
    *out_pool = NULL;
    pool = (cdin_job_pool *)calloc(1u, sizeof(*pool));
    if (pool == NULL) return -1;
    if (cdin_mutex_init(&pool->mutex) != 0) goto fail;
    if (cdin_cond_init(&pool->work_ready) != 0) goto fail_mutex;
    if (cdin_cond_init(&pool->completion_ready) != 0) goto fail_work_cond;
    if (thread_count > SIZE_MAX / sizeof(cdin_thread) || capacity > SIZE_MAX / sizeof(cdin_job *)) goto fail_done_cond;
    bytes = thread_count * sizeof(cdin_thread);
    pool->threads = (cdin_thread *)calloc(1u, bytes);
    pool->work = (cdin_job **)calloc(capacity, sizeof(cdin_job *));
    pool->done = (cdin_job **)calloc(capacity, sizeof(cdin_job *));
    if (pool->threads == NULL || pool->work == NULL || pool->done == NULL) goto fail_arrays;
    pool->thread_count = thread_count;
    pool->work_capacity = capacity;
    pool->done_capacity = capacity;
#ifdef _WIN32
    pool->owner_thread = GetCurrentThreadId();
#else
    pool->owner_thread = pthread_self();
#endif
    pool->owner_valid = 1;
    for (made = 0u; made < thread_count; made++) {
        if (cdin_thread_create(&pool->threads[made], cdin_job_worker, pool) != 0) goto fail_threads;
    }
    *out_pool = pool;
    return 0;

fail_threads:
    cdin_mutex_lock(&pool->mutex);
    pool->stopping = 1;
    cdin_cond_broadcast(&pool->work_ready);
    cdin_mutex_unlock(&pool->mutex);
    while (made > 0u) cdin_thread_join(pool->threads[--made]);
fail_arrays:
    free(pool->done);
    free(pool->work);
    free(pool->threads);
fail_done_cond:
    cdin_cond_destroy(&pool->completion_ready);
fail_work_cond:
    cdin_cond_destroy(&pool->work_ready);
fail_mutex:
    cdin_mutex_destroy(&pool->mutex);
fail:
    free(pool);
    return -1;
}

int cdin_job_submit(cdin_job_pool *pool, const cdin_job_options *options, cdin_job_token **out_token)
{
    cdin_job *job = NULL;
    size_t tail = 0u;
    int rejected = 0;
    if (pool == NULL || options == NULL || options->run == NULL || out_token == NULL) return -1;
    *out_token = NULL;
    job = (cdin_job *)calloc(1u, sizeof(*job));
    if (job == NULL) return -1;
    job->options = *options;
    atomic_init(&job->token.cancelled, 0);
    cdin_mutex_lock(&pool->mutex);
    if (pool->stopping || pool->outstanding >= pool->work_capacity) {
        rejected = pool->stopping ? -2 : -3;
        cdin_mutex_unlock(&pool->mutex);
        free(job);
        return rejected;
    }
    job->token.id = ++pool->next_id;
    tail = (pool->work_head + pool->work_count) % pool->work_capacity;
    pool->work[tail] = job;
    pool->work_count++;
    pool->outstanding++;
    *out_token = &job->token;
    cdin_cond_signal(&pool->work_ready);
    cdin_mutex_unlock(&pool->mutex);
    return 0;
}

void cdin_job_cancel(cdin_job_token *token)
{
    if (token != NULL) atomic_store_explicit(&token->cancelled, 1, memory_order_relaxed);
}

size_t cdin_job_pump(cdin_job_pool *pool, size_t max_completions)
{
    size_t processed = 0u;
    if (pool == NULL || !cdin_job_is_owner(pool)) return 0u;
    while (processed < max_completions) {
        cdin_job *job = NULL;
        cdin_mutex_lock(&pool->mutex);
        if (pool->done_count != 0u) {
            job = pool->done[pool->done_head];
            pool->done[pool->done_head] = NULL;
            pool->done_head = (pool->done_head + 1u) % pool->done_capacity;
            pool->done_count--;
            pool->outstanding--;
            cdin_cond_broadcast(&pool->completion_ready);
        }
        cdin_mutex_unlock(&pool->mutex);
        if (job == NULL) break;
        if (job->options.complete != NULL) job->options.complete(job->options.owner, job->status, job->result);
        cdin_job_release(job, 1);
        processed++;
    }
    return processed;
}

size_t cdin_job_pending(const cdin_job_pool *pool)
{
    size_t value;
    if (pool == NULL) return 0u;
    cdin_mutex_lock((cdin_mutex *)&pool->mutex);
    value = pool->outstanding;
    cdin_mutex_unlock((cdin_mutex *)&pool->mutex);
    return value;
}

int cdin_job_pool_shutdown(cdin_job_pool *pool, int abort_pending)
{
    size_t i;
    if (pool == NULL || !cdin_job_is_owner(pool)) return -1;

    cdin_mutex_lock(&pool->mutex);
    pool->stopping = 1;
    pool->aborting = abort_pending != 0;

    if (pool->aborting) {
        /* Queued jobs have not started; remove them here so workers can exit. */
        for (i = 0u; i < pool->work_count; i++) {
            size_t at = (pool->work_head + i) % pool->work_capacity;
            if (pool->work[at] != NULL) {
                atomic_store_explicit(&pool->work[at]->token.cancelled, 1, memory_order_relaxed);
            }
        }
    }
    cdin_cond_broadcast(&pool->work_ready);
    cdin_cond_broadcast(&pool->completion_ready);
    cdin_mutex_unlock(&pool->mutex);

    if (!abort_pending) {
        /*
         * Do not join before draining completions. A worker may be blocked on
         * a full done queue; pumping on the owner thread releases that slot
         * and preserves the callback ownership contract.
         */
        for (;;) {
            size_t outstanding;
            size_t completed = cdin_job_pump(pool, SIZE_MAX);
            cdin_mutex_lock(&pool->mutex);
            outstanding = pool->outstanding;
            if (outstanding == 0u) {
                cdin_mutex_unlock(&pool->mutex);
                break;
            }
            if (completed == 0u && pool->done_count == 0u && pool->outstanding != 0u) {
                cdin_cond_wait(&pool->completion_ready, &pool->mutex);
            }
            cdin_mutex_unlock(&pool->mutex);
        }
    } else {
        /*
         * Abort mode deliberately discards callbacks. Remove one queued or
         * completed record at a time and release it outside the mutex because
         * user-provided destructors may call unrelated application code.
         */
        for (;;) {
            cdin_job *discarded = NULL;
            size_t outstanding;

            cdin_mutex_lock(&pool->mutex);
            if (pool->work_count != 0u) {
                discarded = pool->work[pool->work_head];
                pool->work[pool->work_head] = NULL;
                pool->work_head = (pool->work_head + 1u) % pool->work_capacity;
                pool->work_count--;
            } else if (pool->done_count != 0u) {
                discarded = pool->done[pool->done_head];
                pool->done[pool->done_head] = NULL;
                pool->done_head = (pool->done_head + 1u) % pool->done_capacity;
                pool->done_count--;
            }
            if (discarded != NULL && pool->outstanding != 0u) pool->outstanding--;
            outstanding = pool->outstanding;
            if (discarded == NULL && outstanding != 0u) {
                cdin_cond_wait(&pool->completion_ready, &pool->mutex);
                outstanding = pool->outstanding;
            }
            cdin_cond_broadcast(&pool->completion_ready);
            cdin_mutex_unlock(&pool->mutex);

            if (discarded != NULL) cdin_job_release(discarded, 1);
            if (outstanding == 0u && discarded == NULL) break;
        }
    }

    for (i = 0u; i < pool->thread_count; i++) cdin_thread_join(pool->threads[i]);
    pool->thread_count = 0u;
    return 0;
}

void cdin_job_pool_destroy(cdin_job_pool *pool)
{
    size_t i;
    if (pool == NULL) return;
    if (pool->thread_count != 0u) (void)cdin_job_pool_shutdown(pool, 1);
    for (i = 0u; i < pool->work_capacity; i++) cdin_job_release(pool->work[i], 1);
    for (i = 0u; i < pool->done_capacity; i++) cdin_job_release(pool->done[i], 1);
    free(pool->done);
    free(pool->work);
    free(pool->threads);
    cdin_cond_destroy(&pool->completion_ready);
    cdin_cond_destroy(&pool->work_ready);
    cdin_mutex_destroy(&pool->mutex);
    free(pool);
}
