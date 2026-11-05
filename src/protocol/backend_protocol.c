/*
 * PROTO-030..042: this adapter exposes the same typed message model to
 * in-process callers and byte-stream Frontends. Payload validation, sequence
 * checks, driver permissions and framing stay in the shared protocol core.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/frontend-protocol.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/protocol-wire.md
 */
#include "../backend_internal.h"
#include "../diagnostics/log.h"
#include "../../include/cdin/backend_protocol.h"
#include <string.h>


void cdin_backend_protocol_log_sink(void *context, int level, const char *event,
                                    uint64_t client_id, cdin_backend_status status,
                                    uint64_t detail)
{
    cdin_backend *backend = (cdin_backend *)context;
    if (backend == NULL || backend->kernel == NULL) return;
    cdin_backend_logf(backend->kernel, (cdin_backend_log_level)level, "PROTOCOL",
                      __FILE__, (uint32_t)__LINE__, __func__,
                      "event=%s client_id=%llu status=%s detail=%llu",
                      event != NULL ? event : "unknown",
                      (unsigned long long)client_id,
                      cdin_backend_status_name(status),
                      (unsigned long long)detail);
}

cdin_backend_status cdin_backend_frontend_open(cdin_backend *backend,
                                                 cdin_backend_frontend **out_frontend)
{
    if (backend == NULL || out_frontend == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (!cdin_backend_is_owner(backend) || backend->state != CDIN_BACKEND_STATE_RUNNING) return CDIN_BACKEND_INVALID_STATE;
    if (backend->protocol == NULL) return CDIN_BACKEND_UNSUPPORTED;
    return cdin_protocol_client_open(backend->protocol, (cdin_protocol_client **)out_frontend);
}

cdin_backend_status cdin_backend_frontend_hello(cdin_backend_frontend *frontend,
                                                  const cdin_backend_hello *hello,
                                                  cdin_backend_hello_ack *ack)
{
    cdin_protocol_hello request;
    cdin_protocol_hello_ack response;
    cdin_backend_status status;
    if (frontend == NULL || hello == NULL || ack == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    request.major = hello->major;
    request.minor = hello->minor;
    request.frontend_name = hello->frontend_name;
    request.capabilities = hello->capabilities;
    status = cdin_protocol_hello_accept((cdin_protocol_client *)frontend, &request, &response);
    if (status != CDIN_BACKEND_OK) return status;
    ack->major = response.major;
    ack->minor = response.minor;
    ack->accepted_capabilities = response.accepted_capabilities;
    ack->client_id = response.client_id;
    ack->session_id = response.session_id;
    ack->driver = response.driver;
    return CDIN_BACKEND_OK;
}

static cdin_backend_status attach_and_publish_snapshot(cdin_protocol_client *client)
{
    cdin_backend_status status;
    cdin_backend *backend;
    if (client == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    status = cdin_protocol_attach(client);
    if (status != CDIN_BACKEND_OK) return status;

    /* A Frontend may attach after Runtime startup. Replay the retained semantic
     * snapshot now so it never waits for an unrelated UI mutation to bootstrap. */
    backend = (cdin_backend *)cdin_protocol_client_server_context(client);
    if (backend != NULL && backend->runtime != NULL) {
        status = cdin_lua_host_publish_latest_ui(backend->runtime, client);
        if (status != CDIN_BACKEND_OK && status != CDIN_BACKEND_UNSUPPORTED) {
            CDIN_BACKEND_WARN(backend->kernel, "PROTOCOL",
                "initial UI snapshot deferred client_id=%llu status=%s reason=queue pressure or serialization",
                (unsigned long long)cdin_protocol_client_id(client), cdin_backend_status_name(status));
        }
    }
    /* Attachment is a lifecycle transition; a backpressured initial snapshot
     * is recoverable because Runtime retains the latest model for a later publish. */
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_backend_frontend_attach(cdin_backend_frontend *frontend)
{ return attach_and_publish_snapshot((cdin_protocol_client *)frontend); }
cdin_backend_status cdin_backend_frontend_detach(cdin_backend_frontend *frontend)
{ return cdin_protocol_detach((cdin_protocol_client *)frontend); }
cdin_backend_status cdin_backend_frontend_close(cdin_backend_frontend *frontend)
{ return cdin_protocol_client_close((cdin_protocol_client *)frontend); }
int cdin_backend_frontend_can_mutate(const cdin_backend_frontend *frontend)
{ return cdin_protocol_can_mutate((const cdin_protocol_client *)frontend); }

static cdin_backend_status protocol_status(cdin_protocol_status status)
{
    switch (status) {
    case CDIN_PROTOCOL_OK: return CDIN_BACKEND_OK;
    case CDIN_PROTOCOL_INVALID_ARGUMENT: return CDIN_BACKEND_INVALID_ARGUMENT;
    case CDIN_PROTOCOL_INVALID_FRAME: return CDIN_BACKEND_INVALID_ARGUMENT;
    case CDIN_PROTOCOL_INVALID_SCHEMA: return CDIN_BACKEND_INVALID_ARGUMENT;
    case CDIN_PROTOCOL_VERSION_MISMATCH: return CDIN_BACKEND_VERSION_MISMATCH;
    case CDIN_PROTOCOL_UNSUPPORTED: return CDIN_BACKEND_UNSUPPORTED;
    case CDIN_PROTOCOL_LIMIT_EXCEEDED: return CDIN_BACKEND_LIMIT_EXCEEDED;
    case CDIN_PROTOCOL_BUFFER_TOO_SMALL: return CDIN_BACKEND_BUFFER_TOO_SMALL;
    case CDIN_PROTOCOL_IO_ERROR: return CDIN_BACKEND_IO_ERROR;
    case CDIN_PROTOCOL_EOF: return CDIN_BACKEND_IO_ERROR;
    default: return CDIN_BACKEND_INTERNAL;
    }
}

static size_t cbor_write_uint(uint8_t *output, uint64_t value)
{
    size_t width;
    size_t index;
    if (value < 24u) { output[0] = (uint8_t)value; return 1u; }
    if (value <= UINT8_MAX) width = 1u;
    else if (value <= UINT16_MAX) width = 2u;
    else if (value <= UINT32_MAX) width = 4u;
    else width = 8u;
    output[0] = (uint8_t)(width == 1u ? 24u : width == 2u ? 25u : width == 4u ? 26u : 27u);
    for (index = 0u; index < width; index++)
        output[index + 1u] = (uint8_t)(value >> ((width - index - 1u) * 8u));
    return width + 1u;
}

static size_t cbor_write_text(uint8_t *output, const char *value)
{
    size_t length = strlen(value);
    if (length >= 24u) return 0u;
    output[0] = (uint8_t)(0x60u + length);
    memcpy(output + 1u, value, length);
    return length + 1u;
}

/* The frame codec has already validated this definite-length string array.
 * Keep this tiny reader local to the adapter so wire capability negotiation
 * does not duplicate the Protocol codec or infer support from display names. */
static int role_event_capability_requested(const uint8_t *array, size_t length)
{
    size_t offset = 0u;
    uint64_t count;
    uint64_t index;
    uint8_t initial;
    if (array == NULL || length == 0u) return 0;
    initial = array[offset++];
    if ((initial >> 5u) != 4u) return 0;
    if ((initial & 0x1fu) < 24u) count = initial & 0x1fu;
    else {
        size_t width;
        uint64_t value = 0u;
        switch (initial & 0x1fu) {
        case 24u: width = 1u; break;
        case 25u: width = 2u; break;
        case 26u: width = 4u; break;
        case 27u: width = 8u; break;
        default: return 0;
        }
        if (width > length - offset) return 0;
        while (width-- > 0u) value = (value << 8u) | array[offset++];
        count = value;
    }
    if (count > 64u) return 0;
    for (index = 0u; index < count; index++) {
        uint64_t text_length;
        uint8_t head;
        if (offset >= length) return 0;
        head = array[offset++];
        if ((head >> 5u) != 3u) return 0;
        if ((head & 0x1fu) < 24u) text_length = head & 0x1fu;
        else {
            size_t width;
            uint64_t value = 0u;
            switch (head & 0x1fu) {
            case 24u: width = 1u; break;
            case 25u: width = 2u; break;
            case 26u: width = 4u; break;
            case 27u: width = 8u; break;
            default: return 0;
            }
            if (width > length - offset) return 0;
            while (width-- > 0u) value = (value << 8u) | array[offset++];
            text_length = value;
        }
        if (text_length > (uint64_t)(length - offset)) return 0;
        if (text_length == strlen(CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS_NAME) &&
            memcmp(array + offset, CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS_NAME,
                   (size_t)text_length) == 0) return 1;
        offset += (size_t)text_length;
    }
    (void)offset;
    return 0;
}

static cdin_backend_status queue_hello_ack(cdin_backend_frontend *frontend,
                                           const cdin_backend_hello_ack *ack, uint64_t driver_role_epoch)
{
    uint8_t payload[128];
    size_t offset = 0u;
    cdin_protocol_message message;
    /* Deterministic CBOR map order is by encoded key length, then lexical order. */
    payload[offset++] = 0xa7u;
#define WRITE_KEY(key_) do { payload[offset++] = (uint8_t)(0x60u + sizeof(key_) - 1u); \
                             memcpy(payload + offset, key_, sizeof(key_) - 1u); offset += sizeof(key_) - 1u; } while (0)
    WRITE_KEY("major"); offset += cbor_write_uint(payload + offset, ack->major);
    WRITE_KEY("minor"); offset += cbor_write_uint(payload + offset, ack->minor);
    WRITE_KEY("driver"); payload[offset++] = ack->driver ? 0xf5u : 0xf4u;
    WRITE_KEY("client_id"); offset += cbor_write_uint(payload + offset, ack->client_id);
    WRITE_KEY("session_id"); offset += cbor_write_uint(payload + offset, ack->session_id);
    WRITE_KEY("driver_role_epoch"); offset += cbor_write_uint(payload + offset, driver_role_epoch);
    WRITE_KEY("accepted_capabilities");
    if ((ack->accepted_capabilities & CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS) != 0u) {
        payload[offset++] = 0x81u;
        offset += cbor_write_text(payload + offset, CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS_NAME);
    } else {
        payload[offset++] = 0x80u;
    }
#undef WRITE_KEY
    memset(&message, 0, sizeof(message));
    message.type = CDIN_PROTOCOL_HELLO_ACK;
    message.payload = payload;
    message.payload_length = offset;
    return cdin_protocol_enqueue((cdin_protocol_client *)frontend, &message);
}

static cdin_backend_status receive_hello(cdin_backend_frontend *frontend,
                                         const cdin_protocol_message_view *view)
{
    uint64_t minimum_major, minimum_minor, maximum_major, maximum_minor;
    char frontend_name[97];
    size_t name_length = 0u;
    const uint8_t *capabilities_array = NULL;
    size_t capabilities_array_length = 0u;
    cdin_protocol_hello hello;
    cdin_protocol_hello_ack ack;
    cdin_backend_hello_ack public_ack;
    cdin_protocol_status decoded;
    cdin_backend_status status;
    const uint32_t supported_major = CDIN_BACKEND_PROTOCOL_MAJOR;
    const uint32_t supported_minor = CDIN_BACKEND_PROTOCOL_MINOR;
    decoded = cdin_protocol_payload_get_uint(view->payload, view->payload_length, "minimum_major", &minimum_major);
    if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
    decoded = cdin_protocol_payload_get_uint(view->payload, view->payload_length, "minimum_minor", &minimum_minor);
    if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
    decoded = cdin_protocol_payload_get_uint(view->payload, view->payload_length, "maximum_major", &maximum_major);
    if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
    decoded = cdin_protocol_payload_get_uint(view->payload, view->payload_length, "maximum_minor", &maximum_minor);
    if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
    decoded = cdin_protocol_payload_get_text(view->payload, view->payload_length, "frontend_name",
                                             frontend_name, sizeof(frontend_name), &name_length);
    if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
    decoded = cdin_protocol_payload_get_raw(view->payload, view->payload_length, "capabilities",
                                            &capabilities_array, &capabilities_array_length);
    if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
    (void)name_length;
    if (minimum_major > maximum_major ||
        (minimum_major == maximum_major && minimum_minor > maximum_minor) ||
        minimum_major > supported_major || maximum_major < supported_major ||
        (minimum_major == supported_major && minimum_minor > supported_minor) ||
        (maximum_major == supported_major && maximum_minor < supported_minor))
        return CDIN_BACKEND_VERSION_MISMATCH;
    memset(&hello, 0, sizeof(hello));
    hello.major = supported_major;
    hello.minor = supported_minor;
    hello.frontend_name = frontend_name;
    hello.capabilities = role_event_capability_requested(capabilities_array, capabilities_array_length)
        ? CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS : 0u;
    status = cdin_protocol_hello_accept((cdin_protocol_client *)frontend, &hello, &ack);
    if (status != CDIN_BACKEND_OK) return status;
    public_ack.major = ack.major;
    public_ack.minor = ack.minor;
    public_ack.accepted_capabilities = ack.accepted_capabilities;
    public_ack.client_id = ack.client_id;
    public_ack.session_id = ack.session_id;
    public_ack.driver = ack.driver;
    return queue_hello_ack(frontend, &public_ack, ack.driver_role_epoch);
}

cdin_backend_status cdin_backend_frontend_enqueue(cdin_backend_frontend *frontend,
                                                    const cdin_backend_message *message)
{
    cdin_protocol_message request;
    if (message == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    request.type = message->type;
    request.sequence = message->sequence;
    request.coalesce_key = message->coalesce_key;
    request.flush_sequence = message->flush_sequence;
    request.payload = message->payload;
    request.payload_length = message->payload_length;
    return cdin_protocol_enqueue((cdin_protocol_client *)frontend, &request);
}

cdin_backend_status cdin_backend_frontend_flush(cdin_backend_frontend *frontend,
                                                 uint64_t *out_sequence)
{ return cdin_protocol_flush((cdin_protocol_client *)frontend, out_sequence); }
cdin_backend_status cdin_backend_frontend_ack(cdin_backend_frontend *frontend, uint64_t sequence)
{ return cdin_protocol_ack((cdin_protocol_client *)frontend, sequence); }
size_t cdin_backend_frontend_pending(const cdin_backend_frontend *frontend)
{ return cdin_protocol_pending_messages((const cdin_protocol_client *)frontend); }

cdin_backend_status cdin_backend_frontend_consume_control_message(cdin_backend_frontend *frontend)
{ return cdin_protocol_consume_control_message((cdin_protocol_client *)frontend); }

cdin_backend_status cdin_backend_frontend_message_at(const cdin_backend_frontend *frontend,
                                                       size_t index,
                                                       cdin_backend_message *out_message)
{
    cdin_protocol_message message;
    cdin_backend_status status;
    if (out_message == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    status = cdin_protocol_message_at((const cdin_protocol_client *)frontend, index, &message);
    if (status != CDIN_BACKEND_OK) return status;
    out_message->type = message.type;
    out_message->sequence = message.sequence;
    out_message->coalesce_key = message.coalesce_key;
    out_message->flush_sequence = message.flush_sequence;
    out_message->payload = message.payload;
    out_message->payload_length = message.payload_length;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_backend_frontend_receive(cdin_backend_frontend *frontend,
                                                    const uint8_t *frame,
                                                    size_t frame_length)
{
    cdin_protocol_message_view view;
    cdin_protocol_status decoded;
    cdin_backend_status status;
    uint64_t value;
    uint64_t session_id;
    uint8_t empty_map = 0xa0u;
    cdin_protocol_message reply;
    if (frontend == NULL || frame == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    decoded = cdin_protocol_decode_frame(frame, frame_length, &view,
                                          CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES);
    if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
    if (!cdin_protocol_message_is_inbound(view.type)) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (view.payload_length > cdin_protocol_client_payload_limit((cdin_protocol_client *)frontend))
        return CDIN_BACKEND_LIMIT_EXCEEDED;
    status = cdin_protocol_accept_input_sequence((cdin_protocol_client *)frontend, view.sequence);
    if (status != CDIN_BACKEND_OK) return status;

    switch (view.type) {
    case CDIN_PROTOCOL_HELLO:
        return receive_hello(frontend, &view);
    case CDIN_PROTOCOL_ATTACH:
        decoded = cdin_protocol_payload_get_uint(view.payload, view.payload_length, "session_id", &session_id);
        if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
        if (session_id != cdin_protocol_client_session_id((cdin_protocol_client *)frontend)) return CDIN_BACKEND_NOT_FOUND;
        return attach_and_publish_snapshot((cdin_protocol_client *)frontend);
    case CDIN_PROTOCOL_DETACH:
        status = cdin_protocol_detach((cdin_protocol_client *)frontend);
        if (status != CDIN_BACKEND_OK) return status;
        memset(&reply, 0, sizeof(reply));
        reply.type = CDIN_PROTOCOL_DETACH_ACK;
        reply.payload = &empty_map;
        reply.payload_length = sizeof(empty_map);
        return cdin_protocol_enqueue((cdin_protocol_client *)frontend, &reply);
    case CDIN_PROTOCOL_ACK:
        decoded = cdin_protocol_payload_get_uint(view.payload, view.payload_length, "sequence", &value);
        if (decoded != CDIN_PROTOCOL_OK) return protocol_status(decoded);
        status = cdin_protocol_ack((cdin_protocol_client *)frontend, value);
        if (status != CDIN_BACKEND_OK) return status;
        /* Retry only when a newer semantic revision could not fit before this
         * ACK freed queue capacity. The Lua bridge compares revisions, so an
         * ACK for an already-current snapshot never creates an ACK loop. */
        {
            cdin_backend *backend = (cdin_backend *)cdin_protocol_client_server_context(
                (cdin_protocol_client *)frontend);
            if (backend != NULL && backend->runtime != NULL) {
                cdin_backend_status replay = cdin_lua_host_publish_latest_ui(
                    backend->runtime, (cdin_protocol_client *)frontend);
                if (replay != CDIN_BACKEND_OK && replay != CDIN_BACKEND_UNSUPPORTED &&
                    replay != CDIN_BACKEND_BUSY && replay != CDIN_BACKEND_QUEUE_FULL) {
                    CDIN_BACKEND_WARN(backend->kernel, "PROTOCOL",
                        "snapshot retry failed client_id=%llu status=%s",
                        (unsigned long long)cdin_protocol_client_id((cdin_protocol_client *)frontend),
                        cdin_backend_status_name(replay));
                }
            }
        }
        return CDIN_BACKEND_OK;
    default:
        return cdin_protocol_receive((cdin_protocol_client *)frontend, &view);
    }
}

size_t cdin_backend_frontend_pending_events(const cdin_backend_frontend *frontend)
{ return cdin_protocol_pending_events((const cdin_protocol_client *)frontend); }

cdin_backend_status cdin_backend_frontend_event_at(const cdin_backend_frontend *frontend,
                                                      size_t index,
                                                      cdin_backend_message *out_message)
{
    cdin_protocol_message message;
    cdin_backend_status status;
    if (out_message == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    status = cdin_protocol_event_at((const cdin_protocol_client *)frontend, index, &message);
    if (status != CDIN_BACKEND_OK) return status;
    out_message->type = message.type;
    out_message->sequence = message.sequence;
    out_message->coalesce_key = message.coalesce_key;
    out_message->flush_sequence = message.flush_sequence;
    out_message->payload = message.payload;
    out_message->payload_length = message.payload_length;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_backend_frontend_consume_event(cdin_backend_frontend *frontend)
{ return cdin_protocol_consume_event((cdin_protocol_client *)frontend); }

cdin_backend_status cdin_backend_frontend_encode_message(const cdin_backend_frontend *frontend,
                                                            size_t index, uint8_t *output,
                                                            size_t output_capacity, size_t *out_length)
{
    cdin_protocol_message message;
    cdin_protocol_message_view view;
    cdin_backend_status status;
    cdin_protocol_status encoded;
    if (frontend == NULL || output == NULL || out_length == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    status = cdin_protocol_message_at((const cdin_protocol_client *)frontend, index, &message);
    if (status != CDIN_BACKEND_OK) return status;
    memset(&view, 0, sizeof(view));
    view.major = CDIN_BACKEND_PROTOCOL_MAJOR;
    view.minor = CDIN_BACKEND_PROTOCOL_MINOR;
    view.sequence = message.sequence;
    view.type = (cdin_protocol_message_type)message.type;
    view.name_space = "core";
    view.payload = message.payload;
    view.payload_length = message.payload_length;
    encoded = cdin_protocol_encode_frame(&view, output, output_capacity, out_length,
                                          CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES);
    return protocol_status(encoded);
}
