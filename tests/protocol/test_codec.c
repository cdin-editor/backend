#include <cdin/protocol.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct memory_pipe {
    uint8_t bytes[4096];
    size_t length;
    size_t position;
    size_t max_chunk;
} memory_pipe;

static ptrdiff_t pipe_read(void *context, uint8_t *buffer, size_t length)
{
    memory_pipe *pipe = (memory_pipe *)context;
    size_t available = pipe->length - pipe->position;
    size_t count = length < available ? length : available;
    if (count > pipe->max_chunk) count = pipe->max_chunk;
    if (count == 0u) return 0;
    memcpy(buffer, pipe->bytes + pipe->position, count);
    pipe->position += count;
    return (ptrdiff_t)count;
}

static ptrdiff_t pipe_write(void *context, const uint8_t *buffer, size_t length)
{
    memory_pipe *pipe = (memory_pipe *)context;
    size_t count = length < pipe->max_chunk ? length : pipe->max_chunk;
    if (count > sizeof(pipe->bytes) - pipe->length) return -1;
    memcpy(pipe->bytes + pipe->length, buffer, count);
    pipe->length += count;
    return (ptrdiff_t)count;
}

static void test_binary_paste_round_trip(void)
{
    static const uint8_t payload[] = {
        0xa1u,             /* map(1) */
        0x65u, 'b','y','t','e','s',
        0x43u, 0xffu, 0x00u, 0x61u /* byte string: invalid UTF-8 + NUL + 'a' */
    };
    uint8_t frame[256];
    size_t frame_length = 0u;
    cdin_protocol_message_view input = {
        CDIN_PROTOCOL_VERSION_MAJOR,
        CDIN_PROTOCOL_VERSION_MINOR,
        17u,
        CDIN_PROTOCOL_INPUT_PASTE,
        "core",
        payload,
        sizeof(payload)
    };
    cdin_protocol_message_view decoded;
    cdin_protocol_status status;

    status = cdin_protocol_encode_frame(&input, frame, sizeof(frame), &frame_length,
                                        CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES);
    assert(status == CDIN_PROTOCOL_OK);
    assert(frame_length > sizeof(payload) + CDIN_PROTOCOL_FRAME_PREFIX_BYTES);
    status = cdin_protocol_decode_frame(frame, frame_length, &decoded,
                                        CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES);
    assert(status == CDIN_PROTOCOL_OK);
    assert(decoded.type == CDIN_PROTOCOL_INPUT_PASTE);
    assert(decoded.sequence == 17u);
    assert(decoded.payload_length == sizeof(payload));
    assert(memcmp(decoded.payload, payload, sizeof(payload)) == 0);
}

static void test_schema_rejection(void)
{
    static const uint8_t valid[] = { 0xa1u, 0x64u, 't','e','x','t', 0x62u, 'o','k' };
    static const uint8_t invalid_type[] = { 0xa1u, 0x64u, 't','e','x','t', 0x41u, 'x' };
    static const uint8_t duplicate[] = {
        0xa2u, 0x64u, 't','e','x','t', 0x61u, 'a',
        0x64u, 't','e','x','t', 0x61u, 'b'
    };
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_INPUT_TEXT, valid, sizeof(valid)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_INPUT_TEXT, invalid_type, sizeof(invalid_type)) == CDIN_PROTOCOL_INVALID_SCHEMA);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_INPUT_TEXT, duplicate, sizeof(duplicate)) == CDIN_PROTOCOL_INVALID_SCHEMA);
}

