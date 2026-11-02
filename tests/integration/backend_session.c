#include "cdin/backend.h"
#include "cdin/backend_protocol.h"
#include "platform/thread.h"

#include <stdio.h>
#include <string.h>

#define LOG_CAPTURE_CAPACITY 32768u

typedef struct log_capture {
    char text[LOG_CAPTURE_CAPACITY];
    size_t length;
    size_t records;
    size_t errors;
    size_t warnings;
    size_t infos;
    size_t debugs;
    size_t traces;
} log_capture;

static void capture_kernel_log(void *context, int level, const char *message, size_t length)
{
    log_capture *capture = (log_capture *)context;
    size_t available;
    size_t copy_length;

    if (capture == NULL || message == NULL) return;
    capture->records++;
    if (level == CDIN_BACKEND_LOG_ERROR || level == CDIN_BACKEND_LOG_FATAL) capture->errors++;
    if (level == CDIN_BACKEND_LOG_WARN) capture->warnings++;
    if (level == CDIN_BACKEND_LOG_INFO) capture->infos++;
    if (level == CDIN_BACKEND_LOG_DEBUG) capture->debugs++;
    if (level == CDIN_BACKEND_LOG_TRACE) capture->traces++;

    available = sizeof(capture->text) - capture->length - 1u;
    copy_length = length < available ? length : available;
    if (copy_length != 0u) {
        memcpy(capture->text + capture->length, message, copy_length);
        capture->length += copy_length;
        capture->text[capture->length] = '\0';
    }
}

static int contains(const char *text, const char *needle)
{
    return text != NULL && needle != NULL && strstr(text, needle) != NULL;
}

static int test_invalid_log_configuration(void)
{
    cdin_backend_config config;
    cdin_backend *backend = NULL;

    cdin_backend_config_init(&config);
    config.log_output = CDIN_BACKEND_LOG_OUTPUT_CALLBACK;
    config.log_callback = NULL;
    if (cdin_backend_create(&config, &backend) != CDIN_BACKEND_INVALID_ARGUMENT) return 1;
    if (backend != NULL) {
        cdin_backend_destroy(backend);
        return 1;
    }
    return 0;
}

typedef struct wrong_thread_call {
    cdin_backend *backend;
    cdin_backend_status result;
} wrong_thread_call;

