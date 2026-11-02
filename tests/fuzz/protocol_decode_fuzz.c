#include <cdin/protocol.h>

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    cdin_protocol_message_view message;
    if (size >= CDIN_PROTOCOL_FRAME_PREFIX_BYTES) {
        (void)cdin_protocol_decode_frame(data, size, &message,
                                         CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES);
    }
    return 0;
}