static void test_ui_flush_scopes(void)
{
    static const uint8_t response_flush[] = {
        0xa2u,
        0x65u, 's','c','o','p','e', 0x68u, 'r','e','s','p','o','n','s','e',
        0x68u, 's','e','q','u','e','n','c','e', 0x01u
    };
    static const uint8_t snapshot_flush[] = {
        0xa3u,
        0x65u, 's','c','o','p','e', 0x68u, 's','n','a','p','s','h','o','t',
        0x68u, 'r','e','v','i','s','i','o','n', 0x18u, 0x2au,
        0x68u, 's','e','q','u','e','n','c','e', 0x01u
    };
    static const uint8_t snapshot_missing_revision[] = {
        0xa2u,
        0x65u, 's','c','o','p','e', 0x68u, 's','n','a','p','s','h','o','t',
        0x68u, 's','e','q','u','e','n','c','e', 0x01u
    };
    static const uint8_t response_with_revision[] = {
        0xa3u,
        0x65u, 's','c','o','p','e', 0x68u, 'r','e','s','p','o','n','s','e',
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u,
        0x68u, 's','e','q','u','e','n','c','e', 0x02u
    };
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_FLUSH, response_flush,
                                          sizeof(response_flush)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_FLUSH, snapshot_flush,
                                          sizeof(snapshot_flush)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_FLUSH, snapshot_missing_revision,
                                          sizeof(snapshot_missing_revision)) == CDIN_PROTOCOL_INVALID_SCHEMA);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_FLUSH, response_with_revision,
                                          sizeof(response_with_revision)) == CDIN_PROTOCOL_INVALID_SCHEMA);
}

