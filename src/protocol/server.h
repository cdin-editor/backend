#ifndef CDIN_BACKEND_PROTOCOL_SERVER_H
#define CDIN_BACKEND_PROTOCOL_SERVER_H
#include "../core/status.h"
#include <stddef.h>
#include <stdint.h>
typedef struct cdin_protocol_server cdin_protocol_server;
typedef struct cdin_protocol_client cdin_protocol_client;
typedef void (*cdin_protocol_diagnostic_fn)(void *context, int level,
                                             const char *event, uint64_t client_id,
                                             cdin_backend_status status, uint64_t detail);
typedef enum cdin_protocol_message_type {
    CDIN_PROTOCOL_UI_OPTIONS = 1,
    CDIN_PROTOCOL_UI_STYLE_TABLE,
    CDIN_PROTOCOL_UI_LAYOUT,
    CDIN_PROTOCOL_VIEW_LINES,
    CDIN_PROTOCOL_VIEW_INVALIDATE,
    CDIN_PROTOCOL_UI_STATUSLINE,
    CDIN_PROTOCOL_UI_PROMPT,
    CDIN_PROTOCOL_UI_PANEL,
    CDIN_PROTOCOL_UI_NOTIFY,
    CDIN_PROTOCOL_REQUEST_CLIPBOARD_GET,
    CDIN_PROTOCOL_REQUEST_CLIPBOARD_SET,
    CDIN_PROTOCOL_REQUEST_DIALOG,
    CDIN_PROTOCOL_ERROR
} cdin_protocol_message_type;
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
} cdin_protocol_hello_ack;
typedef struct cdin_protocol_message {
    uint32_t type;
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
cdin_backend_status cdin_protocol_enqueue(cdin_protocol_client *client,
                                           const cdin_protocol_message *message);
cdin_backend_status cdin_protocol_flush(cdin_protocol_client *client, uint64_t *out_sequence);
cdin_backend_status cdin_protocol_ack(cdin_protocol_client *client, uint64_t sequence);
size_t cdin_protocol_pending_messages(const cdin_protocol_client *client);
cdin_backend_status cdin_protocol_message_at(const cdin_protocol_client *client,
                                               size_t index, cdin_protocol_message *out_message);
#endif
