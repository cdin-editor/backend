#ifndef CDIN_BACKEND_PROTOCOL_H
#define CDIN_BACKEND_PROTOCOL_H

#include "backend.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdin_protocol_client cdin_backend_frontend;
typedef enum cdin_backend_message_type {
    CDIN_BACKEND_MESSAGE_UI_OPTIONS = 1,
    CDIN_BACKEND_MESSAGE_UI_STYLE_TABLE,
    CDIN_BACKEND_MESSAGE_UI_LAYOUT,
    CDIN_BACKEND_MESSAGE_VIEW_LINES,
    CDIN_BACKEND_MESSAGE_VIEW_INVALIDATE,
    CDIN_BACKEND_MESSAGE_UI_STATUSLINE,
    CDIN_BACKEND_MESSAGE_UI_PROMPT,
    CDIN_BACKEND_MESSAGE_UI_PANEL,
    CDIN_BACKEND_MESSAGE_UI_NOTIFY,
    CDIN_BACKEND_MESSAGE_CLIPBOARD_GET,
    CDIN_BACKEND_MESSAGE_CLIPBOARD_SET,
    CDIN_BACKEND_MESSAGE_DIALOG,
    CDIN_BACKEND_MESSAGE_ERROR
} cdin_backend_message_type;

typedef struct cdin_backend_hello {
    uint32_t major;
    uint32_t minor;
    const char *frontend_name;
    uint64_t capabilities;
} cdin_backend_hello;

typedef struct cdin_backend_hello_ack {
    uint32_t major;
    uint32_t minor;
    uint64_t accepted_capabilities;
    uint64_t client_id;
    uint64_t session_id;
    int driver;
} cdin_backend_hello_ack;

typedef struct cdin_backend_message {
    uint32_t type;
    uint64_t coalesce_key;
    uint64_t flush_sequence;
    const uint8_t *payload;
    size_t payload_length;
} cdin_backend_message;

/* These functions exchange semantic messages, not a serialized wire format.
 * The Spec has deliberately left JSON/binary encoding open (OD-22). */
cdin_backend_status cdin_backend_frontend_open(cdin_backend *backend,
                                                 cdin_backend_frontend **out_frontend);
cdin_backend_status cdin_backend_frontend_hello(cdin_backend_frontend *frontend,
                                                  const cdin_backend_hello *hello,
                                                  cdin_backend_hello_ack *ack);
cdin_backend_status cdin_backend_frontend_attach(cdin_backend_frontend *frontend);
cdin_backend_status cdin_backend_frontend_detach(cdin_backend_frontend *frontend);
cdin_backend_status cdin_backend_frontend_close(cdin_backend_frontend *frontend);
int cdin_backend_frontend_can_mutate(const cdin_backend_frontend *frontend);
cdin_backend_status cdin_backend_frontend_enqueue(cdin_backend_frontend *frontend,
                                                    const cdin_backend_message *message);
cdin_backend_status cdin_backend_frontend_flush(cdin_backend_frontend *frontend,
                                                 uint64_t *out_sequence);
cdin_backend_status cdin_backend_frontend_ack(cdin_backend_frontend *frontend,
                                                uint64_t sequence);
size_t cdin_backend_frontend_pending(const cdin_backend_frontend *frontend);
cdin_backend_status cdin_backend_frontend_message_at(const cdin_backend_frontend *frontend,
                                                       size_t index,
                                                       cdin_backend_message *out_message);

#ifdef __cplusplus
}
#endif
#endif
