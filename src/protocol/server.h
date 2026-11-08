#ifndef CDIN_BACKEND_PROTOCOL_SERVER_H
#define CDIN_BACKEND_PROTOCOL_SERVER_H
#include "../core/status.h"
#include <cdin/protocol.h>
#include <stddef.h>
#include <stdint.h>
typedef struct cdin_protocol_server cdin_protocol_server;
typedef struct cdin_protocol_client cdin_protocol_client;
typedef void (*cdin_protocol_diagnostic_fn)(void *context, int level,
                                             const char *event, uint64_t client_id,
                                             cdin_backend_status status, uint64_t detail);

typedef struct cdin_protocol_hello {
    uint32_t major;
    uint32_t minor;
    const char *frontend_name;
    uint64_t capabilities;
} cdin_protocol_hello;
typedef struct cdin_protocol_hello_ack {
    uint32_t major;
    uint32_t minor;
    uint64_t accepted_capabilities;
    uint64_t client_id;
    uint64_t session_id;
    int driver;
    uint64_t driver_role_epoch;
} cdin_protocol_hello_ack;
typedef struct cdin_protocol_message {
    uint32_t type;
    uint64_t sequence;
    uint64_t coalesce_key;
    uint64_t flush_sequence;
    const uint8_t *payload;
    size_t payload_length;
} cdin_protocol_message;

cdin_backend_status cdin_protocol_server_create(size_t max_clients, size_t queue_capacity,
                                                  size_t max_unacked_flushes, uint64_t session_id,
                                                  uint64_t supported_capabilities, size_t max_message_bytes,
                                                  cdin_protocol_diagnostic_fn diagnostic,
                                                  void *diagnostic_context,
                                                  cdin_protocol_server **out_server);
void cdin_protocol_server_destroy(cdin_protocol_server *server);
cdin_backend_status cdin_protocol_client_open(cdin_protocol_server *server,
                                               cdin_protocol_client **out_client);
cdin_backend_status cdin_protocol_hello_accept(cdin_protocol_client *client,
                                                const cdin_protocol_hello *hello,
                                                cdin_protocol_hello_ack *ack);
cdin_backend_status cdin_protocol_attach(cdin_protocol_client *client);
cdin_backend_status cdin_protocol_detach(cdin_protocol_client *client);
cdin_backend_status cdin_protocol_client_close(cdin_protocol_client *client);
int cdin_protocol_can_mutate(const cdin_protocol_client *client);
int cdin_protocol_client_has_capability(const cdin_protocol_client *client, uint64_t capability);
cdin_backend_status cdin_protocol_enqueue(cdin_protocol_client *client,
                                           const cdin_protocol_message *message);
/* Atomically append an ordered UI update and its final ui.flush marker.
 * No queue state changes unless every payload can be copied and validated. */
cdin_backend_status cdin_protocol_publish_batch(cdin_protocol_client *client,
                                                 const cdin_protocol_message *messages,
                                                 size_t message_count,
                                                 uint64_t *out_flush_sequence);
/* Publish a complete Runtime UI snapshot. Its flush marker carries scope="snapshot"
 * and the model revision so Frontends can replace stale UI state atomically. */
cdin_backend_status cdin_protocol_publish_snapshot(cdin_protocol_client *client,
                                                    const cdin_protocol_message *messages,
                                                    size_t message_count,
                                                    uint64_t revision,
                                                    uint64_t *out_flush_sequence);
cdin_backend_status cdin_protocol_flush(cdin_protocol_client *client, uint64_t *out_sequence);
cdin_backend_status cdin_protocol_ack(cdin_protocol_client *client, uint64_t sequence);
size_t cdin_protocol_pending_messages(const cdin_protocol_client *client);
cdin_backend_status cdin_protocol_consume_control_message(cdin_protocol_client *client);
cdin_backend_status cdin_protocol_message_at(const cdin_protocol_client *client,
                                               size_t index, cdin_protocol_message *out_message);
cdin_backend_status cdin_protocol_receive(cdin_protocol_client *client,
                                           const cdin_protocol_message_view *message);
cdin_backend_status cdin_protocol_accept_input_sequence(cdin_protocol_client *client,
                                                         uint64_t sequence);
size_t cdin_protocol_pending_events(const cdin_protocol_client *client);
cdin_backend_status cdin_protocol_event_at(const cdin_protocol_client *client,
                                            size_t index,
                                            cdin_protocol_message *out_message);
cdin_backend_status cdin_protocol_consume_event(cdin_protocol_client *client);
cdin_protocol_client *cdin_protocol_server_client_at(const cdin_protocol_server *server,
                                                      size_t index);
uint64_t cdin_protocol_server_session_id(const cdin_protocol_server *server);
uint64_t cdin_protocol_client_id(const cdin_protocol_client *client);
uint64_t cdin_protocol_client_session_id(const cdin_protocol_client *client);
void *cdin_protocol_client_server_context(const cdin_protocol_client *client);
int cdin_protocol_client_is_greeted(const cdin_protocol_client *client);
int cdin_protocol_client_is_attached(const cdin_protocol_client *client);
size_t cdin_protocol_client_payload_limit(const cdin_protocol_client *client);
int cdin_protocol_client_has_snapshot(const cdin_protocol_client *client);
uint64_t cdin_protocol_client_snapshot_revision(const cdin_protocol_client *client);

#endif