static void test_semantic_schema_constraints(void)
{
    static const uint8_t key_ok[] = {
        0xa3u,
        0x63u, 'k','e','y', 0x61u, 'A',
        0x66u, 'a','c','t','i','o','n', 0x65u, 'p','r','e','s','s',
        0x69u, 'm','o','d','i','f','i','e','r','s', 0x81u, 0x64u, 'c','t','r','l'
    };
    static const uint8_t key_bad_action[] = {
        0xa3u,
        0x63u, 'k','e','y', 0x61u, 'A',
        0x66u, 'a','c','t','i','o','n', 0x66u, 'l','a','u','n','c','h',
        0x69u, 'm','o','d','i','f','i','e','r','s', 0x80u
    };
    static const uint8_t key_duplicate_modifier[] = {
        0xa3u,
        0x63u, 'k','e','y', 0x61u, 'A',
        0x66u, 'a','c','t','i','o','n', 0x65u, 'p','r','e','s','s',
        0x69u, 'm','o','d','i','f','i','e','r','s', 0x82u,
        0x64u, 'c','t','r','l', 0x64u, 'c','t','r','l'
    };
    static const uint8_t resize_zero_width[] = {
        0xa3u,
        0x65u, 'u','n','i','t','s', 0x63u, 'c','e','l',
        0x65u, 'w','i','d','t','h', 0x00u,
        0x66u, 'h','e','i','g','h','t', 0x18u, 0x18u
    };
    static const uint8_t line_valid[] = {
        0xa4u,
        0x65u, 'l','i','n','e','s', 0x81u,
            0xa2u,
                0x65u, 'b','y','t','e','s', 0x43u, 'a','b','c',
                0x65u, 's','p','a','n','s', 0x81u,
                    0xa3u,
                        0x63u, 'e','n','d', 0x03u,
                        0x64u, 'r','o','l','e', 0x64u, 't','e','x','t',
                        0x65u, 's','t','a','r','t', 0x00u,
        0x67u, 'v','e','r','s','i','o','n', 0x01u,
        0x67u, 'v','i','e','w','_','i','d', 0x61u, 'v',
        0x6au, 'f','i','r','s','t','_','l','i','n','e', 0x00u
    };
    static const uint8_t line_valid_with_total[] = {
        0xa5u,
        0x65u, 'l','i','n','e','s', 0x81u,
            0xa1u, 0x65u, 'b','y','t','e','s', 0x43u, 'a','b','c',
        0x67u, 'v','e','r','s','i','o','n', 0x01u,
        0x67u, 'v','i','e','w','_','i','d', 0x61u, 'v',
        0x6au, 'f','i','r','s','t','_','l','i','n','e', 0x00u,
        0x6bu, 't','o','t','a','l','_','l','i','n','e','s', 0x01u
    };
    static const uint8_t line_bad_total[] = {
        0xa5u,
        0x65u, 'l','i','n','e','s', 0x81u,
            0xa1u, 0x65u, 'b','y','t','e','s', 0x43u, 'a','b','c',
        0x67u, 'v','e','r','s','i','o','n', 0x01u,
        0x67u, 'v','i','e','w','_','i','d', 0x61u, 'v',
        0x6au, 'f','i','r','s','t','_','l','i','n','e', 0x01u,
        0x6bu, 't','o','t','a','l','_','l','i','n','e','s', 0x01u
    };
    static const uint8_t cursor_with_scroll_anchor[] = {
        0xa4u,
        0x67u, 'v','e','r','s','i','o','n', 0x01u,
        0x67u, 'v','i','e','w','_','i','d', 0x61u, 'v',
        0x68u, 'p','o','s','i','t','i','o','n', 0x00u,
        0x6du, 's','c','r','o','l','l','_','a','n','c','h','o','r', 0x00u
    };
    static const uint8_t line_bad_span[] = {
        0xa4u,
        0x65u, 'l','i','n','e','s', 0x81u,
            0xa2u,
                0x65u, 'b','y','t','e','s', 0x43u, 'a','b','c',
                0x65u, 's','p','a','n','s', 0x81u,
                    0xa3u,
                        0x63u, 'e','n','d', 0x04u,
                        0x64u, 'r','o','l','e', 0x64u, 't','e','x','t',
                        0x65u, 's','t','a','r','t', 0x00u,
        0x67u, 'v','e','r','s','i','o','n', 0x01u,
        0x67u, 'v','i','e','w','_','i','d', 0x61u, 'v',
        0x6au, 'f','i','r','s','t','_','l','i','n','e', 0x00u
    };

    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_INPUT_KEY, key_ok, sizeof(key_ok)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_INPUT_KEY, key_bad_action, sizeof(key_bad_action)) == CDIN_PROTOCOL_INVALID_SCHEMA);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_INPUT_KEY, key_duplicate_modifier, sizeof(key_duplicate_modifier)) == CDIN_PROTOCOL_INVALID_SCHEMA);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_WINDOW_RESIZE, resize_zero_width, sizeof(resize_zero_width)) == CDIN_PROTOCOL_INVALID_SCHEMA);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_VIEW_LINES, line_valid, sizeof(line_valid)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_VIEW_LINES, line_valid_with_total, sizeof(line_valid_with_total)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_VIEW_LINES, line_bad_total, sizeof(line_bad_total)) == CDIN_PROTOCOL_INVALID_SCHEMA);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_CURSOR, cursor_with_scroll_anchor, sizeof(cursor_with_scroll_anchor)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_VIEW_LINES, line_bad_span, sizeof(line_bad_span)) == CDIN_PROTOCOL_INVALID_SCHEMA);
}

static int decode_hex_fixture(const char *hex, uint8_t *output, size_t capacity, size_t *out_length);

/* The ratio is encoded as an unsigned 16-bit CBOR integer. Mutate only the
 * known split fixture so boundary tests exercise the schema, not the decoder. */
static int set_ratio_bp(uint8_t *bytes, size_t length, uint16_t ratio_bp)
{
    size_t index;
    for (index = 0u; index + 2u < length; index++) {
        if (bytes[index] == 0x19u && bytes[index + 1u] == 0x13u &&
            bytes[index + 2u] == 0x88u) {
            bytes[index + 1u] = (uint8_t)(ratio_bp >> 8u);
            bytes[index + 2u] = (uint8_t)(ratio_bp & 0xffu);
            return 1;
        }
    }
    return 0;
}

