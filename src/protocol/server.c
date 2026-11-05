/* PROTO-010..042 implement the accepted message lifecycle, role checks,
 * bounded queues and acknowledgement contract. The codec is a separate
 * dependency so in-process tests and stream transports share identical CBOR
 * validation and framing.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/frontend-protocol.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/protocol-wire.md
 */
#include "server.h"
#include <stdlib.h>
#include <string.h>

#define CDIN_PROTOCOL_NAME_MAX 96u

typedef struct cdin_protocol_queued {
    uint32_t type;
    uint64_t sequence;
    uint64_t coalesce_key;
    uint64_t flush_sequence;
    uint8_t *payload;
    size_t payload_length;
} cdin_protocol_queued;

struct cdin_protocol_server {
    size_t max_clients;
    size_t queue_capacity;
    size_t max_unacked_flushes;
    size_t max_message_bytes;
    size_t client_count;
    uint64_t session_id;
    uint64_t supported_capabilities;
    uint64_t next_client_id;
    uint64_t driver_client_id;
    cdin_protocol_client **clients;
    cdin_protocol_diagnostic_fn diagnostic;
    void *diagnostic_context;
};

struct cdin_protocol_client {
    cdin_protocol_server *server;
    uint64_t id;
    char name[CDIN_PROTOCOL_NAME_MAX + 1u];
    uint64_t requested_capabilities;
    uint64_t accepted_capabilities;
    cdin_protocol_queued *queue;
    size_t queue_count;
    size_t queue_capacity;
    cdin_protocol_queued *inbound;
    size_t inbound_count;
    size_t inbound_capacity;
    uint64_t last_inbound_sequence;
    uint64_t next_message_sequence;
    uint64_t next_flush_sequence;
    uint64_t acknowledged_sequence;
    uint64_t last_snapshot_revision;
    int has_snapshot_revision;
    size_t pending_flushes;
    int greeted;
    int attached;
    int closed;
    int driver;
    int role_event_pending;
};

static void cdin_protocol_log(cdin_protocol_server *server, int level, const char *event,
                              uint64_t client_id, cdin_backend_status status, uint64_t detail)
{
    if (server != NULL && server->diagnostic != NULL) {
        server->diagnostic(server->diagnostic_context, level, event, client_id, status, detail);
    }
}

static cdin_backend_status protocol_enqueue(cdin_protocol_client *client,
                                             const cdin_protocol_message *message,
                                             int is_flush_marker);
static cdin_backend_status queue_driver_role_event(cdin_protocol_client *client,
                                                     uint64_t epoch);
static cdin_backend_status promote_oldest_observer(cdin_protocol_server *server);

static void cdin_protocol_clear_item(cdin_protocol_queued *item)
{
    free(item->payload);
    memset(item, 0, sizeof(*item));
}

static void cdin_protocol_remove_item(cdin_protocol_client *client, size_t index)
{
    if (client->queue[index].type == CDIN_PROTOCOL_DRIVER_ROLE_CHANGED) {
        client->role_event_pending = 0;
    }
    cdin_protocol_clear_item(&client->queue[index]);
    if (index + 1u < client->queue_count) {
        memmove(&client->queue[index], &client->queue[index + 1u],
                (client->queue_count - index - 1u) * sizeof(*client->queue));
    }
    client->queue_count--;
    memset(&client->queue[client->queue_count], 0, sizeof(*client->queue));
}

cdin_backend_status cdin_protocol_server_create(size_t max_clients, size_t queue_capacity,
                                                  size_t max_unacked_flushes, uint64_t session_id,
                                                  uint64_t supported_capabilities, size_t max_message_bytes,
                                                  cdin_protocol_diagnostic_fn diagnostic,
                                                  void *diagnostic_context,
                                                  cdin_protocol_server **out_server)
{
    cdin_protocol_server *server;
    if (out_server == NULL || max_clients == 0u || queue_capacity == 0u || max_unacked_flushes == 0u || max_message_bytes == 0u) return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_server = NULL;
    if (max_clients > SIZE_MAX / sizeof(cdin_protocol_client *)) return CDIN_BACKEND_LIMIT_EXCEEDED;
    server = (cdin_protocol_server *)calloc(1u, sizeof(*server));
    if (server == NULL) return CDIN_BACKEND_NO_MEMORY;
    server->clients = (cdin_protocol_client **)calloc(max_clients, sizeof(cdin_protocol_client *));
    if (server->clients == NULL) { free(server); return CDIN_BACKEND_NO_MEMORY; }
    server->max_clients = max_clients;
    server->queue_capacity = queue_capacity;
    server->max_unacked_flushes = max_unacked_flushes;
    server->max_message_bytes = max_message_bytes;
    server->session_id = session_id;
    server->supported_capabilities = supported_capabilities;
    server->diagnostic = diagnostic;
    server->diagnostic_context = diagnostic_context;
    *out_server = server;
    return CDIN_BACKEND_OK;
}

