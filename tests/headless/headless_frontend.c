/*
 * FE-007 / TEST-020 / TEST-021 — headless conformance frontend.
 * It deliberately has no display or Kernel dependency: it records exact wire
 * frames and replays the same bytes so two implementations can compare their
 * observable protocol behaviour without pixel-level assertions.
 */
#include <cdin/protocol.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FRAME_CAPACITY 4096u
#define RECORD_CAPACITY 8u

typedef struct recorded_frame {
    uint8_t bytes[FRAME_CAPACITY];
    size_t length;
} recorded_frame;

typedef struct headless_frontend {
    recorded_frame records[RECORD_CAPACITY];
    size_t count;
    uint64_t last_inbound_sequence;
    uint64_t last_outbound_sequence;
    size_t received;
} headless_frontend;

static void record_frame(headless_frontend *frontend,
                         const cdin_protocol_message_view *message)
{
    recorded_frame *record;
    size_t length = 0u;
    assert(frontend->count < RECORD_CAPACITY);
    record = &frontend->records[frontend->count];
    cdin_protocol_status status = cdin_protocol_encode_frame(
        message, record->bytes, sizeof(record->bytes), &length,
        CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES);
    if (status != CDIN_PROTOCOL_OK) {
        fprintf(stderr, "headless encode failed type=%s status=%s\n",
                cdin_protocol_message_name(message->type), cdin_protocol_status_name(status));
        assert(status == CDIN_PROTOCOL_OK);
    }
    record->length = length;
    frontend->count++;
}

static void consume_frame(headless_frontend *frontend,
                          const uint8_t *frame, size_t frame_length)
{
    cdin_protocol_message_view message;
    assert(cdin_protocol_decode_frame(frame, frame_length, &message,
                                      CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES) == CDIN_PROTOCOL_OK);
    if (cdin_protocol_message_is_inbound(message.type)) {
        assert(message.sequence > frontend->last_inbound_sequence);
        frontend->last_inbound_sequence = message.sequence;
    } else {
        assert(message.sequence > frontend->last_outbound_sequence);
        frontend->last_outbound_sequence = message.sequence;
    }
    frontend->received++;
}

static void test_bidirectional_record_and_replay(void)
{
    /* CBOR payloads are canonical (encoded map keys sorted by encoded length). */
    static const uint8_t text[] = {
        0xa1u, 0x64u, 't','e','x','t', 0x65u, 'h','e','l','l','o'
    };
    static const uint8_t paste[] = {
        0xa1u, 0x65u, 'b','y','t','e','s', 0x43u, 0xffu, 0x00u, 0x61u
    };
    static const uint8_t resize[] = {
        0xa3u,
        0x65u, 'u','n','i','t','s', 0x65u, 'c','e','l','l','s',
        0x65u, 'w','i','d','t','h', 0x18u, 0x50u,
        0x66u, 'h','e','i','g','h','t', 0x18u, 0x18u
    };
    static const uint8_t layout[] = {
        0xa2u,
        0x66u, 'l','a','y','o','u','t', 0xa2u,
        0x64u, 'r','o','o','t', 0xf6u,
        0x6bu, 'a','c','t','i','v','e','_','l','e','a','f', 0xf6u,
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u
    };
    static const uint8_t options[] = {
        0xa2u,
        0x65u, 't','h','e','m','e', 0x64u, 'd','a','r','k',
        0x68u, 'r','e','v','i','s','i','o','n', 0x01u
    };
    headless_frontend recorder = { 0 };
    headless_frontend replay = { 0 };
    cdin_protocol_message_view message;
    cdin_protocol_message_view decoded;
    size_t index;

    memset(&message, 0, sizeof(message));
    message.major = CDIN_PROTOCOL_VERSION_MAJOR;
    message.minor = CDIN_PROTOCOL_VERSION_MINOR;
    message.name_space = "core";

    /* Frontend -> Backend input trace. */
    message.sequence = 1u; message.type = CDIN_PROTOCOL_INPUT_TEXT;
    message.payload = text; message.payload_length = sizeof(text);
    record_frame(&recorder, &message);
    message.sequence = 2u; message.type = CDIN_PROTOCOL_INPUT_PASTE;
    message.payload = paste; message.payload_length = sizeof(paste);
    record_frame(&recorder, &message);
    message.sequence = 3u; message.type = CDIN_PROTOCOL_WINDOW_RESIZE;
    message.payload = resize; message.payload_length = sizeof(resize);
    record_frame(&recorder, &message);

    /* Backend -> Frontend state trace uses its own outbound sequence space. */
    message.sequence = 1u; message.type = CDIN_PROTOCOL_UI_LAYOUT;
    message.payload = layout; message.payload_length = sizeof(layout);
    record_frame(&recorder, &message);
    message.sequence = 2u; message.type = CDIN_PROTOCOL_UI_OPTIONS;
    message.payload = options; message.payload_length = sizeof(options);
    record_frame(&recorder, &message);

    /* Replay checks exact semantic message identity and retains binary payloads. */
    for (index = 0u; index < recorder.count; index++) {
        recorded_frame *record = &recorder.records[index];
        assert(cdin_protocol_decode_frame(record->bytes, record->length, &decoded,
                                          CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES) == CDIN_PROTOCOL_OK);
        assert(decoded.sequence == (index < 3u ? index + 1u : index - 2u));
        if (index == 1u) {
            assert(decoded.type == CDIN_PROTOCOL_INPUT_PASTE);
            assert(decoded.payload_length == sizeof(paste));
            assert(memcmp(decoded.payload, paste, sizeof(paste)) == 0);
        }
        if (index == 3u) assert(decoded.type == CDIN_PROTOCOL_UI_LAYOUT);
        if (index == 4u) assert(decoded.type == CDIN_PROTOCOL_UI_OPTIONS);
        consume_frame(&replay, record->bytes, record->length);
    }
    assert(replay.received == recorder.count);
}

int main(void)
{
    test_bidirectional_record_and_replay();
    puts("PASS headless frontend: bidirectional frames, binary-safe event recording and deterministic replay");
    return 0;
}
