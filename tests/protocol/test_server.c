#include "protocol/server.h"
#include <cdin/protocol.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct diagnostic_capture {
    size_t warnings;
    size_t traces;
} diagnostic_capture;

static void capture_diagnostic(void *context, int level, const char *event,
                               uint64_t client_id, cdin_backend_status status,
                               uint64_t detail)
{
    diagnostic_capture *capture = (diagnostic_capture *)context;
    (void)event;
    (void)client_id;
    (void)status;
    (void)detail;
    if (level == CDIN_BACKEND_LOG_WARN) capture->warnings++;
    if (level == CDIN_BACKEND_LOG_TRACE) capture->traces++;
}

static cdin_protocol_client *new_attached(cdin_protocol_server *server,
                                           const char *name)
{
    cdin_protocol_client *client = NULL;
    cdin_protocol_hello hello;
    cdin_protocol_hello_ack ack;
    assert(cdin_protocol_client_open(server, &client) == CDIN_BACKEND_OK);
    memset(&hello, 0, sizeof(hello));
    hello.major = CDIN_PROTOCOL_VERSION_MAJOR;
    hello.minor = CDIN_PROTOCOL_VERSION_MINOR;
    hello.frontend_name = name;
    assert(cdin_protocol_hello_accept(client, &hello, &ack) == CDIN_BACKEND_OK);
    assert(ack.session_id == 0x11223344u);
    assert(cdin_protocol_attach(client) == CDIN_BACKEND_OK);
    return client;
}

static void test_queue_flush_ack_and_inbound(void)
{
    static const uint8_t layout_v1[] = {
        0xa2u, 0x66u, 'l','a','y','o','u','t', 0xa2u,
        0x64u, 'r','o','o','t', 0xf6u,
        0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0xf6u,
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u
    };
    static const uint8_t layout_v2[] = {
        0xa2u, 0x66u, 'l','a','y','o','u','t', 0xa2u,
        0x64u, 'r','o','o','t', 0xf6u,
        0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0xf6u,
        0x68u, 'r','e','v','i','s','i','o','n', 0x02u
    };
    static const uint8_t paste[] = { 0xa1u, 0x65u, 'b','y','t','e','s', 0x42u, 0x00u, 0xffu };
    diagnostic_capture diagnostics = { 0u, 0u };
    cdin_protocol_server *server = NULL;
    cdin_protocol_client *driver;
    cdin_protocol_client *observer;
    cdin_protocol_message outbound;
    cdin_protocol_message queued;
    cdin_protocol_message_view inbound;
    uint64_t flush_sequence = 0u;

    assert(cdin_protocol_server_create(2u, 4u, 2u, 0x11223344u, 0u,
                                       1024u, capture_diagnostic, &diagnostics,
                                       &server) == CDIN_BACKEND_OK);
    driver = new_attached(server, "test-driver");
    observer = new_attached(server, "test-observer");
    assert(cdin_protocol_can_mutate(driver));
    assert(!cdin_protocol_can_mutate(observer));

    memset(&outbound, 0, sizeof(outbound));
    outbound.type = CDIN_PROTOCOL_UI_LAYOUT;
    outbound.coalesce_key = 42u;
    outbound.payload = layout_v1;
    outbound.payload_length = sizeof(layout_v1);
    assert(cdin_protocol_enqueue(driver, &outbound) == CDIN_BACKEND_OK);
    outbound.payload = layout_v2;
    outbound.payload_length = sizeof(layout_v2);
    assert(cdin_protocol_enqueue(driver, &outbound) == CDIN_BACKEND_OK);
    assert(cdin_protocol_pending_messages(driver) == 1u);

    /* Inbound payloads are owned by the bounded queue after receive returns. */
    memset(&inbound, 0, sizeof(inbound));
    inbound.major = CDIN_PROTOCOL_VERSION_MAJOR;
    inbound.minor = CDIN_PROTOCOL_VERSION_MINOR;
    inbound.sequence = 1u;
    inbound.type = CDIN_PROTOCOL_INPUT_PASTE;
    inbound.name_space = "core";
    inbound.payload = paste;
    inbound.payload_length = sizeof(paste);
    assert(cdin_protocol_accept_input_sequence(observer, 1u) == CDIN_BACKEND_OK);
    assert(cdin_protocol_receive(observer, &inbound) == CDIN_BACKEND_PERMISSION_DENIED);
    assert(cdin_protocol_pending_events(observer) == 0u);
    assert(cdin_protocol_accept_input_sequence(driver, 1u) == CDIN_BACKEND_OK);
    assert(cdin_protocol_receive(driver, &inbound) == CDIN_BACKEND_OK);
    assert(cdin_protocol_pending_events(driver) == 1u);
    assert(cdin_protocol_event_at(driver, 0u, &queued) == CDIN_BACKEND_OK);
    assert(queued.type == CDIN_PROTOCOL_INPUT_PASTE);
    assert(queued.payload_length == sizeof(paste));
    assert(memcmp(queued.payload, paste, sizeof(paste)) == 0);
    assert(cdin_protocol_accept_input_sequence(driver, 1u) == CDIN_BACKEND_INVALID_ARGUMENT);
    assert(cdin_protocol_consume_event(driver) == CDIN_BACKEND_OK);
    assert(cdin_protocol_pending_events(driver) == 0u);

    assert(cdin_protocol_flush(driver, &flush_sequence) == CDIN_BACKEND_OK);
    assert(flush_sequence == 1u);
    assert(cdin_protocol_pending_messages(driver) == 2u);
    assert(cdin_protocol_message_at(driver, 0u, &queued) == CDIN_BACKEND_OK);
    assert(queued.type == CDIN_PROTOCOL_UI_LAYOUT);
    assert(queued.payload_length == sizeof(layout_v2));
    assert(memcmp(queued.payload, layout_v2, sizeof(layout_v2)) == 0);
    assert(cdin_protocol_message_at(driver, 1u, &queued) == CDIN_BACKEND_OK);
    assert(queued.type == CDIN_PROTOCOL_UI_FLUSH);
    assert(queued.flush_sequence == 1u);
    assert(cdin_protocol_ack(driver, 9u) == CDIN_BACKEND_INVALID_ARGUMENT);
    assert(cdin_protocol_ack(driver, 1u) == CDIN_BACKEND_OK);
    assert(cdin_protocol_pending_messages(driver) == 0u);

    /* Driver release deterministically promotes the oldest attached observer. */
    assert(cdin_protocol_detach(driver) == CDIN_BACKEND_OK);
    assert(cdin_protocol_can_mutate(observer));
    assert(cdin_protocol_pending_events(driver) == 0u);
    assert(diagnostics.warnings >= 2u);
    assert(diagnostics.traces >= 4u);

    assert(cdin_protocol_client_close(driver) == CDIN_BACKEND_OK);
    assert(cdin_protocol_client_close(observer) == CDIN_BACKEND_OK);
    cdin_protocol_server_destroy(server);
}