void cdin_protocol_server_destroy(cdin_protocol_server *server)
{
    size_t i;
    if (server == NULL) return;
    for (i = 0u; i < server->max_clients; i++) {
        cdin_protocol_client *client = server->clients[i];
        if (client != NULL) {
            size_t n;
            for (n = 0u; n < client->queue_count; n++) cdin_protocol_clear_item(&client->queue[n]);
            for (n = 0u; n < client->inbound_count; n++) cdin_protocol_clear_item(&client->inbound[n]);
            free(client->queue);
            free(client->inbound);
            free(client);
        }
    }
    free(server->clients);
    free(server);
}

cdin_backend_status cdin_protocol_client_open(cdin_protocol_server *server,
                                               cdin_protocol_client **out_client)
{
    cdin_protocol_client *client;
    size_t i;
    if (server == NULL || out_client == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_client = NULL;
    if (server->client_count >= server->max_clients) {
        cdin_protocol_log(server, CDIN_BACKEND_LOG_WARN, "client_limit_reached", 0u,
                          CDIN_BACKEND_LIMIT_EXCEEDED, (uint64_t)server->max_clients);
        return CDIN_BACKEND_LIMIT_EXCEEDED;
    }
    client = (cdin_protocol_client *)calloc(1u, sizeof(*client));
    if (client == NULL) return CDIN_BACKEND_NO_MEMORY;
    client->queue = (cdin_protocol_queued *)calloc(server->queue_capacity, sizeof(*client->queue));
    client->inbound = (cdin_protocol_queued *)calloc(server->queue_capacity, sizeof(*client->inbound));
    if (client->queue == NULL || client->inbound == NULL) {
        free(client->queue);
        free(client->inbound);
        free(client);
        return CDIN_BACKEND_NO_MEMORY;
    }
    client->server = server;
    client->queue_capacity = server->queue_capacity;
    client->inbound_capacity = server->queue_capacity;
    if (server->next_client_id == UINT64_MAX) { free(client->queue); free(client->inbound); free(client); return CDIN_BACKEND_LIMIT_EXCEEDED; }
    client->id = ++server->next_client_id;
    for (i = 0u; i < server->max_clients; i++) {
        if (server->clients[i] == NULL) { server->clients[i] = client; break; }
    }
    if (i == server->max_clients) { free(client->queue); free(client->inbound); free(client); return CDIN_BACKEND_LIMIT_EXCEEDED; }
    server->client_count++;
    *out_client = client;
    cdin_protocol_log(server, CDIN_BACKEND_LOG_TRACE, "client_opened", client->id,
                      CDIN_BACKEND_OK, (uint64_t)server->client_count);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_hello_accept(cdin_protocol_client *client,
                                                const cdin_protocol_hello *hello,
                                                cdin_protocol_hello_ack *ack)
{
    size_t length;
    if (client == NULL || hello == NULL || ack == NULL || client->closed) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (client->greeted) return CDIN_BACKEND_INVALID_STATE;
    if (hello->frontend_name == NULL || hello->frontend_name[0] == '\0') return CDIN_BACKEND_INVALID_ARGUMENT;
    length = strlen(hello->frontend_name);
    if (length > CDIN_PROTOCOL_NAME_MAX) return CDIN_BACKEND_LIMIT_EXCEEDED;
    if (hello->major != CDIN_BACKEND_PROTOCOL_MAJOR || hello->minor != CDIN_BACKEND_PROTOCOL_MINOR) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "hello_version_rejected",
                          client->id, CDIN_BACKEND_VERSION_MISMATCH,
                          ((uint64_t)hello->major << 32) | (uint64_t)hello->minor);
        return CDIN_BACKEND_VERSION_MISMATCH;
    }
    memcpy(client->name, hello->frontend_name, length + 1u);
    client->requested_capabilities = hello->capabilities;
    client->accepted_capabilities = hello->capabilities & client->server->supported_capabilities;
    client->greeted = 1;
    ack->major = CDIN_BACKEND_PROTOCOL_MAJOR;
    ack->minor = CDIN_BACKEND_PROTOCOL_MINOR;
    ack->accepted_capabilities = client->accepted_capabilities;
    ack->client_id = client->id;
    ack->session_id = client->server->session_id;
    /* The role is returned in HELLO_ACK, so reserve the first negotiated
     * client as driver before replying. ATTACH activates that reservation. */
    if (client->server->driver_client_id == 0u) {
        client->server->driver_client_id = client->id;
        client->driver = 1;
    }
    ack->driver = client->driver;
    ack->driver_role_epoch = client->next_message_sequence + 1u;
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_DEBUG, "hello_accepted", client->id,
                      CDIN_BACKEND_OK, ack->accepted_capabilities);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_attach(cdin_protocol_client *client)
{
    if (client == NULL || client->closed) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (!client->greeted) return CDIN_BACKEND_INVALID_STATE;
    if (client->attached) return CDIN_BACKEND_INVALID_STATE;
    client->attached = 1;
    if (client->server->driver_client_id == 0u && client->driver) {
        client->server->driver_client_id = client->id;
    } else if (client->server->driver_client_id != client->id) {
        client->driver = 0;
    }
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_INFO, "client_attached", client->id,
                      CDIN_BACKEND_OK, (uint64_t)(client->driver != 0));
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_detach(cdin_protocol_client *client)
{
    cdin_protocol_server *server;
    if (client == NULL || client->closed || !client->attached) return CDIN_BACKEND_INVALID_STATE;
    server = client->server;
    client->attached = 0;
    client->driver = 0;
    /* A detached frontend must not replay stale output after it reattaches. */
    while (client->queue_count > 0u) cdin_protocol_remove_item(client, client->queue_count - 1u);
    client->pending_flushes = 0u;
    client->acknowledged_sequence = client->next_flush_sequence;
    client->last_snapshot_revision = 0u;
    client->has_snapshot_revision = 0;
    /* Events accepted before detach must never execute after a later reattach. */
    while (client->inbound_count > 0u) {
        cdin_protocol_clear_item(&client->inbound[client->inbound_count - 1u]);
        client->inbound_count--;
    }
    /* Per-connection sequence numbers survive detach/reattach. */
    if (server->driver_client_id == client->id) {
        cdin_backend_status promotion_status = promote_oldest_observer(server);
        if (promotion_status != CDIN_BACKEND_OK) {
            cdin_protocol_log(server, CDIN_BACKEND_LOG_ERROR, "driver_role_event_failed",
                              client->id, promotion_status, client->next_message_sequence);
            return promotion_status;
        }
    }
    cdin_protocol_log(server, CDIN_BACKEND_LOG_INFO, "client_detached", client->id,
                      CDIN_BACKEND_OK, server->driver_client_id);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_client_close(cdin_protocol_client *client)
{
    cdin_protocol_server *server;
    cdin_backend_status status;
    size_t i;
    if (client == NULL || client->closed) return CDIN_BACKEND_INVALID_STATE;
    server = client->server;
    if (client->attached) {
        status = cdin_protocol_detach(client);
        if (status != CDIN_BACKEND_OK) return status;
    } else if (server->driver_client_id == client->id) {
        /* Release a HELLO-time lease reservation; promote the oldest attached
         * observer through the same observable role-transition path. */
        cdin_backend_status promotion_status;
        server->driver_client_id = 0u;
        promotion_status = promote_oldest_observer(server);
        if (promotion_status != CDIN_BACKEND_OK) {
            cdin_protocol_log(server, CDIN_BACKEND_LOG_ERROR, "driver_role_event_failed",
                              0u, promotion_status, server->driver_role_epoch);
            return promotion_status;
        }
    }
    for (i = 0u; i < server->max_clients; i++) {
        if (server->clients[i] == client) { server->clients[i] = NULL; break; }
    }
    server->client_count--;
    client->closed = 1;
    cdin_protocol_log(server, CDIN_BACKEND_LOG_INFO, "client_closed", client->id,
                      CDIN_BACKEND_OK, (uint64_t)server->client_count);
    for (i = 0u; i < client->queue_count; i++) cdin_protocol_clear_item(&client->queue[i]);
    for (i = 0u; i < client->inbound_count; i++) cdin_protocol_clear_item(&client->inbound[i]);
    free(client->queue);
    free(client->inbound);
    free(client);
    return CDIN_BACKEND_OK;
}

int cdin_protocol_can_mutate(const cdin_protocol_client *client)
{
    return client != NULL && !client->closed && client->attached && client->driver;
}

static cdin_backend_status protocol_enqueue(cdin_protocol_client *client,
                                            const cdin_protocol_message *message,
                                            int is_flush_marker)
{
    uint8_t *copy = NULL;
    size_t i;
    size_t queue_limit;
    int handshake_reply;
    if (client == NULL || message == NULL || client->closed) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (message->type == CDIN_PROTOCOL_DRIVER_ROLE_CHANGED) {
        if ((client->accepted_capabilities & CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS) == 0u)
            return CDIN_BACKEND_UNSUPPORTED;
        if (client->role_event_pending) return CDIN_BACKEND_BUSY;
    }
    handshake_reply = (message->type == CDIN_PROTOCOL_HELLO_ACK ||
                       message->type == CDIN_PROTOCOL_DETACH_ACK);
    if (!cdin_protocol_client_is_attached(client) &&
        !(handshake_reply && client->greeted)) return CDIN_BACKEND_INVALID_STATE;
    if (cdin_protocol_message_is_inbound((cdin_protocol_message_type)message->type) ||
        cdin_protocol_message_name((cdin_protocol_message_type)message->type) == NULL) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "outbound_direction_rejected",
                          client->id, CDIN_BACKEND_INVALID_ARGUMENT, (uint64_t)message->type);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if (message->payload == NULL || message->payload_length == 0u) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "message_rejected_missing_payload",
                          client->id, CDIN_BACKEND_INVALID_ARGUMENT, (uint64_t)message->type);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if (message->payload_length > client->server->max_message_bytes) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "message_rejected_size_limit",
                          client->id, CDIN_BACKEND_LIMIT_EXCEEDED, (uint64_t)message->payload_length);
        return CDIN_BACKEND_LIMIT_EXCEEDED;
    }
    if (cdin_protocol_validate_payload((cdin_protocol_message_type)message->type,
                                       message->payload, message->payload_length) != CDIN_PROTOCOL_OK) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "message_schema_rejected",
                          client->id, CDIN_BACKEND_INVALID_ARGUMENT, (uint64_t)message->type);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if (client->next_message_sequence == UINT64_MAX) return CDIN_BACKEND_LIMIT_EXCEEDED;
    if (message->payload_length != 0u) {
        copy = (uint8_t *)malloc(message->payload_length);
        if (copy == NULL) return CDIN_BACKEND_NO_MEMORY;
        memcpy(copy, message->payload, message->payload_length);
    }
    if (message->coalesce_key != 0u) {
        for (i = client->queue_count; i > 0u; i--) {
            cdin_protocol_queued *old = &client->queue[i - 1u];
            if (old->type == message->type && old->coalesce_key == message->coalesce_key &&
                old->flush_sequence == 0u) {
                free(old->payload);
                old->payload = copy;
                old->payload_length = message->payload_length;
                cdin_protocol_log(client->server, CDIN_BACKEND_LOG_TRACE, "message_coalesced",
                                  client->id, CDIN_BACKEND_OK, (uint64_t)message->type);
                return CDIN_BACKEND_OK;
            }
        }
    }
    /* Keep one queue slot for the ui.flush marker while another flush can
     * still be published. Without this reservation normal messages could
     * consume the entire queue and make an otherwise valid batch impossible
     * to terminate atomically. The internal flush path may consume the slot. */
    queue_limit = client->queue_capacity;
    if (!is_flush_marker) {
        size_t reserved = 0u;
        if (client->pending_flushes < client->server->max_unacked_flushes) reserved++;
        if ((client->accepted_capabilities & CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS) != 0u) reserved++;
        queue_limit = reserved >= queue_limit ? 0u : queue_limit - reserved;
    }
    if (client->queue_count >= queue_limit) {
        free(copy);
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "message_queue_full",
                          client->id, CDIN_BACKEND_QUEUE_FULL, (uint64_t)client->queue_count);
        return CDIN_BACKEND_QUEUE_FULL;
    }
    client->queue[client->queue_count].type = message->type;
    client->queue[client->queue_count].sequence = ++client->next_message_sequence;
    client->queue[client->queue_count].coalesce_key = message->coalesce_key;
    client->queue[client->queue_count].payload = copy;
    client->queue[client->queue_count].payload_length = message->payload_length;
    client->queue[client->queue_count].flush_sequence = 0u;
    client->queue_count++;
    if (message->type == CDIN_PROTOCOL_DRIVER_ROLE_CHANGED) client->role_event_pending = 1;
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_TRACE, "message_queued",
                      client->id, CDIN_BACKEND_OK, (uint64_t)message->type);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_enqueue(cdin_protocol_client *client,
                                           const cdin_protocol_message *message)
{
    return protocol_enqueue(client, message, 0);
}

