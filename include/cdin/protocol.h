#ifndef CDIN_PROTOCOL_H
#define CDIN_PROTOCOL_H

/*
 * Small, transport-independent CDIN Frontend Protocol codec.
 *
 * The library has no Kernel, Lua, GUI or OS dependency. It is intentionally a
 * separable target so a frontend can link the codec without linking Backend.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDIN_PROTOCOL_VERSION_MAJOR 1u
#define CDIN_PROTOCOL_VERSION_MINOR 0u
#define CDIN_PROTOCOL_VERSION_PATCH 0u
#define CDIN_PROTOCOL_FRAME_PREFIX_BYTES 4u
#define CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES (1024u * 1024u)
#define CDIN_PROTOCOL_MAX_DEPTH 32u
#define CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS (UINT64_C(1) << 9u)
#define CDIN_PROTOCOL_CAP_DRIVER_ROLE_EVENTS_NAME "driver-role-events/1"

typedef enum cdin_protocol_status {
    CDIN_PROTOCOL_OK = 0,
    CDIN_PROTOCOL_INVALID_ARGUMENT,
    CDIN_PROTOCOL_INVALID_FRAME,
    CDIN_PROTOCOL_INVALID_SCHEMA,
    CDIN_PROTOCOL_VERSION_MISMATCH,
    CDIN_PROTOCOL_UNSUPPORTED,
    CDIN_PROTOCOL_LIMIT_EXCEEDED,
    CDIN_PROTOCOL_BUFFER_TOO_SMALL,
    CDIN_PROTOCOL_IO_ERROR,
    CDIN_PROTOCOL_EOF
} cdin_protocol_status;

typedef enum cdin_protocol_message_type {
    /* Backend -> Frontend. Numeric values 1..13 retain the development API. */
    CDIN_PROTOCOL_UI_OPTIONS = 1,
    CDIN_PROTOCOL_UI_STYLE_TABLE = 2,
    CDIN_PROTOCOL_UI_LAYOUT = 3,
    CDIN_PROTOCOL_VIEW_LINES = 4,
    CDIN_PROTOCOL_VIEW_INVALIDATE = 5,
    CDIN_PROTOCOL_UI_STATUSLINE = 6,
    CDIN_PROTOCOL_UI_PROMPT = 7,
    CDIN_PROTOCOL_UI_PANEL = 8,
    CDIN_PROTOCOL_UI_NOTIFY = 9,
    CDIN_PROTOCOL_REQUEST_CLIPBOARD_GET = 10,
    CDIN_PROTOCOL_REQUEST_CLIPBOARD_SET = 11,
    CDIN_PROTOCOL_REQUEST_DIALOG = 12,
    CDIN_PROTOCOL_ERROR = 13,
    CDIN_PROTOCOL_UI_CURSOR = 14,
    CDIN_PROTOCOL_UI_SELECTION = 15,
    CDIN_PROTOCOL_UI_FLUSH = 16,
    CDIN_PROTOCOL_HELLO_ACK = 17,
    CDIN_PROTOCOL_DETACH_ACK = 18,
    CDIN_PROTOCOL_DRIVER_ROLE_CHANGED = 19,

    /* Frontend -> Backend. */
    CDIN_PROTOCOL_HELLO = 64,
    CDIN_PROTOCOL_ATTACH = 65,
    CDIN_PROTOCOL_DETACH = 66,
    CDIN_PROTOCOL_INPUT_KEY = 67,
    CDIN_PROTOCOL_INPUT_TEXT = 68,
    CDIN_PROTOCOL_INPUT_MOUSE = 69,
    CDIN_PROTOCOL_INPUT_PASTE = 70,
    CDIN_PROTOCOL_WINDOW_RESIZE = 71,
    CDIN_PROTOCOL_FOCUS = 72,
    CDIN_PROTOCOL_VIEW_REQUEST_LINES = 73,
    CDIN_PROTOCOL_VIEW_SET_POSITION = 74,
    CDIN_PROTOCOL_REQUEST_COMMAND = 75,
    CDIN_PROTOCOL_RESPONSE_CLIPBOARD = 76,
    CDIN_PROTOCOL_RESPONSE_DIALOG = 77,
    CDIN_PROTOCOL_ACK = 78
} cdin_protocol_message_type;

typedef struct cdin_protocol_message_view {
    uint32_t major;
    uint32_t minor;
    uint64_t sequence;
    cdin_protocol_message_type type;
    const char *name_space;       /* Borrowed from a static codec string. */
    const uint8_t *payload;        /* Borrowed CBOR map in the input frame. */
    size_t payload_length;
} cdin_protocol_message_view;

typedef ptrdiff_t (*cdin_protocol_read_fn)(void *context, uint8_t *buffer, size_t length);
typedef ptrdiff_t (*cdin_protocol_write_fn)(void *context, const uint8_t *buffer, size_t length);

typedef struct cdin_protocol_io {
    void *context;
    cdin_protocol_read_fn read;
    cdin_protocol_write_fn write;
} cdin_protocol_io;

const char *cdin_protocol_status_name(cdin_protocol_status status);
const char *cdin_protocol_message_name(cdin_protocol_message_type type);
int cdin_protocol_message_is_inbound(cdin_protocol_message_type type);
int cdin_protocol_message_requires_driver(cdin_protocol_message_type type);

/* Payloads are definite-length CBOR maps; this validates each message schema. */
cdin_protocol_status cdin_protocol_validate_payload(cdin_protocol_message_type type,
                                                     const uint8_t *payload,
                                                     size_t payload_length);
cdin_protocol_status cdin_protocol_payload_get_uint(const uint8_t *payload,
                                                    size_t payload_length,
                                                    const char *field,
                                                    uint64_t *out_value);
cdin_protocol_status cdin_protocol_payload_get_text(const uint8_t *payload,
                                                    size_t payload_length,
                                                    const char *field,
                                                    char *output,
                                                    size_t output_capacity,
                                                    size_t *out_length);
cdin_protocol_status cdin_protocol_payload_get_bool(const uint8_t *payload,
                                                    size_t payload_length,
                                                    const char *field,
                                                    int *out_value);
/* Return one already-validated CBOR value from a payload map. The returned
 * slice borrows the original payload and remains valid only as long as it. */
cdin_protocol_status cdin_protocol_payload_get_raw(const uint8_t *payload,
                                                    size_t payload_length,
                                                    const char *field,
                                                    const uint8_t **out_value,
                                                    size_t *out_value_length);

/* Frame = 4-byte unsigned big-endian body length + deterministic CBOR envelope. */
cdin_protocol_status cdin_protocol_encode_frame(const cdin_protocol_message_view *message,
                                                 uint8_t *output,
                                                 size_t output_capacity,
                                                 size_t *out_length,
                                                 size_t max_frame_bytes);
cdin_protocol_status cdin_protocol_decode_frame(const uint8_t *frame,
                                                 size_t frame_length,
                                                 cdin_protocol_message_view *out_message,
                                                 size_t max_frame_bytes);

/* Read/write exactly one framed message; callbacks may perform partial I/O. */
cdin_protocol_status cdin_protocol_read_frame(const cdin_protocol_io *io,
                                               uint8_t *buffer,
                                               size_t capacity,
                                               size_t *out_length,
                                               size_t max_frame_bytes);
cdin_protocol_status cdin_protocol_write_frame(const cdin_protocol_io *io,
                                                const uint8_t *frame,
                                                size_t frame_length);

#ifdef __cplusplus
}
#endif
#endif
