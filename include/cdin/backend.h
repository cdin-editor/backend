#ifndef CDIN_BACKEND_H
#define CDIN_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDIN_BACKEND_VERSION_MAJOR 1u
#define CDIN_BACKEND_VERSION_MINOR 0u
#define CDIN_BACKEND_VERSION_PATCH 0u
#define CDIN_BACKEND_HOST_API_MAJOR 0u
#define CDIN_BACKEND_HOST_API_MINOR 3u
#define CDIN_BACKEND_HOST_API_PATCH 0u
#define CDIN_BACKEND_PROTOCOL_MAJOR 1u
#define CDIN_BACKEND_PROTOCOL_MINOR 0u
#define CDIN_BACKEND_PROTOCOL_PATCH 0u

#define CDIN_BACKEND_DEFAULT_MAX_FRONTENDS 8u
#define CDIN_BACKEND_DEFAULT_QUEUE_CAPACITY 128u
/* A maximum Runtime UI snapshot (32 views, 16 panels) needs 118 queue items including ui.flush. */
#define CDIN_BACKEND_MIN_RUNTIME_UI_QUEUE_CAPACITY 118u
#define CDIN_BACKEND_DEFAULT_JOB_THREADS 2u
#define CDIN_BACKEND_MAX_JOB_THREADS 16u

/* Backend errors are deliberately independent from Kernel ABI status values. */
typedef enum cdin_backend_status {
    CDIN_BACKEND_OK = 0,
    CDIN_BACKEND_INVALID_ARGUMENT,
    CDIN_BACKEND_INVALID_STATE,
    CDIN_BACKEND_NO_MEMORY,
    CDIN_BACKEND_UNSUPPORTED,
    CDIN_BACKEND_VERSION_MISMATCH,
    CDIN_BACKEND_LIMIT_EXCEEDED,
    CDIN_BACKEND_QUEUE_FULL,
    CDIN_BACKEND_NOT_FOUND,
    CDIN_BACKEND_BUSY,
    CDIN_BACKEND_STALE_VERSION,
    CDIN_BACKEND_OUT_OF_RANGE,
    CDIN_BACKEND_INVALID_POSITION,
    CDIN_BACKEND_PATTERN_INVALID,
    CDIN_BACKEND_BUFFER_TOO_SMALL,
    CDIN_BACKEND_SHUTDOWN,
    CDIN_BACKEND_IO_ERROR,
    CDIN_BACKEND_CANCELLED,
    CDIN_BACKEND_INTERNAL,
    CDIN_BACKEND_PERMISSION_DENIED
} cdin_backend_status;

/* Logger levels accepted by cdin_backend_config.log_level. These values are
 * Backend API values, not an invitation to depend on Kernel enum definitions. */
typedef enum cdin_backend_log_level {
    CDIN_BACKEND_LOG_ERROR = 0,
    CDIN_BACKEND_LOG_WARN = 1,
    CDIN_BACKEND_LOG_INFO = 2,
    CDIN_BACKEND_LOG_DEBUG = 3,
    CDIN_BACKEND_LOG_TRACE = 4,
    CDIN_BACKEND_LOG_FATAL = 5
} cdin_backend_log_level;

/* Kernel logger output. A sink receives the final shared record, including
 * the Kernel prefix and the Backend component tag when the record came from
 * native Backend code. The message is borrowed for the duration of the call. */
typedef enum cdin_backend_log_output {
    CDIN_BACKEND_LOG_OUTPUT_CALLBACK = 0,
    CDIN_BACKEND_LOG_OUTPUT_STDERR = 1,
    CDIN_BACKEND_LOG_OUTPUT_STDOUT = 2,
    CDIN_BACKEND_LOG_OUTPUT_BOTH = 3
} cdin_backend_log_output;

typedef void (*cdin_backend_log_callback)(void *context, int level,
                                          const char *message, size_t length);

typedef enum cdin_backend_state {
    CDIN_BACKEND_STATE_CREATED = 0,
    CDIN_BACKEND_STATE_STARTING,
    CDIN_BACKEND_STATE_RUNNING,
    CDIN_BACKEND_STATE_STOPPING,
    CDIN_BACKEND_STATE_STOPPED,
    CDIN_BACKEND_STATE_FAILED
} cdin_backend_state;

typedef struct cdin_backend cdin_backend;

typedef struct cdin_backend_config {
    uint32_t struct_size;
    uint32_t max_frontends;
    uint32_t outgoing_queue_capacity;
    uint32_t job_queue_capacity;
    uint32_t job_threads;
    uint32_t max_pending_completions;
    uint64_t max_document_bytes;
    uint64_t max_undo_bytes_per_document;
    uint64_t max_documents;
    uint64_t max_message_bytes;
    /* Runtime Lua module root and require() name. The root is relative to the
     * process working directory; entry is normally "cdin.runtime". */
    const char *runtime_root;
    const char *runtime_entry;
    int enable_lua;
    int enable_headless_protocol;
    int log_level; /* One of cdin_backend_log_level; defaults to INFO. */
    /* Appended fields preserve callers that pass an older struct_size. */
    int log_output; /* One of cdin_backend_log_output; defaults to STDERR. */
    cdin_backend_log_callback log_callback; /* Optional shared Kernel-log sink. */
    void *log_context; /* Borrowed until cdin_backend_destroy(). */
    /* Independent cap for Kernel logger records; does not limit Protocol payloads. */
    uint64_t max_log_message_bytes;
} cdin_backend_config;

/* Fill all fields with portable defaults before applying caller overrides. */
void cdin_backend_config_init(cdin_backend_config *config);
const char *cdin_backend_status_name(cdin_backend_status status);
const char *cdin_backend_state_name(cdin_backend_state state);
const char *cdin_backend_version(void);

/*
 * Create validates the Kernel ABI and configures the one shared Kernel logger.
 * A custom callback may observe records from Kernel and Backend. It must be
 * thread-safe, must not retain message, and must not re-enter Backend APIs.
 */
cdin_backend_status cdin_backend_create(const cdin_backend_config *config,
                                        cdin_backend **out_backend);

/* Start creates the Runtime Host and Lua state on the calling (owner) thread. */
cdin_backend_status cdin_backend_start(cdin_backend *backend);

/* Pump completions and notifications. Must be called by the Lua owner thread. */
cdin_backend_status cdin_backend_pump(cdin_backend *backend,
                                      size_t max_completions,
                                      size_t *out_processed);

/* Graceful drains accepted jobs; abort cancels queued work and drops callbacks. */
cdin_backend_status cdin_backend_shutdown(cdin_backend *backend, int abort_pending);
void cdin_backend_destroy(cdin_backend *backend);
cdin_backend_state cdin_backend_get_state(const cdin_backend *backend);

#ifdef __cplusplus
}
#endif

#endif