static CDIN_THREAD_RETURN CDIN_THREAD_CALL call_pump_from_worker(void *opaque)
{
    wrong_thread_call *call = (wrong_thread_call *)opaque;
    size_t processed = 0u;
    call->result = cdin_backend_pump(call->backend, 1u, &processed);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int test_running_session_and_protocol(log_capture *logs)
{
    cdin_backend_config config;
    cdin_backend *backend = NULL;
    cdin_backend_frontend *driver = NULL;
    cdin_backend_frontend *observer = NULL;
    cdin_backend_frontend *overflow = NULL;
    cdin_backend_hello hello;
    cdin_backend_hello_ack ack;
    cdin_backend_message message;
    cdin_thread thread;
    wrong_thread_call wrong_thread;
    uint64_t flush_sequence = 0u;
    size_t processed = 0u;
    const uint8_t first_payload[] = "initial UI state";
    const uint8_t latest_payload[] = "latest UI state";
    const uint8_t other_payload[] = "other panel";
    cdin_backend_status status;

    cdin_backend_config_init(&config);
    config.enable_lua = 0;
    config.enable_headless_protocol = 1;
    config.max_frontends = 2u;
    config.outgoing_queue_capacity = 1u;
    config.max_pending_completions = 1u;
    config.max_message_bytes = 64u; /* Protocol payloads stay small in this scenario. */
    config.max_log_message_bytes = 16u * 1024u; /* Diagnostics use their independent cap. */
    config.log_level = CDIN_BACKEND_LOG_TRACE;
    config.log_output = CDIN_BACKEND_LOG_OUTPUT_CALLBACK;
    config.log_callback = capture_kernel_log;
    config.log_context = logs;

    status = cdin_backend_create(&config, &backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "create failed: %s\n", cdin_backend_status_name(status));
        return 1;
    }
    if (cdin_backend_get_state(backend) != CDIN_BACKEND_STATE_CREATED) goto fail;
    if (cdin_backend_pump(backend, 1u, &processed) != CDIN_BACKEND_INVALID_STATE) goto fail;
    if (cdin_backend_start(backend) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_get_state(backend) != CDIN_BACKEND_STATE_RUNNING) goto fail;
    if (cdin_backend_start(backend) != CDIN_BACKEND_INVALID_STATE) goto fail;

    /* The API is owner-thread-affine: a worker may do native work, but it may
     * not pump callbacks that could enter Lua or Frontend state. */
    wrong_thread.backend = backend;
    wrong_thread.result = CDIN_BACKEND_OK;
    if (cdin_thread_create(&thread, call_pump_from_worker, &wrong_thread) != 0) goto fail;
    cdin_thread_join(thread);
    if (wrong_thread.result != CDIN_BACKEND_INVALID_STATE) goto fail;

    if (cdin_backend_frontend_open(backend, &driver) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_frontend_open(backend, &observer) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_frontend_open(backend, &overflow) != CDIN_BACKEND_LIMIT_EXCEEDED) goto fail;

    memset(&hello, 0, sizeof(hello));
    hello.major = CDIN_BACKEND_PROTOCOL_MAJOR + 1u;
    hello.minor = CDIN_BACKEND_PROTOCOL_MINOR;
    hello.frontend_name = "example-terminal";
    hello.capabilities = UINT64_MAX;
    if (cdin_backend_frontend_hello(driver, &hello, &ack) != CDIN_BACKEND_VERSION_MISMATCH) goto fail;

    hello.major = CDIN_BACKEND_PROTOCOL_MAJOR;
    hello.minor = CDIN_BACKEND_PROTOCOL_MINOR;
    if (cdin_backend_frontend_hello(driver, &hello, &ack) != CDIN_BACKEND_OK) goto fail;
    if (ack.session_id == 0u || ack.client_id == 0u) goto fail;
    if (cdin_backend_frontend_attach(driver) != CDIN_BACKEND_OK) goto fail;
    if (!cdin_backend_frontend_can_mutate(driver)) goto fail;

    if (cdin_backend_frontend_hello(observer, &hello, &ack) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_frontend_attach(observer) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_frontend_can_mutate(observer)) goto fail;

    memset(&message, 0, sizeof(message));
    message.type = CDIN_BACKEND_MESSAGE_UI_LAYOUT;
    message.coalesce_key = 77u;
    message.payload = first_payload;
    message.payload_length = sizeof(first_payload) - 1u;
    if (cdin_backend_frontend_enqueue(driver, &message) != CDIN_BACKEND_OK) goto fail;

    /* Replacing an unflushed message with the same coalesce key models rapid
     * resize/scroll updates: only the latest layout is kept in the bounded queue. */
    message.payload = latest_payload;
    message.payload_length = sizeof(latest_payload) - 1u;
    if (cdin_backend_frontend_enqueue(driver, &message) != CDIN_BACKEND_OK) goto fail;

    message.coalesce_key = 78u;
    message.payload = other_payload;
    message.payload_length = sizeof(other_payload) - 1u;
    if (cdin_backend_frontend_enqueue(driver, &message) != CDIN_BACKEND_QUEUE_FULL) goto fail;
    if (cdin_backend_frontend_pending(driver) != 1u) goto fail;

    if (cdin_backend_frontend_flush(driver, &flush_sequence) != CDIN_BACKEND_OK || flush_sequence != 1u) goto fail;
    if (cdin_backend_frontend_flush(driver, &flush_sequence) != CDIN_BACKEND_BUSY) goto fail;
    if (cdin_backend_frontend_ack(driver, 99u) != CDIN_BACKEND_INVALID_ARGUMENT) goto fail;
    if (cdin_backend_frontend_message_at(driver, 0u, &message) != CDIN_BACKEND_OK) goto fail;
    if (message.payload_length != sizeof(latest_payload) - 1u ||
        memcmp(message.payload, latest_payload, message.payload_length) != 0) goto fail;
    if (cdin_backend_frontend_ack(driver, 1u) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_frontend_pending(driver) != 0u) goto fail;

    /* When the driver leaves, the oldest attached observer is promoted. */
    if (cdin_backend_frontend_detach(driver) != CDIN_BACKEND_OK) goto fail;
    if (!cdin_backend_frontend_can_mutate(observer)) goto fail;
    if (cdin_backend_frontend_close(driver) != CDIN_BACKEND_OK) goto fail;
    driver = NULL;
    if (cdin_backend_frontend_detach(observer) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_frontend_close(observer) != CDIN_BACKEND_OK) goto fail;
    observer = NULL;

    if (cdin_backend_pump(backend, 8u, &processed) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_shutdown(backend, 0) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_get_state(backend) != CDIN_BACKEND_STATE_STOPPED) goto fail;
    if (cdin_backend_shutdown(backend, 0) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_pump(backend, 1u, &processed) != CDIN_BACKEND_INVALID_STATE) goto fail;
    cdin_backend_destroy(backend);

    if (!contains(logs->text, "[BACKEND][CORE]")) return 1;
    if (!contains(logs->text, "[BACKEND][KERNEL]")) return 1;
    if (!contains(logs->text, "[BACKEND][JOBS]")) return 1;
    if (!contains(logs->text, "[BACKEND][PROTOCOL]")) return 1;
    if (!contains(logs->text, "backend.c:") || !contains(logs->text, "cdin_backend_start()")) return 1;
    if (!contains(logs->text, "hello_version_rejected") ||
        !contains(logs->text, "message_queue_full") || !contains(logs->text, "ack_rejected")) return 1;
    if (logs->warnings == 0u || logs->infos == 0u || logs->debugs == 0u ||
        logs->traces == 0u || logs->records < 8u) return 1;
    (void)puts("PASS backend_session: Kernel linkage, diagnostics, lifecycle, thread ownership, handshake, queue coalescing, backpressure, ACK and driver promotion");
    return 0;

fail:
    (void)fprintf(stderr, "backend session scenario failed at state=%s\n",
                  cdin_backend_state_name(cdin_backend_get_state(backend)));
    if (driver != NULL) (void)cdin_backend_frontend_close(driver);
    if (observer != NULL) (void)cdin_backend_frontend_close(observer);
    if (overflow != NULL) (void)cdin_backend_frontend_close(overflow);
    if (backend != NULL) {
        (void)cdin_backend_shutdown(backend, 1);
        cdin_backend_destroy(backend);
    }
    return 1;
}