static void test_atomic_ui_batch(void)
{
    static const uint8_t options_payload[] = {
        0xa2u,
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u,
        0x65u, 't','h','e','m','e', 0x67u, 'd','e','f','a','u','l','t'
    };
    static const uint8_t status_payload[] = {
        0xa2u,
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u,
        0x66u, 's','t','a','t','u','s', 0xa0u
    };
    cdin_protocol_server *server = NULL;
    cdin_protocol_client *driver;
    cdin_protocol_client *observer;
    cdin_protocol_message messages[2];
    cdin_protocol_message queued;
    uint64_t flush_sequence = 0u;

    assert(cdin_protocol_server_create(2u, 2u, 2u, 0x11223344u, 0u,
                                       1024u, NULL, NULL, &server) == CDIN_BACKEND_OK);
    driver = new_attached(server, "batch-driver");
    observer = new_attached(server, "batch-observer");
    assert(!cdin_protocol_client_has_snapshot(driver));
    assert(!cdin_protocol_client_has_snapshot(observer));
    memset(messages, 0, sizeof(messages));
    messages[0].type = CDIN_PROTOCOL_UI_OPTIONS;
    messages[0].payload = options_payload;
    messages[0].payload_length = sizeof(options_payload);
    messages[1].type = CDIN_PROTOCOL_UI_STATUSLINE;
    messages[1].payload = status_payload;
    messages[1].payload_length = sizeof(status_payload);

    /* Queue capacity is checked for the entire group including ui.flush. */
    assert(cdin_protocol_publish_batch(driver, messages, 2u, &flush_sequence) == CDIN_BACKEND_QUEUE_FULL);
    assert(flush_sequence == 0u);
    assert(cdin_protocol_pending_messages(driver) == 0u);

    assert(cdin_protocol_publish_batch(driver, messages, 1u, &flush_sequence) == CDIN_BACKEND_OK);
    assert(flush_sequence == 1u);
    assert(cdin_protocol_pending_messages(driver) == 2u);
    assert(cdin_protocol_message_at(driver, 0u, &queued) == CDIN_BACKEND_OK);
    assert(queued.type == CDIN_PROTOCOL_UI_OPTIONS && queued.flush_sequence == 1u);
    assert(cdin_protocol_message_at(driver, 1u, &queued) == CDIN_BACKEND_OK);
    assert(queued.type == CDIN_PROTOCOL_UI_FLUSH && queued.flush_sequence == 1u);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_FLUSH, queued.payload,
                                          queued.payload_length) == CDIN_PROTOCOL_OK);
    {
        char scope[16];
        size_t scope_length = 0u;
        assert(cdin_protocol_payload_get_text(queued.payload, queued.payload_length,
                                              "scope", scope, sizeof(scope), &scope_length) == CDIN_PROTOCOL_OK);
        assert(scope_length == 8u && memcmp(scope, "response", 8u) == 0);
    }
    assert(cdin_protocol_ack(driver, flush_sequence) == CDIN_BACKEND_OK);
    assert(cdin_protocol_pending_messages(driver) == 0u);

    /* Full Runtime snapshots carry a distinct flush scope and model revision.
     * A Frontend must replace projected UI state only for this scope; line and
     * error responses use scope=response and must not clear existing UI state. */
    assert(cdin_protocol_publish_snapshot(driver, messages, 1u, 0u, &flush_sequence) == CDIN_BACKEND_OK);
    assert(cdin_protocol_client_has_snapshot(driver));
    assert(cdin_protocol_client_snapshot_revision(driver) == 0u);
    assert(!cdin_protocol_client_has_snapshot(observer));
    assert(cdin_protocol_ack(driver, flush_sequence) == CDIN_BACKEND_OK);
    assert(cdin_protocol_pending_messages(driver) == 0u);
    assert(cdin_protocol_publish_snapshot(driver, messages, 1u, 42u, &flush_sequence) == CDIN_BACKEND_OK);
    assert(cdin_protocol_client_snapshot_revision(driver) == 42u);
    assert(cdin_protocol_client_snapshot_revision(observer) == 0u);
    assert(cdin_protocol_message_at(driver, 1u, &queued) == CDIN_BACKEND_OK);
    assert(queued.type == CDIN_PROTOCOL_UI_FLUSH &&
           cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_FLUSH, queued.payload,
                                          queued.payload_length) == CDIN_PROTOCOL_OK);
    {
        char scope[16];
        size_t scope_length = 0u;
        uint64_t revision = 0u;
        assert(cdin_protocol_payload_get_text(queued.payload, queued.payload_length,
                                              "scope", scope, sizeof(scope), &scope_length) == CDIN_PROTOCOL_OK);
        assert(scope_length == 8u && memcmp(scope, "snapshot", 8u) == 0);
        assert(cdin_protocol_payload_get_uint(queued.payload, queued.payload_length,
                                              "revision", &revision) == CDIN_PROTOCOL_OK);
        assert(revision == 42u);
    }
    assert(cdin_protocol_ack(driver, flush_sequence) == CDIN_BACKEND_OK);
    assert(cdin_protocol_pending_messages(driver) == 0u);
    assert(cdin_protocol_client_snapshot_revision(driver) == 42u);
    assert(cdin_protocol_publish_snapshot(driver, messages, 1u, 41u, &flush_sequence) == CDIN_BACKEND_STALE_VERSION);
    assert(cdin_protocol_detach(driver) == CDIN_BACKEND_OK);
    assert(!cdin_protocol_client_has_snapshot(driver));
    assert(cdin_protocol_client_snapshot_revision(driver) == 0u);
    assert(cdin_protocol_attach(driver) == CDIN_BACKEND_OK);

    assert(cdin_protocol_client_close(driver) == CDIN_BACKEND_OK);
    assert(cdin_protocol_client_close(observer) == CDIN_BACKEND_OK);
    cdin_protocol_server_destroy(server);
}