static void test_ui_layout_schema(void)
{
    /* Empty and one-leaf PaneTree snapshots are valid layout states. */
    static const uint8_t empty_layout[] = {
        0xa2u, 0x66u, 'l','a','y','o','u','t', 0xa2u,
        0x64u, 'r','o','o','t', 0xf6u,
        0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0xf6u,
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u
    };
    static const uint8_t leaf_layout[] = {
        0xa2u, 0x66u, 'l','a','y','o','u','t', 0xa2u,
        0x64u, 'r','o','o','t', 0xa4u,
        0x62u, 'i','d', 0x66u, 'l','e','a','f','-','1',
        0x64u, 'k','i','n','d', 0x64u, 'l','e','a','f',
        0x65u, 'v','i','e','w','s', 0x81u, 0x66u, 'v','i','e','w','-','1',
        0x6bu, 'a','c','t','i','v','e','_','v','i','e','w', 0x66u, 'v','i','e','w','-','1',
        0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0x66u, 'l','e','a','f','-','1',
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u
    };
    static const uint8_t stale_active_leaf[] = {
        0xa2u, 0x66u, 'l','a','y','o','u','t', 0xa2u,
        0x64u, 'r','o','o','t', 0xa4u,
        0x62u, 'i','d', 0x66u, 'l','e','a','f','-','1',
        0x64u, 'k','i','n','d', 0x64u, 'l','e','a','f',
        0x65u, 'v','i','e','w','s', 0x81u, 0x66u, 'v','i','e','w','-','1',
        0x6bu, 'a','c','t','i','v','e','_','v','i','e','w', 0x66u, 'v','i','e','w','-','1',
        0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0x66u, 'l','e','a','f','-','2',
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u
    };
    static const char split_layout_hex[] =
        "a2666c61796f7574a264726f6f74a66269646773706c69742d31646178697368766572746963616c"
        "646b696e646573706c6974656669727374a4626964666c6561662d31646b696e64646c656166"
        "6576696577738166766965772d316b6163746976655f7669657766766965772d31667365636f6e64"
        "a4626964666c6561662d32646b696e64646c6561666576696577738166766965772d326b616374"
        "6976655f7669657766766965772d3268726174696f5f62701913886b6163746976655f6c656166"
        "666c6561662d32687265766973696f6e01";
    uint8_t split_layout[256];
    uint8_t ratio_layout[256];
    size_t split_layout_length = 0u;
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_LAYOUT, empty_layout,
                                          sizeof(empty_layout)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_LAYOUT, leaf_layout,
                                          sizeof(leaf_layout)) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_LAYOUT, stale_active_leaf,
                                          sizeof(stale_active_leaf)) == CDIN_PROTOCOL_INVALID_SCHEMA);
    assert(decode_hex_fixture(split_layout_hex, split_layout, sizeof(split_layout),
                              &split_layout_length));
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_LAYOUT, split_layout,
                                          split_layout_length) == CDIN_PROTOCOL_OK);

    memcpy(ratio_layout, split_layout, split_layout_length);
    assert(set_ratio_bp(ratio_layout, split_layout_length, 1000u));
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_LAYOUT, ratio_layout,
                                          split_layout_length) == CDIN_PROTOCOL_OK);

    memcpy(ratio_layout, split_layout, split_layout_length);
    assert(set_ratio_bp(ratio_layout, split_layout_length, 8999u));
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_LAYOUT, ratio_layout,
                                          split_layout_length) == CDIN_PROTOCOL_OK);

    memcpy(ratio_layout, split_layout, split_layout_length);
    assert(set_ratio_bp(ratio_layout, split_layout_length, 999u));
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_LAYOUT, ratio_layout,
                                          split_layout_length) == CDIN_PROTOCOL_INVALID_SCHEMA);

    memcpy(ratio_layout, split_layout, split_layout_length);
    assert(set_ratio_bp(ratio_layout, split_layout_length, 9000u));
    assert(cdin_protocol_validate_payload(CDIN_PROTOCOL_UI_LAYOUT, ratio_layout,
                                          split_layout_length) == CDIN_PROTOCOL_INVALID_SCHEMA);
}