static size_t write_cbor_uint(uint8_t *output, uint64_t value)
{
    size_t bytes;
    size_t index;
    if (value < 24u) { output[0] = (uint8_t)value; return 1u; }
    if (value <= UINT8_MAX) bytes = 1u;
    else if (value <= UINT16_MAX) bytes = 2u;
    else if (value <= UINT32_MAX) bytes = 4u;
    else bytes = 8u;
    output[0] = (uint8_t)(bytes == 1u ? 24u : bytes == 2u ? 25u : bytes == 4u ? 26u : 27u);
    for (index = 0u; index < bytes; index++)
        output[1u + index] = (uint8_t)(value >> ((bytes - index - 1u) * 8u));
    return bytes + 1u;
}

static cdin_backend_status queue_driver_role_event(cdin_protocol_client *client,
                                                     uint64_t epoch)
{
    uint8_t payload[32];
    size_t offset = 0u;
    cdin_protocol_message message;
    if (client == NULL || client->closed || !client->attached) {
        return CDIN_BACKEND_INVALID_STATE;
    }
    if ((client->accepted_capabilities & CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS) == 0u) {
        return CDIN_BACKEND_OK; /* Legacy Frontends reconnect to refresh HELLO_ACK. */
    }
    if (client->role_event_pending) return CDIN_BACKEND_BUSY;
    payload[offset++] = 0xa2u;
    payload[offset++] = 0x65u; memcpy(payload + offset, "epoch", 5u); offset += 5u;
    offset += write_cbor_uint(payload + offset, epoch);
    payload[offset++] = 0x66u; memcpy(payload + offset, "driver", 6u); offset += 6u;
    payload[offset++] = client->driver ? 0xf5u : 0xf4u;
    memset(&message, 0, sizeof(message));
    message.type = CDIN_PROTOCOL_DRIVER_ROLE_CHANGED;
    message.payload = payload;
    message.payload_length = offset;
    /* This consumes the slot reserved for a negotiated role event, not a
     * flush slot: the role update is ordered but is not an ACK group. */
    return protocol_enqueue(client, &message, 1);
}

