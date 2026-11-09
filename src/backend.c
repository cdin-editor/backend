#include "backend_internal.h"
#include "diagnostics/log.h"
#include "platform/clock.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Backend creation follows LIFE-001's dependency order: validate the caller's
 * versioned config, create the Kernel adapter, then allocate session, queues
 * and optional Runtime services. Keeping this order explicit lets failure
 * unwind in reverse order without leaving a half-live Kernel or worker pool.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 */

static char *cdin_backend_strdup(const char *value)
{
    size_t length;
    char *copy;
    if (value == NULL) return NULL;
    length = strlen(value);
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1u);
    if (copy != NULL) memcpy(copy, value, length + 1u);
    return copy;
}

int cdin_backend_is_owner(const cdin_backend *backend)
{
    if (backend == NULL || !backend->owner_thread_valid) return 0;
#ifdef _WIN32
    return GetCurrentThreadId() == backend->owner_thread;
#else
    return pthread_equal(pthread_self(), backend->owner_thread) != 0;
#endif
}

void cdin_backend_config_init(cdin_backend_config *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->max_frontends = CDIN_BACKEND_DEFAULT_MAX_FRONTENDS;
    config->outgoing_queue_capacity = CDIN_BACKEND_DEFAULT_QUEUE_CAPACITY;
    config->job_queue_capacity = CDIN_BACKEND_DEFAULT_QUEUE_CAPACITY;
    config->job_threads = CDIN_BACKEND_DEFAULT_JOB_THREADS;
    config->max_pending_completions = CDIN_BACKEND_DEFAULT_QUEUE_CAPACITY;
    config->max_document_bytes = UINT64_C(1) << 30;
    config->max_undo_bytes_per_document = UINT64_C(64) << 20;
    config->max_documents = 1024u;
    config->max_message_bytes = 16u * 1024u;
    config->max_snapshot_bytes = CDIN_BACKEND_DEFAULT_MAX_SNAPSHOT_BYTES;
    config->runtime_root = "../runtime/lua";
    config->runtime_entry = "cdin.runtime";
    config->enable_lua = 1;
    config->enable_headless_protocol = 1;
    config->log_level = CDIN_BACKEND_LOG_INFO;
    config->log_output = CDIN_BACKEND_LOG_OUTPUT_STDERR;
    config->max_log_message_bytes = 16u * 1024u;
    config->extension_source_root = NULL;
    config->max_extension_source_bytes = 256u * 1024u;
    config->max_extension_modules = 128u;
}

