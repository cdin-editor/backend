#ifndef CDIN_BACKEND_PROTOCOL_H
#define CDIN_BACKEND_PROTOCOL_H

#include "backend.h"
#include "protocol.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdin_protocol_client cdin_backend_frontend;
typedef cdin_protocol_message_type cdin_backend_message_type;

#define CDIN_BACKEND_MESSAGE_UI_OPTIONS CDIN_PROTOCOL_UI_OPTIONS
#define CDIN_BACKEND_MESSAGE_UI_STYLE_TABLE CDIN_PROTOCOL_UI_STYLE_TABLE
#define CDIN_BACKEND_MESSAGE_UI_LAYOUT CDIN_PROTOCOL_UI_LAYOUT
#define CDIN_BACKEND_MESSAGE_VIEW_LINES CDIN_PROTOCOL_VIEW_LINES
#define CDIN_BACKEND_MESSAGE_VIEW_INVALIDATE CDIN_PROTOCOL_VIEW_INVALIDATE
#define CDIN_BACKEND_MESSAGE_UI_STATUSLINE CDIN_PROTOCOL_UI_STATUSLINE
#define CDIN_BACKEND_MESSAGE_UI_PROMPT CDIN_PROTOCOL_UI_PROMPT
#define CDIN_BACKEND_MESSAGE_UI_PANEL CDIN_PROTOCOL_UI_PANEL
#define CDIN_BACKEND_MESSAGE_UI_NOTIFY CDIN_PROTOCOL_UI_NOTIFY
#define CDIN_BACKEND_MESSAGE_CLIPBOARD_GET CDIN_PROTOCOL_REQUEST_CLIPBOARD_GET
#define CDIN_BACKEND_MESSAGE_CLIPBOARD_SET CDIN_PROTOCOL_REQUEST_CLIPBOARD_SET
#define CDIN_BACKEND_MESSAGE_DIALOG CDIN_PROTOCOL_REQUEST_DIALOG
#define CDIN_BACKEND_MESSAGE_ERROR CDIN_PROTOCOL_ERROR

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
    uint64_t sequence;       /* Per-connection frame sequence. */
    uint64_t coalesce_key;
    uint64_t flush_sequence; /* Atomic ui.flush group acknowledged by the client. */
    const uint8_t *payload;
    size_t payload_length;
} cdin_backend_message;

/* Lifecycle helpers are also useful to in-process embedders. Serialized
 * transports use receive()/encode_message() and the same validated CBOR core. */
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
/* Remove the oldest non-flush control reply after the transport has written
 * it successfully. Flush-group messages are removed only by ACK. */
cdin_backend_status cdin_backend_frontend_consume_control_message(
    cdin_backend_frontend *frontend);
cdin_backend_status cdin_backend_frontend_message_at(const cdin_backend_frontend *frontend,
                                                       size_t index,
                                                       cdin_backend_message *out_message);


/* Accept one length-prefixed CBOR frame from an attached frontend. This API is
 * called on the Backend/Runtime owner thread after the transport has read a
 * complete frame; it validates schema, sequence and driver permissions before
 * copying the event into the bounded inbound queue. */
cdin_backend_status cdin_backend_frontend_receive(cdin_backend_frontend *frontend,
                                                    const uint8_t *frame,
                                                    size_t frame_length);
size_t cdin_backend_frontend_pending_events(const cdin_backend_frontend *frontend);
cdin_backend_status cdin_backend_frontend_event_at(const cdin_backend_frontend *frontend,
                                                      size_t index,
                                                      cdin_backend_message *out_message);
cdin_backend_status cdin_backend_frontend_consume_event(cdin_backend_frontend *frontend);

/* Serialize an already queued outbound semantic message into one wire frame.
 * The result is written into the caller buffer; message payload remains borrowed
 * from the queue and is never modified by the codec. */
cdin_backend_status cdin_backend_frontend_encode_message(const cdin_backend_frontend *frontend,
                                                            size_t index,
                                                            uint8_t *output,
                                                            size_t output_capacity,
                                                            size_t *out_length);

#ifdef __cplusplus
}
#endif
#endif