static cdin_backend_status promote_oldest_observer(cdin_protocol_server *server)
{
    cdin_protocol_client *oldest = NULL;
    size_t index;
    cdin_backend_status status;
    if (server == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (server->driver_role_epoch == UINT64_MAX) return CDIN_BACKEND_LIMIT_EXCEEDED;
    server->driver_role_epoch++;
    server->driver_client_id = 0u;
    for (index = 0u; index < server->max_clients; index++) {
        cdin_protocol_client *candidate = server->clients[index];
        if (candidate != NULL && candidate->attached && !candidate->closed &&
            (oldest == NULL || candidate->id < oldest->id)) oldest = candidate;
    }
    if (oldest == NULL) return CDIN_BACKEND_OK;
    oldest->driver = 1;
    server->driver_client_id = oldest->id;
    status = queue_driver_role_event(oldest, server->driver_role_epoch);
    if (status != CDIN_BACKEND_OK) {
        /* Never grant an unobservable lease to a negotiated Frontend. The
         * session stays fail-closed and a subsequent HELLO can acquire it. */
        oldest->driver = 0;
        server->driver_client_id = 0u;
    }
    return status;
}

static size_t write_cbor_text(uint8_t *output, const char *value, size_t length)
{
    if (length >= 24u) return 0u; /* Static protocol field names and scopes are bounded. */
    output[0] = (uint8_t)(0x60u | (uint8_t)length);
    memcpy(output + 1u, value, length);
    return length + 1u;
}

static size_t build_flush_payload(uint8_t *output, size_t capacity, uint64_t sequence,
                                  const char *scope, int has_revision, uint64_t revision)
{
    size_t offset = 0u;
    size_t written;
    size_t pair_count = has_revision ? 3u : 2u;
    if (output == NULL || scope == NULL || capacity < 48u) return 0u;

    /* Canonical CBOR key order: scope (5), revision (8), sequence (8). */
    output[offset++] = (uint8_t)(0xa0u | (uint8_t)pair_count);
    written = write_cbor_text(output + offset, "scope", 5u);
    if (written == 0u) return 0u;
    offset += written;
    written = write_cbor_text(output + offset, scope, strlen(scope));
    if (written == 0u) return 0u;
    offset += written;
    if (has_revision) {
        written = write_cbor_text(output + offset, "revision", 8u);
        if (written == 0u) return 0u;
        offset += written;
        offset += write_cbor_uint(output + offset, revision);
    }
    written = write_cbor_text(output + offset, "sequence", 8u);
    if (written == 0u) return 0u;
    offset += written;
    offset += write_cbor_uint(output + offset, sequence);
    return offset <= capacity ? offset : 0u;
}

static cdin_backend_status protocol_publish_batch(cdin_protocol_client *client,
                                                    const cdin_protocol_message *messages,
                                                    size_t message_count,
                                                    const char *flush_scope,
                                                    int has_revision,
                                                    uint64_t revision,
                                                    uint64_t *out_flush_sequence)
{
    cdin_protocol_queued *prepared = NULL;
    uint8_t flush_payload[64];
    size_t flush_length;
    size_t total_items;
    size_t index;
    uint64_t flush_sequence;
    uint64_t next_message_sequence;
    if (client == NULL || out_flush_sequence == NULL || flush_scope == NULL ||
        (messages == NULL && message_count != 0u) || client->closed || !client->attached)
        return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_flush_sequence = 0u;
    if (client->pending_flushes >= client->server->max_unacked_flushes)
        return CDIN_BACKEND_BUSY;
    if (has_revision && client->has_snapshot_revision && revision < client->last_snapshot_revision)
        return CDIN_BACKEND_STALE_VERSION;
    if (message_count == SIZE_MAX) return CDIN_BACKEND_LIMIT_EXCEEDED;
    total_items = message_count + 1u; /* Every group ends with a real ui.flush. */
    if (total_items > client->queue_capacity - client->queue_count)
        return CDIN_BACKEND_QUEUE_FULL;
    if (total_items > UINT64_MAX - client->next_message_sequence ||
        client->next_flush_sequence == UINT64_MAX)
        return CDIN_BACKEND_LIMIT_EXCEEDED;
    if (total_items > SIZE_MAX / sizeof(*prepared)) return CDIN_BACKEND_LIMIT_EXCEEDED;
    prepared = (cdin_protocol_queued *)calloc(total_items, sizeof(*prepared));
    if (prepared == NULL) return CDIN_BACKEND_NO_MEMORY;

    for (index = 0u; index < message_count; index++) {
        const cdin_protocol_message *message = &messages[index];
        if (message->type == CDIN_PROTOCOL_UI_FLUSH ||
            cdin_protocol_message_is_inbound((cdin_protocol_message_type)message->type) ||
            cdin_protocol_message_name((cdin_protocol_message_type)message->type) == NULL ||
            message->payload == NULL || message->payload_length == 0u ||
            message->payload_length > client->server->max_message_bytes ||
            cdin_protocol_validate_payload((cdin_protocol_message_type)message->type,
                                           message->payload, message->payload_length) != CDIN_PROTOCOL_OK) {
            while (index > 0u) free(prepared[--index].payload);
            free(prepared);
            return CDIN_BACKEND_INVALID_ARGUMENT;
        }
        prepared[index].payload = (uint8_t *)malloc(message->payload_length);
        if (prepared[index].payload == NULL) {
            while (index > 0u) free(prepared[--index].payload);
            free(prepared);
            return CDIN_BACKEND_NO_MEMORY;
        }
        memcpy(prepared[index].payload, message->payload, message->payload_length);
        prepared[index].payload_length = message->payload_length;
        prepared[index].type = (uint32_t)message->type;
        prepared[index].coalesce_key = message->coalesce_key;
    }

    flush_sequence = client->next_flush_sequence + 1u;
    flush_length = build_flush_payload(flush_payload, sizeof(flush_payload), flush_sequence,
                                       flush_scope, has_revision, revision);
    if (flush_length == 0u || flush_length > client->server->max_message_bytes) {
        for (index = 0u; index < message_count; index++) free(prepared[index].payload);
        free(prepared);
        return CDIN_BACKEND_INTERNAL;
    }
    prepared[message_count].payload = (uint8_t *)malloc(flush_length);
    if (prepared[message_count].payload == NULL) {
        for (index = 0u; index < message_count; index++) free(prepared[index].payload);
        free(prepared);
        return CDIN_BACKEND_NO_MEMORY;
    }
    memcpy(prepared[message_count].payload, flush_payload, flush_length);
    prepared[message_count].payload_length = flush_length;
    prepared[message_count].type = CDIN_PROTOCOL_UI_FLUSH;

    next_message_sequence = client->next_message_sequence;
    for (index = 0u; index < total_items; index++) {
        prepared[index].sequence = ++next_message_sequence;
        prepared[index].flush_sequence = flush_sequence;
    }
    memcpy(client->queue + client->queue_count, prepared, total_items * sizeof(*prepared));
    client->queue_count += total_items;
    client->next_message_sequence = next_message_sequence;
    client->next_flush_sequence = flush_sequence;
    client->pending_flushes++;
    if (has_revision) {
        client->last_snapshot_revision = revision;
        client->has_snapshot_revision = 1;
    }
    *out_flush_sequence = flush_sequence;
    free(prepared); /* Payload ownership moved into the client queue. */
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_DEBUG, "ui_batch_published",
                      client->id, CDIN_BACKEND_OK, flush_sequence);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_publish_batch(cdin_protocol_client *client,
                                                 const cdin_protocol_message *messages,
                                                 size_t message_count,
                                                 uint64_t *out_flush_sequence)
{
    return protocol_publish_batch(client, messages, message_count, "response", 0, 0u,
                                  out_flush_sequence);
}

cdin_backend_status cdin_protocol_publish_snapshot(cdin_protocol_client *client,
                                                    const cdin_protocol_message *messages,
                                                    size_t message_count,
                                                    uint64_t revision,
                                                    uint64_t *out_flush_sequence)
{
    return protocol_publish_batch(client, messages, message_count, "snapshot", 1, revision,
                                  out_flush_sequence);
}

cdin_backend_status cdin_protocol_flush(cdin_protocol_client *client, uint64_t *out_sequence)
{
    uint8_t payload[64];
    size_t payload_length;
    size_t i;
    uint64_t sequence;
    cdin_protocol_message marker;
    cdin_backend_status status;
    if (client == NULL || out_sequence == NULL || client->closed || !client->attached)
        return CDIN_BACKEND_INVALID_ARGUMENT;
    if (client->pending_flushes >= client->server->max_unacked_flushes) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "flush_backpressure",
                          client->id, CDIN_BACKEND_BUSY, (uint64_t)client->pending_flushes);
        return CDIN_BACKEND_BUSY;
    }
    if (client->next_flush_sequence == UINT64_MAX) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_ERROR, "flush_sequence_exhausted",
                          client->id, CDIN_BACKEND_LIMIT_EXCEEDED, client->next_flush_sequence);
        return CDIN_BACKEND_LIMIT_EXCEEDED;
    }
    sequence = client->next_flush_sequence + 1u;
    payload_length = build_flush_payload(payload, sizeof(payload), sequence, "response", 0, 0u);
    if (payload_length == 0u) return CDIN_BACKEND_INTERNAL;
    if (payload_length > client->server->max_message_bytes) return CDIN_BACKEND_LIMIT_EXCEEDED;
    memset(&marker, 0, sizeof(marker));
    marker.type = CDIN_PROTOCOL_UI_FLUSH;
    marker.payload = payload;
    marker.payload_length = payload_length;
    status = protocol_enqueue(client, &marker, 1);
    if (status != CDIN_BACKEND_OK) return status;
    client->next_flush_sequence = sequence;
    for (i = 0u; i < client->queue_count; i++) {
        if (client->queue[i].flush_sequence == 0u) client->queue[i].flush_sequence = sequence;
    }
    client->pending_flushes++;
    *out_sequence = sequence;
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_TRACE, "flush_published",
                      client->id, CDIN_BACKEND_OK, sequence);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_ack(cdin_protocol_client *client, uint64_t sequence)
{
    size_t i = 0u;
    size_t newly_acked = 0u;
    if (client == NULL || client->closed || !client->attached) return CDIN_BACKEND_INVALID_STATE;
    if (sequence <= client->acknowledged_sequence || sequence > client->next_flush_sequence) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "ack_rejected",
                          client->id, CDIN_BACKEND_INVALID_ARGUMENT, sequence);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    newly_acked = (size_t)(sequence - client->acknowledged_sequence);
    if (newly_acked > client->pending_flushes) return CDIN_BACKEND_INTERNAL;
    client->acknowledged_sequence = sequence;
    while (i < client->queue_count) {
        if (client->queue[i].flush_sequence != 0u && client->queue[i].flush_sequence <= sequence) {
            cdin_protocol_remove_item(client, i);
        } else i++;
    }
    client->pending_flushes -= newly_acked;
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_DEBUG, "ack_accepted",
                      client->id, CDIN_BACKEND_OK, sequence);
    return CDIN_BACKEND_OK;
}

