/*
 * PROTO-030 / PROTO-031 define semantic messages independently of transport.
 * OD-22, "Encoding of the serialized protocol", is still OPEN. This module
 * therefore implements handshake, driver/observer permissions, flush groups,
 * bounded queues and ACK/backpressure semantics in-process only. Inventing a
 * byte layout here would silently turn an unresolved cross-language contract
 * into a Backend-specific format that Frontends could not rely on.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/frontend-protocol.md
 * https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-22
 */
#include "server.h"
#include <stdlib.h>
#include <string.h>

#define CDIN_PROTOCOL_NAME_MAX 96u

typedef struct cdin_protocol_queued {
    uint32_t type;
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
    uint64_t next_flush_sequence;
    uint64_t acknowledged_sequence;
    size_t pending_flushes;
    int greeted;
    int attached;
    int closed;
    int driver;
};

static void cdin_protocol_log(cdin_protocol_server *server, int level, const char *event,
                              uint64_t client_id, cdin_backend_status status, uint64_t detail)
{
    if (server != NULL && server->diagnostic != NULL) {
        server->diagnostic(server->diagnostic_context, level, event, client_id, status, detail);
    }
}

static void cdin_protocol_clear_item(cdin_protocol_queued *item)
{
    free(item->payload);
    memset(item, 0, sizeof(*item));
}