static void test_flush_slot_is_reserved(void)
{
    static const uint8_t layout_payload[] = {
        0xa2u, 0x66u, 'l','a','y','o','u','t', 0xa2u,
        0x64u, 'r','o','o','t', 0xf6u,
        0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0xf6u,
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u
    };
    cdin_protocol_server *server = NULL;
    cdin_protocol_client *driver;
    cdin_protocol_message message;
    uint64_t flush_sequence = 0u;

    assert(cdin_protocol_server_create(1u, 3u, 1u, 0x11223344u, 0u,
                                       1024u, NULL, NULL, &server) == CDIN_BACKEND_OK);
    driver = new_attached(server, "flush-reservation-driver");
    memset(&message, 0, sizeof(message));
    message.type = CDIN_PROTOCOL_UI_LAYOUT;
    message.payload = layout_payload;
    message.payload_length = sizeof(layout_payload);
    message.coalesce_key = 1u;
    assert(cdin_protocol_enqueue(driver, &message) == CDIN_BACKEND_OK);
    message.coalesce_key = 2u;
    assert(cdin_protocol_enqueue(driver, &message) == CDIN_BACKEND_OK);
    message.coalesce_key = 3u;
    assert(cdin_protocol_enqueue(driver, &message) == CDIN_BACKEND_QUEUE_FULL);
    assert(cdin_protocol_pending_messages(driver) == 2u);

    /* The reserved slot must remain available for the atomic group marker. */
    assert(cdin_protocol_flush(driver, &flush_sequence) == CDIN_BACKEND_OK);
    assert(flush_sequence == 1u);
    assert(cdin_protocol_pending_messages(driver) == 3u);
    assert(cdin_protocol_flush(driver, &flush_sequence) == CDIN_BACKEND_BUSY);
    assert(cdin_protocol_ack(driver, flush_sequence) == CDIN_BACKEND_OK);
    assert(cdin_protocol_pending_messages(driver) == 0u);

    assert(cdin_protocol_client_close(driver) == CDIN_BACKEND_OK);
    cdin_protocol_server_destroy(server);
}

int main(void)
{
    test_queue_flush_ack_and_inbound();
    test_flush_slot_is_reserved();
    test_atomic_ui_batch();
    puts("PASS protocol server: single-driver permissions, stable promotion, schema validation, coalescing, atomic UI batch, flush marker, ACK and binary event queue");
    return 0;
}