size_t cdin_protocol_pending_messages(const cdin_protocol_client *client)
{
    return client == NULL || client->closed ? 0u : client->queue_count;
}

cdin_backend_status cdin_protocol_consume_control_message(cdin_protocol_client *client)
{
    if (client == NULL || client->closed) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (client->queue_count == 0u) return CDIN_BACKEND_NOT_FOUND;
    if (client->queue[0].flush_sequence != 0u ||
        (client->queue[0].type != CDIN_PROTOCOL_HELLO_ACK &&
         client->queue[0].type != CDIN_PROTOCOL_DETACH_ACK)) {
        return CDIN_BACKEND_INVALID_STATE;
    }
    cdin_protocol_remove_item(client, 0u);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_message_at(const cdin_protocol_client *client,
                                               size_t index, cdin_protocol_message *out_message)
{
    const cdin_protocol_queued *item;
    if (client == NULL || out_message == NULL || client->closed) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (index >= client->queue_count) return CDIN_BACKEND_NOT_FOUND;
    item = &client->queue[index];
    out_message->type = item->type;
    out_message->sequence = item->sequence;
    out_message->coalesce_key = item->coalesce_key;
    out_message->flush_sequence = item->flush_sequence;
    out_message->payload = item->payload;
    out_message->payload_length = item->payload_length;
    return CDIN_BACKEND_OK;
}


cdin_backend_status cdin_protocol_accept_input_sequence(cdin_protocol_client *client,
                                                         uint64_t sequence)
{
    if (client == NULL || client->closed) return CDIN_BACKEND_INVALID_STATE;
    if (sequence == 0u || sequence <= client->last_inbound_sequence) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "input_sequence_rejected",
                          client->id, CDIN_BACKEND_INVALID_ARGUMENT, sequence);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    client->last_inbound_sequence = sequence;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_receive(cdin_protocol_client *client,
                                           const cdin_protocol_message_view *message)
{
    cdin_protocol_queued *item;
    uint8_t *copy = NULL;
    if (client == NULL || message == NULL || client->closed || !client->attached) return CDIN_BACKEND_INVALID_STATE;
    if (!cdin_protocol_message_is_inbound(message->type) ||
        message->type == CDIN_PROTOCOL_HELLO || message->type == CDIN_PROTOCOL_ATTACH ||
        message->type == CDIN_PROTOCOL_DETACH || message->type == CDIN_PROTOCOL_ACK) {
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if (cdin_protocol_message_requires_driver(message->type) && !client->driver) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "observer_mutation_rejected",
                          client->id, CDIN_BACKEND_PERMISSION_DENIED, (uint64_t)message->type);
        return CDIN_BACKEND_PERMISSION_DENIED;
    }
    if (message->payload_length > client->server->max_message_bytes) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "input_message_size_rejected",
                          client->id, CDIN_BACKEND_LIMIT_EXCEEDED, (uint64_t)message->payload_length);
        return CDIN_BACKEND_LIMIT_EXCEEDED;
    }
    if (cdin_protocol_validate_payload(message->type, message->payload,
                                       message->payload_length) != CDIN_PROTOCOL_OK) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "input_schema_rejected",
                          client->id, CDIN_BACKEND_INVALID_ARGUMENT, (uint64_t)message->type);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if (client->inbound_count == client->inbound_capacity) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "input_queue_full",
                          client->id, CDIN_BACKEND_QUEUE_FULL, (uint64_t)client->inbound_count);
        return CDIN_BACKEND_QUEUE_FULL;
    }
    if (message->payload_length != 0u) {
        copy = (uint8_t *)malloc(message->payload_length);
        if (copy == NULL) return CDIN_BACKEND_NO_MEMORY;
        memcpy(copy, message->payload, message->payload_length);
    }
    item = &client->inbound[client->inbound_count++];
    item->type = (uint32_t)message->type;
    item->sequence = message->sequence;
    item->coalesce_key = 0u;
    item->flush_sequence = message->sequence;
    item->payload = copy;
    item->payload_length = message->payload_length;
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_TRACE, "input_queued",
                      client->id, CDIN_BACKEND_OK, message->sequence);
    return CDIN_BACKEND_OK;
}