static void cdin_protocol_remove_item(cdin_protocol_client *client, size_t index)
{
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
            free(client->queue);
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
    if (client->queue == NULL) { free(client); return CDIN_BACKEND_NO_MEMORY; }
    client->server = server;
    client->queue_capacity = server->queue_capacity;
    if (server->next_client_id == UINT64_MAX) { free(client->queue); free(client); return CDIN_BACKEND_LIMIT_EXCEEDED; }
    client->id = ++server->next_client_id;
    for (i = 0u; i < server->max_clients; i++) {
        if (server->clients[i] == NULL) { server->clients[i] = client; break; }
    }
    if (i == server->max_clients) { free(client->queue); free(client); return CDIN_BACKEND_LIMIT_EXCEEDED; }
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
    ack->driver = 0;
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
    if (client->server->driver_client_id == 0u) {
        client->server->driver_client_id = client->id;
        client->driver = 1;
    }
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_INFO, "client_attached", client->id,
                      CDIN_BACKEND_OK, (uint64_t)(client->driver != 0));
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_detach(cdin_protocol_client *client)
{
    size_t i;
    cdin_protocol_server *server;
    if (client == NULL || client->closed || !client->attached) return CDIN_BACKEND_INVALID_STATE;
    server = client->server;
    client->attached = 0;
    client->driver = 0;
    if (server->driver_client_id == client->id) {
        server->driver_client_id = 0u;
        for (i = 0u; i < server->max_clients; i++) {
            cdin_protocol_client *candidate = server->clients[i];
            if (candidate != NULL && candidate->attached && !candidate->closed) {
                candidate->driver = 1;
                server->driver_client_id = candidate->id;
                break;
            }
        }
    }
    cdin_protocol_log(server, CDIN_BACKEND_LOG_INFO, "client_detached", client->id,
                      CDIN_BACKEND_OK, server->driver_client_id);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_client_close(cdin_protocol_client *client)
{
    cdin_protocol_server *server;
    size_t i;
    if (client == NULL || client->closed) return CDIN_BACKEND_INVALID_STATE;
    server = client->server;
    if (client->attached) (void)cdin_protocol_detach(client);
    for (i = 0u; i < server->max_clients; i++) {
        if (server->clients[i] == client) { server->clients[i] = NULL; break; }
    }
    server->client_count--;
    client->closed = 1;
    cdin_protocol_log(server, CDIN_BACKEND_LOG_INFO, "client_closed", client->id,
                      CDIN_BACKEND_OK, (uint64_t)server->client_count);
    for (i = 0u; i < client->queue_count; i++) cdin_protocol_clear_item(&client->queue[i]);
    free(client->queue);
    free(client);
    return CDIN_BACKEND_OK;
}

int cdin_protocol_can_mutate(const cdin_protocol_client *client)
{
    return client != NULL && !client->closed && client->attached && client->driver;
}

cdin_backend_status cdin_protocol_enqueue(cdin_protocol_client *client,
                                           const cdin_protocol_message *message)
{
    uint8_t *copy = NULL;
    size_t i;
    if (client == NULL || message == NULL || client->closed || !client->attached) return CDIN_BACKEND_INVALID_STATE;
    if (message->payload_length != 0u && message->payload == NULL) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "message_rejected_missing_payload",
                          client->id, CDIN_BACKEND_INVALID_ARGUMENT, (uint64_t)message->type);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if (message->payload_length > client->server->max_message_bytes) {
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "message_rejected_size_limit",
                          client->id, CDIN_BACKEND_LIMIT_EXCEEDED, (uint64_t)message->payload_length);
        return CDIN_BACKEND_LIMIT_EXCEEDED;
    }
    if (message->payload_length != 0u) {
        copy = (uint8_t *)malloc(message->payload_length);
        if (copy == NULL) return CDIN_BACKEND_NO_MEMORY;
        memcpy(copy, message->payload, message->payload_length);
    }
    if (message->coalesce_key != 0u) {
        for (i = client->queue_count; i > 0u; i--) {
            cdin_protocol_queued *old = &client->queue[i - 1u];
            if (old->type == message->type && old->coalesce_key == message->coalesce_key && old->flush_sequence == 0u) {
                free(old->payload);
                old->payload = copy;
                old->payload_length = message->payload_length;
                cdin_protocol_log(client->server, CDIN_BACKEND_LOG_TRACE, "message_coalesced",
                                  client->id, CDIN_BACKEND_OK, (uint64_t)message->type);
                return CDIN_BACKEND_OK;
            }
        }
    }
    if (client->queue_count == client->queue_capacity) {
        free(copy);
        cdin_protocol_log(client->server, CDIN_BACKEND_LOG_WARN, "message_queue_full",
                          client->id, CDIN_BACKEND_QUEUE_FULL, (uint64_t)client->queue_count);
        return CDIN_BACKEND_QUEUE_FULL;
    }
    client->queue[client->queue_count].type = message->type;
    client->queue[client->queue_count].coalesce_key = message->coalesce_key;
    client->queue[client->queue_count].payload = copy;
    client->queue[client->queue_count].payload_length = message->payload_length;
    client->queue[client->queue_count].flush_sequence = 0u;
    client->queue_count++;
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_TRACE, "message_queued",
                      client->id, CDIN_BACKEND_OK, (uint64_t)message->type);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_protocol_flush(cdin_protocol_client *client, uint64_t *out_sequence)
{
    size_t i;
    uint64_t sequence;
    if (client == NULL || out_sequence == NULL || client->closed || !client->attached) return CDIN_BACKEND_INVALID_ARGUMENT;
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
    sequence = ++client->next_flush_sequence;
    for (i = 0u; i < client->queue_count; i++) if (client->queue[i].flush_sequence == 0u) client->queue[i].flush_sequence = sequence;
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
    client->acknowledged_sequence = sequence;
    while (i < client->queue_count) {
        if (client->queue[i].flush_sequence != 0u && client->queue[i].flush_sequence <= sequence) {
            cdin_protocol_remove_item(client, i);
        } else i++;
    }
    newly_acked = (size_t)(sequence - (client->next_flush_sequence - (uint64_t)client->pending_flushes));
    if (newly_acked > client->pending_flushes) newly_acked = client->pending_flushes;
    client->pending_flushes -= newly_acked;
    cdin_protocol_log(client->server, CDIN_BACKEND_LOG_DEBUG, "ack_accepted",
                      client->id, CDIN_BACKEND_OK, sequence);
    return CDIN_BACKEND_OK;
}

size_t cdin_protocol_pending_messages(const cdin_protocol_client *client)
{
    return client == NULL || client->closed ? 0u : client->queue_count;
}

cdin_backend_status cdin_protocol_message_at(const cdin_protocol_client *client,
                                               size_t index, cdin_protocol_message *out_message)
{
    const cdin_protocol_queued *item;
    if (client == NULL || out_message == NULL || client->closed) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (index >= client->queue_count) return CDIN_BACKEND_NOT_FOUND;
    item = &client->queue[index];
    out_message->type = item->type;
    out_message->coalesce_key = item->coalesce_key;
    out_message->flush_sequence = item->flush_sequence;
    out_message->payload = item->payload;
    out_message->payload_length = item->payload_length;
    return CDIN_BACKEND_OK;
}