static cdin_backend_status config_copy(const cdin_backend_config *input,
                                       cdin_backend_config *out,
                                       char **out_root, char **out_entry)
{
    cdin_backend_config defaults;
    size_t bytes;
    if (input == NULL || out == NULL || out_root == NULL || out_entry == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    cdin_backend_config_init(&defaults);
    if (input->struct_size < offsetof(cdin_backend_config, max_frontends) + sizeof(input->max_frontends)) return CDIN_BACKEND_INVALID_ARGUMENT;
    *out = defaults;
    bytes = input->struct_size;
    if (bytes > sizeof(*out)) bytes = sizeof(*out);
    memcpy(out, input, bytes);
    out->struct_size = (uint32_t)sizeof(*out);
    if (out->max_log_message_bytes == 0u) out->max_log_message_bytes = defaults.max_log_message_bytes;
    *out_root = cdin_backend_strdup(out->runtime_root != NULL ? out->runtime_root : defaults.runtime_root);
    *out_entry = cdin_backend_strdup(out->runtime_entry != NULL ? out->runtime_entry : defaults.runtime_entry);
    if (*out_root == NULL || *out_entry == NULL) {
        free(*out_root); free(*out_entry); *out_root = NULL; *out_entry = NULL;
        return CDIN_BACKEND_NO_MEMORY;
    }
    out->runtime_root = *out_root;
    out->runtime_entry = *out_entry;
    if (out->max_frontends == 0u || out->outgoing_queue_capacity == 0u ||
        out->job_queue_capacity == 0u || out->max_pending_completions == 0u ||
        out->job_threads == 0u || out->job_threads > CDIN_BACKEND_MAX_JOB_THREADS ||
        out->max_documents == 0u || out->max_documents > SIZE_MAX / 16u ||
        out->max_document_bytes == 0u ||
        out->max_message_bytes == 0u || out->max_message_bytes > 1024u * 1024u ||
        out->max_snapshot_bytes < out->max_message_bytes ||
        out->max_snapshot_bytes > CDIN_BACKEND_MAX_SNAPSHOT_BYTES ||
        out->max_log_message_bytes > 1024u * 1024u ||
        out->max_extension_source_bytes == 0u ||
        out->max_extension_source_bytes > 1024u * 1024u ||
        out->max_extension_modules == 0u || out->max_extension_modules > 256u ||
        out->max_extension_source_bytes >
            (UINT64_C(64) * 1024u * 1024u) / (uint64_t)out->max_extension_modules ||
        (out->extension_source_root != NULL &&
         (out->extension_source_root[0] == '\0' || out->enable_lua == 0)) ||
        out->enable_lua < 0 || out->enable_lua > 1 ||
        out->enable_headless_protocol < 0 || out->enable_headless_protocol > 1 ||
        (out->enable_lua != 0 && out->enable_headless_protocol != 0 &&
         out->outgoing_queue_capacity < CDIN_BACKEND_MIN_RUNTIME_UI_QUEUE_CAPACITY)) {
        free(*out_root); free(*out_entry); *out_root = NULL; *out_entry = NULL;
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
#if SIZE_MAX < UINT64_MAX
    if ((uint64_t)out->max_frontends > (uint64_t)(SIZE_MAX / 16u)) {
        free(*out_root); free(*out_entry); *out_root = NULL; *out_entry = NULL;
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
#endif
    if (out->log_level != CDIN_BACKEND_LOG_ERROR && out->log_level != CDIN_BACKEND_LOG_WARN &&
        out->log_level != CDIN_BACKEND_LOG_INFO && out->log_level != CDIN_BACKEND_LOG_DEBUG &&
        out->log_level != CDIN_BACKEND_LOG_TRACE && out->log_level != CDIN_BACKEND_LOG_FATAL) {
        free(*out_root); free(*out_entry); *out_root = NULL; *out_entry = NULL;
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if (out->log_output < CDIN_BACKEND_LOG_OUTPUT_CALLBACK ||
        out->log_output > CDIN_BACKEND_LOG_OUTPUT_BOTH ||
        (out->log_output == CDIN_BACKEND_LOG_OUTPUT_CALLBACK && out->log_callback == NULL)) {
        free(*out_root); free(*out_entry); *out_root = NULL; *out_entry = NULL;
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    return CDIN_BACKEND_OK;
}

/* BE-004 / LIFE-001: verify and create the Kernel before any Runtime or
 * Frontend service can become active. Keeping this step in create() means an
 * unsupported ABI fails before callers can observe a half-started session.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md */
cdin_backend_status cdin_backend_create(const cdin_backend_config *config,
                                         cdin_backend **out_backend)
{
    cdin_backend *backend;
    cdin_backend_status status;
    if (out_backend == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_backend = NULL;
    backend = (cdin_backend *)calloc(1u, sizeof(*backend));
    if (backend == NULL) return CDIN_BACKEND_NO_MEMORY;
    status = config_copy(config, &backend->config, &backend->runtime_root, &backend->runtime_entry);
    if (status != CDIN_BACKEND_OK) { free(backend); return status; }
    if (backend->config.extension_source_root != NULL) {
        char *extension_root = cdin_backend_strdup(backend->config.extension_source_root);
        if (extension_root == NULL) {
            free(backend->runtime_entry);
            free(backend->runtime_root);
            free(backend);
            return CDIN_BACKEND_NO_MEMORY;
        }
        backend->config.extension_source_root = extension_root;
    }
#ifdef _WIN32
    backend->owner_thread = GetCurrentThreadId();
#else
    backend->owner_thread = pthread_self();
#endif
    backend->owner_thread_valid = 1;
    backend->session_id = cdin_clock_monotonic_ns() ^ (uint64_t)(uintptr_t)backend;
    if (backend->session_id == 0u) backend->session_id = 1u;
    status = cdin_kernel_host_create(backend->config.max_documents,
                                     backend->config.max_document_bytes,
                                     backend->config.max_undo_bytes_per_document,
                                     backend->config.log_level, backend->config.log_output,
                                     backend->config.max_log_message_bytes, backend->config.log_callback,
                                     backend->config.log_context, &backend->kernel);
    if (status != CDIN_BACKEND_OK) {
        free((char *)backend->config.extension_source_root);
        free(backend->runtime_entry); free(backend->runtime_root); free(backend);
        return status;
    }
    backend->state = CDIN_BACKEND_STATE_CREATED;
    CDIN_BACKEND_INFO(backend->kernel, "CORE",
                      "instance created session_id=%llu max_frontends=%u job_threads=%u log_level=%d",
                      (unsigned long long)backend->session_id,
                      (unsigned)backend->config.max_frontends,
                      (unsigned)backend->config.job_threads, backend->config.log_level);
    *out_backend = backend;
    return CDIN_BACKEND_OK;
}

/* LIFE-001, LIFE-002 and LIFE-004: initialize in dependency order, unwind
 * every completed stage on failure, and allow the service to start headless.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md */
cdin_backend_status cdin_backend_start(cdin_backend *backend)
{
    cdin_backend_status status;
    const char *failed_stage = "session creation";
    if (backend == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (!cdin_backend_is_owner(backend)) return CDIN_BACKEND_INVALID_STATE;
    if (backend->state != CDIN_BACKEND_STATE_CREATED) return CDIN_BACKEND_INVALID_STATE;
    backend->state = CDIN_BACKEND_STATE_STARTING;
    CDIN_BACKEND_DEBUG(backend->kernel, "CORE", "startup requested session_id=%llu",
                       (unsigned long long)backend->session_id);
    status = cdin_session_create((size_t)backend->config.max_documents,
                                 (size_t)backend->config.max_frontends * (size_t)16u,
                                 backend->session_id, &backend->session);
    if (status != CDIN_BACKEND_OK) goto fail;
    CDIN_BACKEND_TRACE(backend->kernel, "SESSION", "session metadata allocated documents=%llu views=%u",
                       (unsigned long long)backend->config.max_documents,
                       (unsigned)(backend->config.max_frontends * 16u));
    failed_stage = "worker pool creation";
    if (cdin_job_pool_create(backend->config.job_threads,
                             backend->config.job_queue_capacity,
                             backend->config.wakeup_callback,
                             backend->config.wakeup_context,
                             &backend->jobs) != 0) {
        status = CDIN_BACKEND_NO_MEMORY;
        goto fail;
    }
    CDIN_BACKEND_DEBUG(backend->kernel, "JOBS", "worker pool ready workers=%u queue_capacity=%u completion_capacity=%u",
                       (unsigned)backend->config.job_threads,
                       (unsigned)backend->config.job_queue_capacity,
                       (unsigned)backend->config.max_pending_completions);
    failed_stage = "Lua Runtime startup";
    if (backend->config.enable_lua) {
        status = cdin_lua_host_start(backend, &backend->runtime);
        if (status != CDIN_BACKEND_OK) goto fail;
        CDIN_BACKEND_INFO(backend->kernel, "RUNTIME", "Runtime Host started entry=%s", backend->runtime_entry);
    } else {
        CDIN_BACKEND_DEBUG(backend->kernel, "RUNTIME", "Lua bridge disabled by configuration");
    }
    /* LIFE-001: the Runtime Host must be ready before the Protocol can expose
     * the session to a Frontend. This prevents an attach/input arriving while
     * the Lua Runtime is only partially initialized. */
    failed_stage = "Frontend Protocol startup";
    if (backend->config.enable_headless_protocol) {
        {
            uint64_t supported_capabilities = CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS;
            if (backend->runtime != NULL &&
                cdin_lua_host_supports_pane_splitter(backend->runtime)) {
                supported_capabilities |= CDIN_PROTOCOL_CAP_PANE_SPLITTER_RESIZE;
            }
            if (backend->runtime != NULL &&
                cdin_lua_host_supports_ime_composition(backend->runtime)) {
                supported_capabilities |= CDIN_PROTOCOL_CAP_IME_COMPOSITION;
            }
            status = cdin_protocol_server_create(backend->config.max_frontends,
                                                 backend->config.outgoing_queue_capacity,
                                                 backend->config.max_pending_completions,
                                                 backend->session_id, supported_capabilities,
                                                 backend->config.max_message_bytes,
                                                 cdin_backend_protocol_log_sink, backend,
                                                 &backend->protocol);
        }
        if (status != CDIN_BACKEND_OK) goto fail;
        CDIN_BACKEND_INFO(backend->kernel, "PROTOCOL",
                          "in-process frontend protocol ready clients=%u queue_capacity=%u",
                          (unsigned)backend->config.max_frontends,
                          (unsigned)backend->config.outgoing_queue_capacity);
    } else {
        CDIN_BACKEND_DEBUG(backend->kernel, "PROTOCOL", "headless protocol disabled by configuration");
    }
    backend->state = CDIN_BACKEND_STATE_RUNNING;
    CDIN_BACKEND_INFO(backend->kernel, "CORE", "instance started session_id=%llu",
                      (unsigned long long)backend->session_id);
    return CDIN_BACKEND_OK;
fail:
    backend->last_error = status;
    backend->state = CDIN_BACKEND_STATE_FAILED;
    CDIN_BACKEND_ERROR(backend->kernel, "CORE",
                       "startup failed stage=\"%s\" status=%s reason=initialization stage returned failure",
                       failed_stage, cdin_backend_status_name(status));
    /* Reverse LIFE-001 startup order. No worker callback may outlive the
     * Runtime state it targets, and no Frontend may see a half-started host. */
    if (backend->protocol != NULL) {
        CDIN_BACKEND_TRACE(backend->kernel, "PROTOCOL", "releasing partially initialized protocol server");
        cdin_protocol_server_destroy(backend->protocol);
        backend->protocol = NULL;
    }
    if (backend->runtime != NULL) {
        /* Runtime startup completed before a later stage failed. Run its
         * shutdown hook while the Lua state and Kernel logger are still alive;
         * destroying lua_State alone would skip Runtime-owned cleanup. */
        CDIN_BACKEND_TRACE(backend->kernel, "RUNTIME", "running Runtime shutdown after startup failure");
        (void)cdin_lua_host_shutdown(backend->runtime);
        cdin_lua_host_destroy(backend->runtime);
        backend->runtime = NULL;
    }
    if (backend->jobs != NULL) {
        (void)cdin_job_pool_shutdown(backend->jobs, 1);
        CDIN_BACKEND_TRACE(backend->kernel, "JOBS", "worker pool drained and destroyed");
        cdin_job_pool_destroy(backend->jobs);
        backend->jobs = NULL;
    }
    if (backend->session != NULL) { cdin_session_destroy(backend->session); backend->session = NULL; }
    return status;
}

cdin_backend_status cdin_backend_pump(cdin_backend *backend,
                                       size_t max_completions,
                                       size_t *out_processed)
{
    size_t processed = 0u;
    if (backend == NULL || out_processed == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_processed = 0u;
    if (!cdin_backend_is_owner(backend)) return CDIN_BACKEND_INVALID_STATE;
    if (backend->state != CDIN_BACKEND_STATE_RUNNING) return CDIN_BACKEND_INVALID_STATE;
    if (backend->jobs != NULL) processed = cdin_job_pump(backend->jobs, max_completions);
    if (backend->runtime != NULL) {
        size_t runtime_processed = 0u;
        cdin_backend_status status = cdin_lua_host_pump(backend->runtime, max_completions, &runtime_processed);
        if (status != CDIN_BACKEND_OK) {
            CDIN_BACKEND_ERROR(backend->kernel, "RUNTIME",
                               "pump failed status=%s reason=Runtime Host rejected event processing",
                               cdin_backend_status_name(status));
            return status;
        }
        if (runtime_processed > SIZE_MAX - processed) {
            CDIN_BACKEND_ERROR(backend->kernel, "CORE", "pump counter overflow jobs=%llu runtime=%llu",
                               (unsigned long long)processed,
                               (unsigned long long)runtime_processed);
            return CDIN_BACKEND_INTERNAL;
        }
        processed += runtime_processed;
    }
    if (processed != 0u) {
        CDIN_BACKEND_TRACE(backend->kernel, "CORE", "pump delivered completions=%llu",
                           (unsigned long long)processed);
    }
    *out_processed = processed;
    return CDIN_BACKEND_OK;
}

/* OD-18 proposes distinct graceful-drain and abort paths. Keep the mode
 * explicit so the caller chooses whether accepted work must finish or pending
 * work is cancelled; do not silently turn a graceful close into data loss.
 * https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-18 */
cdin_backend_status cdin_backend_shutdown(cdin_backend *backend, int abort_pending)
{
    cdin_backend_status status = CDIN_BACKEND_OK;
    if (backend == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (!cdin_backend_is_owner(backend)) return CDIN_BACKEND_INVALID_STATE;
    if (backend->state == CDIN_BACKEND_STATE_STOPPED) return CDIN_BACKEND_OK;
    if (backend->state == CDIN_BACKEND_STATE_STOPPING) return CDIN_BACKEND_BUSY;
    backend->state = CDIN_BACKEND_STATE_STOPPING;
    CDIN_BACKEND_INFO(backend->kernel, "CORE", "shutdown requested mode=%s session_id=%llu",
                      abort_pending ? "abort" : "graceful",
                      (unsigned long long)backend->session_id);
    /* Stop Frontend delivery before shutting down the Runtime it feeds. */
    if (backend->protocol != NULL) {
        CDIN_BACKEND_DEBUG(backend->kernel, "PROTOCOL", "closing frontend server");
        cdin_protocol_server_destroy(backend->protocol);
        backend->protocol = NULL;
    }
    if (backend->jobs != NULL) {
        CDIN_BACKEND_DEBUG(backend->kernel, "JOBS", "stopping worker pool pending=%llu mode=%s",
                           (unsigned long long)cdin_job_pending(backend->jobs),
                           abort_pending ? "abort" : "graceful");
        if (cdin_job_pool_shutdown(backend->jobs, abort_pending) != 0 && status == CDIN_BACKEND_OK) {
            status = CDIN_BACKEND_BUSY;
            CDIN_BACKEND_ERROR(backend->kernel, "JOBS", "worker shutdown failed status=%s",
                               cdin_backend_status_name(status));
        }
        /* Completion callbacks may own Lua userdata. Drain them on the owner
         * thread while Lua is still alive; abort mode intentionally discards. */
        if (!abort_pending) (void)cdin_job_pump(backend->jobs, SIZE_MAX);
        cdin_job_pool_destroy(backend->jobs);
        backend->jobs = NULL;
    }
    if (backend->runtime != NULL) {
        cdin_backend_status runtime_status = cdin_lua_host_shutdown(backend->runtime);
        if (runtime_status != CDIN_BACKEND_OK) {
            CDIN_BACKEND_ERROR(backend->kernel, "RUNTIME", "Runtime shutdown failed status=%s",
                               cdin_backend_status_name(runtime_status));
        } else {
            CDIN_BACKEND_DEBUG(backend->kernel, "RUNTIME", "Runtime shutdown completed");
        }
        if (runtime_status != CDIN_BACKEND_OK && status == CDIN_BACKEND_OK) status = runtime_status;
        if (runtime_status != CDIN_BACKEND_OK && !abort_pending) backend->last_error = runtime_status;
        cdin_lua_host_destroy(backend->runtime);
        backend->runtime = NULL;
    }
    if (backend->session != NULL) { cdin_session_destroy(backend->session); backend->session = NULL; }
    if (backend->kernel != NULL) {
        CDIN_BACKEND_INFO(backend->kernel, "CORE", "instance stopped result=%s",
                          cdin_backend_status_name(status));
        cdin_kernel_host_destroy(backend->kernel);
        backend->kernel = NULL;
    }
    backend->state = CDIN_BACKEND_STATE_STOPPED;
    return status;
}

void cdin_backend_destroy(cdin_backend *backend)
{
    if (backend == NULL || !cdin_backend_is_owner(backend)) return;
    if (backend->state != CDIN_BACKEND_STATE_STOPPED && cdin_backend_is_owner(backend)) {
        (void)cdin_backend_shutdown(backend, 1);
    }
    if (backend->protocol != NULL) cdin_protocol_server_destroy(backend->protocol);
    if (backend->jobs != NULL) cdin_job_pool_destroy(backend->jobs);
    if (backend->runtime != NULL) cdin_lua_host_destroy(backend->runtime);
    if (backend->session != NULL) cdin_session_destroy(backend->session);
    if (backend->kernel != NULL) cdin_kernel_host_destroy(backend->kernel);
    free((char *)backend->config.extension_source_root);
    free(backend->runtime_entry);
    free(backend->runtime_root);
    free(backend);
}

cdin_backend_state cdin_backend_get_state(const cdin_backend *backend)
{
    return backend == NULL ? CDIN_BACKEND_STATE_FAILED : backend->state;
}