size_t cdin_protocol_pending_events(const cdin_protocol_client *client)
{
    return client == NULL || client->closed ? 0u : client->inbound_count;
}

cdin_backend_status cdin_protocol_event_at(const cdin_protocol_client *client,
                                            size_t index,
                                            cdin_protocol_message *out_message)
{
    const cdin_protocol_queued *item;
    if (client == NULL || out_message == NULL || client->closed) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (index >= client->inbound_count) return CDIN_BACKEND_NOT_FOUND;
    item = &client->inbound[index];
    out_message->type = item->type;
    out_message->sequence = item->sequence;
    out_message->coalesce_key = 0u;
    out_message->flush_sequence = item->flush_sequence;
    out_message->payload = item->payload;
    out_message->payload_length = item->payload_length;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_consume_event(cdin_protocol_client *client)
{
    size_t index;
    if (client == NULL || client->closed) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (client->inbound_count == 0u) return CDIN_BACKEND_NOT_FOUND;
    cdin_protocol_clear_item(&client->inbound[0]);
    for (index = 1u; index < client->inbound_count; index++) {
        client->inbound[index - 1u] = client->inbound[index];
    }
    client->inbound_count--;
    memset(&client->inbound[client->inbound_count], 0, sizeof(*client->inbound));
    return CDIN_BACKEND_OK;
}

cdin_protocol_client *cdin_protocol_server_client_at(const cdin_protocol_server *server,
                                                      size_t index)
{
    if (server == NULL || index >= server->max_clients) return NULL;
    return server->clients[index];
}

uint64_t cdin_protocol_server_session_id(const cdin_protocol_server *server)
{ return server == NULL ? 0u : server->session_id; }

uint64_t cdin_protocol_client_id(const cdin_protocol_client *client)
{ return client == NULL || client->closed ? 0u : client->id; }

int cdin_protocol_client_is_greeted(const cdin_protocol_client *client)
{ return client != NULL && !client->closed && client->greeted; }

int cdin_protocol_client_is_attached(const cdin_protocol_client *client)
{ return client != NULL && !client->closed && client->attached; }

size_t cdin_protocol_client_payload_limit(const cdin_protocol_client *client)
{ return client == NULL || client->closed ? 0u : client->server->max_message_bytes; }

int cdin_protocol_client_has_snapshot(const cdin_protocol_client *client)
{ return client != NULL && !client->closed && client->has_snapshot_revision; }

uint64_t cdin_protocol_client_snapshot_revision(const cdin_protocol_client *client)
{ return client == NULL || client->closed ? 0u : client->last_snapshot_revision; }

uint64_t cdin_protocol_client_session_id(const cdin_protocol_client *client)
{ return client == NULL || client->closed ? 0u : client->server->session_id; }


void *cdin_protocol_client_server_context(const cdin_protocol_client *client)
{
    return client != NULL && client->server != NULL
        ? client->server->diagnostic_context : NULL;
}
