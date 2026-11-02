#include "cdin/backend.h"
#include "cdin/backend_protocol.h"
#include "backend_internal.h"
#include "platform/thread.h"
#include <cdin/protocol.h>

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
    const uint8_t first_payload[] = { 0xa2u, 0x66u, 'l','a','y','o','u','t', 0xa2u,
                                      0x64u, 'r','o','o','t', 0xf6u,
                                      0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0xf6u,
                                      0x68u, 'r','e','v','i','s','i','o','n', 0x01u };
    const uint8_t latest_payload[] = { 0xa2u, 0x66u, 'l','a','y','o','u','t', 0xa2u,
                                       0x64u, 'r','o','o','t', 0xf6u,
                                       0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0xf6u,
                                       0x68u, 'r','e','v','i','s','i','o','n', 0x02u };
    const uint8_t other_payload[] = { 0xa3u, 0x65u, 'p','a','n','e','l', 0xa0u,
                                      0x68u, 'p','a','n','e','l','_','i','d', 0x61u, 'x',
                                      0x68u, 'r','e','v','i','s','i','o','n', 0x01u };
    const uint8_t third_payload[] = { 0xa3u, 0x65u, 'p','a','n','e','l', 0xa0u,
                                      0x68u, 'p','a','n','e','l','_','i','d', 0x61u, 'y',
                                      0x68u, 'r','e','v','i','s','i','o','n', 0x02u };
    const uint8_t paste_payload[] = { 0xa1u, 0x65u, 'b','y','t','e','s', 0x42u, 0x00u, 0xffu };
    const uint8_t ack_payload[] = { 0xa1u, 0x68u, 's','e','q','u','e','n','c','e', 0x01u };
    const uint8_t text_payload[] = { 0xa1u, 0x64u, 't','e','x','t', 0x61u, 'x' };
    uint8_t frame[256];
    size_t frame_length = 0u;
    cdin_protocol_message_view input_view;
    cdin_backend_status status;

    cdin_backend_config_init(&config);
    config.enable_lua = 0;
    config.enable_headless_protocol = 1;
    config.max_frontends = 2u;
    config.outgoing_queue_capacity = 3u;
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

    /* The Runtime Host exposes semantic feature names, while Kernel ABI bits
     * remain private to src/kernel/. Verify the adapter translates all
     * capabilities required by the pinned Kernel package correctly. */
    if (!cdin_kernel_host_has_feature(backend->kernel, CDIN_HOST_FEATURE_BATCH) ||
        !cdin_kernel_host_has_feature(backend->kernel, CDIN_HOST_FEATURE_TRANSACTIONS) ||
        !cdin_kernel_host_has_feature(backend->kernel, CDIN_HOST_FEATURE_SNAPSHOTS) ||
        !cdin_kernel_host_has_feature(backend->kernel, CDIN_HOST_FEATURE_NOTIFICATIONS) ||
        !cdin_kernel_host_has_feature(backend->kernel, CDIN_HOST_FEATURE_SEARCH_LITERAL) ||
        !cdin_kernel_host_has_feature(backend->kernel, CDIN_HOST_FEATURE_SEARCH_BYTES) ||
        cdin_kernel_host_has_feature(backend->kernel, CDIN_HOST_FEATURE_SEARCH_REGEX)) goto fail;

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
    message.payload_length = sizeof(first_payload);
    if (cdin_backend_frontend_enqueue(driver, &message) != CDIN_BACKEND_OK) goto fail;

    /* An unflushed state snapshot is replaced in place by its newest revision. */
    message.payload = latest_payload;
    message.payload_length = sizeof(latest_payload);
    if (cdin_backend_frontend_enqueue(driver, &message) != CDIN_BACKEND_OK) goto fail;

    message.type = CDIN_BACKEND_MESSAGE_UI_PANEL;
    message.coalesce_key = 78u;
    message.payload = other_payload;
    message.payload_length = sizeof(other_payload);
    if (cdin_backend_frontend_enqueue(driver, &message) != CDIN_BACKEND_OK) goto fail;
    message.coalesce_key = 79u;
    message.payload = third_payload;
    message.payload_length = sizeof(third_payload);
    if (cdin_backend_frontend_enqueue(driver, &message) != CDIN_BACKEND_QUEUE_FULL) goto fail;
    if (cdin_backend_frontend_pending(driver) != 2u) goto fail;

    if (cdin_backend_frontend_flush(driver, &flush_sequence) != CDIN_BACKEND_OK || flush_sequence != 1u) goto fail;
    if (cdin_backend_frontend_pending(driver) != 3u) goto fail; /* Includes the real ui.flush message. */
    if (cdin_backend_frontend_flush(driver, &flush_sequence) != CDIN_BACKEND_BUSY) goto fail;
    if (cdin_backend_frontend_ack(driver, 99u) != CDIN_BACKEND_INVALID_ARGUMENT) goto fail;
    if (cdin_backend_frontend_message_at(driver, 0u, &message) != CDIN_BACKEND_OK) goto fail;
    if (message.payload_length != sizeof(latest_payload) ||
        memcmp(message.payload, latest_payload, message.payload_length) != 0) goto fail;
    {
        uint8_t encoded[256];
        size_t encoded_length = 0u;
        cdin_protocol_message_view decoded;
        if (cdin_backend_frontend_encode_message(driver, 2u, encoded, sizeof(encoded), &encoded_length) != CDIN_BACKEND_OK) goto fail;
        if (cdin_protocol_decode_frame(encoded, encoded_length, &decoded, sizeof(encoded)) != CDIN_PROTOCOL_OK ||
            decoded.type != CDIN_PROTOCOL_UI_FLUSH || decoded.sequence != 3u) goto fail;
    }

    /* Inbound binary paste must survive byte-for-byte and observers cannot mutate. */
    memset(&input_view, 0, sizeof(input_view));
    input_view.major = CDIN_BACKEND_PROTOCOL_MAJOR;
    input_view.minor = CDIN_BACKEND_PROTOCOL_MINOR;
    input_view.sequence = 1u;
    input_view.type = CDIN_PROTOCOL_INPUT_TEXT;
    input_view.name_space = "core";
    input_view.payload = text_payload;
    input_view.payload_length = sizeof(text_payload);
    if (cdin_protocol_encode_frame(&input_view, frame, sizeof(frame), &frame_length, sizeof(frame)) != CDIN_PROTOCOL_OK) goto fail;
    if (cdin_backend_frontend_receive(observer, frame, frame_length) != CDIN_BACKEND_PERMISSION_DENIED) goto fail;
    input_view.type = CDIN_PROTOCOL_INPUT_PASTE;
    if (cdin_protocol_encode_frame(&input_view, frame, sizeof(frame), &frame_length, sizeof(frame)) != CDIN_PROTOCOL_OK) goto fail;
    if (cdin_backend_frontend_receive(driver, frame, frame_length) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_frontend_pending_events(driver) != 1u) goto fail;
    if (cdin_backend_frontend_event_at(driver, 0u, &message) != CDIN_BACKEND_OK ||
        message.type != CDIN_PROTOCOL_INPUT_PASTE || message.payload_length != sizeof(paste_payload) ||
        memcmp(message.payload, paste_payload, sizeof(paste_payload)) != 0) goto fail;
    if (cdin_backend_frontend_consume_event(driver) != CDIN_BACKEND_OK) goto fail;
    if (cdin_backend_frontend_receive(driver, frame, frame_length) != CDIN_BACKEND_INVALID_ARGUMENT) goto fail;

    /* Wire ACK payload references the flush group, not the frame sequence. */
    input_view.sequence = 2u;
    input_view.type = CDIN_PROTOCOL_ACK;
    input_view.payload = ack_payload;
    input_view.payload_length = sizeof(ack_payload);
    if (cdin_protocol_encode_frame(&input_view, frame, sizeof(frame), &frame_length, sizeof(frame)) != CDIN_PROTOCOL_OK) goto fail;
    if (cdin_backend_frontend_receive(driver, frame, frame_length) != CDIN_BACKEND_OK) goto fail;
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
        !contains(logs->text, "message_queue_full") || !contains(logs->text, "ack_rejected") ||
        !contains(logs->text, "observer_mutation_rejected")) return 1;
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