static int decode_hex_fixture(const char *hex, uint8_t *output, size_t capacity, size_t *out_length)
{
    size_t index;
    size_t length;
    if (hex == NULL || output == NULL || out_length == NULL) return 0;
    length = strlen(hex);
    if ((length % 2u) != 0u || length / 2u > capacity) return 0;
    for (index = 0u; index < length / 2u; index++) {
        unsigned int value;
        char pair[3] = { hex[index * 2u], hex[index * 2u + 1u], '\0' };
        if (sscanf(pair, "%2x", &value) != 1) return 0;
        output[index] = (uint8_t)value;
    }
    *out_length = length / 2u;
    return 1;
}

static void test_stream_partial_io(void)
{
    static const uint8_t payload[] = { 0xa2u, 0x65u, 't','h','e','m','e', 0x64u, 'd','a','r','k',
                                       0x68u, 'r','e','v','i','s','i','o','n', 0x01u };
    uint8_t encoded[256];
    uint8_t received[256];
    size_t encoded_length = 0u;
    size_t received_length = 0u;
    memory_pipe pipe;
    cdin_protocol_io io;
    cdin_protocol_message_view message = {
        CDIN_PROTOCOL_VERSION_MAJOR, CDIN_PROTOCOL_VERSION_MINOR, 3u,
        CDIN_PROTOCOL_UI_OPTIONS, "core", payload, sizeof(payload)
    };
    cdin_protocol_message_view decoded;

    memset(&pipe, 0, sizeof(pipe));
    pipe.max_chunk = 2u;
    io.context = &pipe;
    io.read = pipe_read;
    io.write = pipe_write;
    assert(cdin_protocol_encode_frame(&message, encoded, sizeof(encoded), &encoded_length,
                                      CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES) == CDIN_PROTOCOL_OK);
    assert(cdin_protocol_write_frame(&io, encoded, encoded_length) == CDIN_PROTOCOL_OK);
    pipe.position = 0u;
    assert(cdin_protocol_read_frame(&io, received, sizeof(received), &received_length,
                                    CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES) == CDIN_PROTOCOL_OK);
    assert(received_length == encoded_length);
    assert(memcmp(received, encoded, encoded_length) == 0);
    assert(cdin_protocol_decode_frame(received, received_length, &decoded,
                                      CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES) == CDIN_PROTOCOL_OK);
    assert(decoded.type == CDIN_PROTOCOL_UI_OPTIONS);
}

static void test_frame_bounds(void)
{
    static const uint8_t zero_length[] = { 0, 0, 0, 0 };
    static const uint8_t impossible_length[] = { 0, 0x10, 0, 1 };
    cdin_protocol_message_view out;
    assert(cdin_protocol_decode_frame(zero_length, sizeof(zero_length), &out, 1024u) == CDIN_PROTOCOL_LIMIT_EXCEEDED);
    assert(cdin_protocol_decode_frame(impossible_length, sizeof(impossible_length), &out, 1024u) == CDIN_PROTOCOL_LIMIT_EXCEEDED);
}

static void test_direction_and_roles(void)
{
    assert(cdin_protocol_message_is_inbound(CDIN_PROTOCOL_INPUT_KEY));
    assert(!cdin_protocol_message_is_inbound(CDIN_PROTOCOL_UI_LAYOUT));
    assert(cdin_protocol_message_requires_driver(CDIN_PROTOCOL_INPUT_PASTE));
    assert(cdin_protocol_message_requires_driver(CDIN_PROTOCOL_REQUEST_COMMAND));
    assert(!cdin_protocol_message_requires_driver(CDIN_PROTOCOL_ACK));
    assert(!cdin_protocol_message_requires_driver(CDIN_PROTOCOL_VIEW_REQUEST_LINES));
}

int main(void)
{
    test_binary_paste_round_trip();
    test_schema_rejection();
    test_ui_flush_scopes();
    test_semantic_schema_constraints();
    test_ui_layout_schema();
    test_stream_partial_io();
    test_frame_bounds();
    test_direction_and_roles();
    puts("PASS protocol codec: CBOR envelope, binary paste, strict schema constraints, partial stream I/O and bounds");
    return 0;
}
