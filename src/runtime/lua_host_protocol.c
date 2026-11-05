static int cbor_write_head(cbor_buffer *buffer, uint8_t major, uint64_t value)
{
    uint8_t bytes[9];
    size_t count = 1u;
    size_t width = 0u;
    size_t index;
    if (value < 24u)
        bytes[0] = (uint8_t)(((uint32_t)major << 5u) | (uint32_t)value);
    else {
        if (value <= UINT8_MAX) width = 1u;
        else if (value <= UINT16_MAX) width = 2u;
        else if (value <= UINT32_MAX) width = 4u;
        else width = 8u;
        bytes[0] = (uint8_t)(((uint32_t)major << 5u) |
            (width == 1u ? 24u : width == 2u ? 25u : width == 4u ? 26u : 27u));
        for (index = 0u; index < width; index++)
            bytes[index + 1u] = (uint8_t)(value >> ((width - index - 1u) * 8u));
        count += width;
    }
    return buffer_append(buffer, bytes, count);
}