static int test_startup_failure_is_diagnosable(void)
{
    cdin_backend_config config;
    cdin_backend *backend = NULL;
    log_capture logs;
    cdin_backend_status status;

    memset(&logs, 0, sizeof(logs));
    cdin_backend_config_init(&config);
    config.enable_lua = 1;
    config.enable_headless_protocol = 0;
    config.runtime_root = "./this-runtime-directory-does-not-exist";
    config.log_level = CDIN_BACKEND_LOG_TRACE;
    config.log_output = CDIN_BACKEND_LOG_OUTPUT_CALLBACK;
    config.log_callback = capture_kernel_log;
    config.log_context = &logs;

    status = cdin_backend_create(&config, &backend);
    if (status != CDIN_BACKEND_OK) return 1;
    status = cdin_backend_start(backend);
    if (status == CDIN_BACKEND_OK) {
        (void)cdin_backend_shutdown(backend, 1);
        cdin_backend_destroy(backend);
        return 1;
    }
    if (cdin_backend_get_state(backend) != CDIN_BACKEND_STATE_FAILED) {
        cdin_backend_destroy(backend);
        return 1;
    }
    if (!contains(logs.text, "[BACKEND][CORE]") ||
        !contains(logs.text, "startup failed stage=\"Lua Runtime startup\"") ||
        !contains(logs.text, "status=") || !contains(logs.text, "reason=") || logs.errors == 0u) {
        cdin_backend_destroy(backend);
        return 1;
    }
    if (cdin_backend_shutdown(backend, 1) != CDIN_BACKEND_OK) {
        cdin_backend_destroy(backend);
        return 1;
    }
    cdin_backend_destroy(backend);
    (void)puts("PASS backend_startup_failure: failed Runtime startup is unwound and logged with stage, status and reason");
    return 0;
}

int main(void)
{
    log_capture logs;
    memset(&logs, 0, sizeof(logs));
    if (test_invalid_log_configuration() != 0) return 1;
    if (test_running_session_and_protocol(&logs) != 0) return 1;
    if (test_startup_failure_is_diagnosable() != 0) return 1;
    return 0;
}
