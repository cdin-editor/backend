#include <cdin/protocol.h>

#include <limits.h>
#include <string.h>

#define CBOR_MAJOR_UINT 0u
#define CBOR_MAJOR_NINT 1u
#define CBOR_MAJOR_BYTES 2u
#define CBOR_MAJOR_TEXT 3u
#define CBOR_MAJOR_ARRAY 4u
#define CBOR_MAJOR_MAP 5u
#define CBOR_MAJOR_SIMPLE 7u
#define CBOR_FALSE 20u
#define CBOR_TRUE 21u
#define CBOR_NULL 22u
#define CBOR_MAX_MAP_FIELDS 64u

typedef struct cbor_cursor {
    const uint8_t *data;
    size_t length;
    size_t offset;
} cbor_cursor;

typedef struct cbor_item {
    uint8_t major;
    uint64_t value;
    size_t start;
    size_t end;
} cbor_item;

typedef enum field_kind {
    FIELD_UINT,
    FIELD_TEXT,
    FIELD_BYTES,
    FIELD_ARRAY,
    FIELD_MAP,
    FIELD_BOOL,
    FIELD_MAP_OR_NULL
} field_kind;

typedef struct field_rule {
    const char *name;
    field_kind kind;
    int required;
} field_rule;

static int cbor_read_head(cbor_cursor *cursor, uint8_t *out_major, uint64_t *out_value)
{
    uint8_t first;
    uint8_t additional;
    uint64_t value;
    size_t extra;
    size_t i;

    if (cursor == NULL || out_major == NULL || out_value == NULL || cursor->offset >= cursor->length) {
        return 0;
    }
    first = cursor->data[cursor->offset++];
    *out_major = (uint8_t)(first >> 5);
    additional = (uint8_t)(first & 0x1fu);
    if (additional < 24u) {
        *out_value = additional;
        return 1;
    }
    if (additional == 24u) extra = 1u;
    else if (additional == 25u) extra = 2u;
    else if (additional == 26u) extra = 4u;
    else if (additional == 27u) extra = 8u;
    else return 0; /* Indefinite-length items are forbidden by the wire contract. */
    if (extra > cursor->length - cursor->offset) return 0;
    value = 0u;
    for (i = 0u; i < extra; i++) value = (value << 8) | cursor->data[cursor->offset++];
    /* Reject non-shortest integer/length encodings for deterministic CBOR. */
    if ((extra == 1u && value < 24u) ||
        (extra == 2u && value <= UINT8_MAX) ||
        (extra == 4u && value <= UINT16_MAX) ||
        (extra == 8u && value <= UINT32_MAX)) return 0;
    *out_value = value;
    return 1;
}

static int utf8_valid(const uint8_t *data, size_t length)
{
    size_t i = 0u;
    while (i < length) {
        uint8_t a = data[i++];
        if (a <= 0x7fu) continue;
        if (a >= 0xc2u && a <= 0xdfu) {
            if (i >= length || (data[i] & 0xc0u) != 0x80u) return 0;
            i++;
        } else if (a >= 0xe0u && a <= 0xefu) {
            uint8_t b;
            if (length - i < 2u) return 0;
            b = data[i++];
            if ((b & 0xc0u) != 0x80u) return 0;
            if ((a == 0xe0u && b < 0xa0u) || (a == 0xedu && b >= 0xa0u)) return 0;
            if ((data[i] & 0xc0u) != 0x80u) return 0;
            i++;
        } else if (a >= 0xf0u && a <= 0xf4u) {
            uint8_t b;
            if (length - i < 3u) return 0;
            b = data[i++];
            if ((b & 0xc0u) != 0x80u) return 0;
            if ((a == 0xf0u && b < 0x90u) || (a == 0xf4u && b > 0x8fu)) return 0;
            if ((data[i] & 0xc0u) != 0x80u || (data[i + 1u] & 0xc0u) != 0x80u) return 0;
            i += 2u;
        } else return 0;
    }
    return 1;
}

static int cbor_skip(cbor_cursor *cursor, unsigned depth, cbor_item *out_item)
{
    uint8_t major;
    uint64_t value;
    uint64_t index;
    cbor_item item;
    if (cursor == NULL || depth > CDIN_PROTOCOL_MAX_DEPTH) return 0;
    item.start = cursor->offset;
    if (!cbor_read_head(cursor, &major, &value)) return 0;
    item.major = major;
    item.value = value;

    if (major == CBOR_MAJOR_UINT || major == CBOR_MAJOR_NINT) {
        /* Scalar integer. */
    } else if (major == CBOR_MAJOR_BYTES || major == CBOR_MAJOR_TEXT) {
        if (value > (uint64_t)(cursor->length - cursor->offset)) return 0;
        if (major == CBOR_MAJOR_TEXT && !utf8_valid(cursor->data + cursor->offset, (size_t)value)) return 0;
        cursor->offset += (size_t)value;
    } else if (major == CBOR_MAJOR_ARRAY) {
        if (value > (uint64_t)cursor->length) return 0;
        for (index = 0u; index < value; index++) {
            if (!cbor_skip(cursor, depth + 1u, NULL)) return 0;
        }
    } else if (major == CBOR_MAJOR_MAP) {
        const uint8_t *keys[CBOR_MAX_MAP_FIELDS];
        size_t key_lengths[CBOR_MAX_MAP_FIELDS];
        const uint8_t *encoded_keys[CBOR_MAX_MAP_FIELDS];
        size_t encoded_key_lengths[CBOR_MAX_MAP_FIELDS];
        size_t key_count = 0u;
        if (value > CBOR_MAX_MAP_FIELDS) return 0;
        for (index = 0u; index < value; index++) {
            cbor_item key_item;
            cbor_cursor key_cursor;
            uint8_t key_major;
            uint64_t key_length_u64;
            const uint8_t *key_data;
            size_t key_length;
            size_t previous;
            if (!cbor_skip(cursor, depth + 1u, &key_item) || key_item.major != CBOR_MAJOR_TEXT) return 0;
            key_cursor.data = cursor->data;
            key_cursor.length = cursor->length;
            key_cursor.offset = key_item.start;
            if (!cbor_read_head(&key_cursor, &key_major, &key_length_u64) ||
                key_major != CBOR_MAJOR_TEXT || key_length_u64 > SIZE_MAX ||
                key_cursor.offset + (size_t)key_length_u64 != key_item.end) return 0;
            key_data = cursor->data + key_cursor.offset;
            key_length = (size_t)key_length_u64;
            for (previous = 0u; previous < key_count; previous++) {
                if (key_lengths[previous] == key_length &&
                    memcmp(keys[previous], key_data, key_length) == 0) return 0;
            }
            {
                size_t encoded_length = key_item.end - key_item.start;
                if (key_count != 0u) {
                    size_t prior_length = encoded_key_lengths[key_count - 1u];
                    if (encoded_length < prior_length ||
                        (encoded_length == prior_length &&
                         memcmp(encoded_keys[key_count - 1u], cursor->data + key_item.start, encoded_length) >= 0))
                        return 0;
                }
                encoded_keys[key_count] = cursor->data + key_item.start;
                encoded_key_lengths[key_count] = encoded_length;
            }
            keys[key_count] = key_data;
            key_lengths[key_count] = key_length;
            key_count++;
            if (!cbor_skip(cursor, depth + 1u, NULL)) return 0;
        }
    } else if (major == CBOR_MAJOR_SIMPLE) {
        /* Booleans and null only. Floats and other simple values are not v1. */
        if (value != CBOR_FALSE && value != CBOR_TRUE && value != CBOR_NULL) return 0;
    } else return 0;

    item.end = cursor->offset;
    if (out_item != NULL) *out_item = item;
    return 1;
}

static int item_matches(const cbor_item *item, field_kind kind)
{
    if (item == NULL) return 0;
    switch (kind) {
    case FIELD_UINT: return item->major == CBOR_MAJOR_UINT;
    case FIELD_TEXT: return item->major == CBOR_MAJOR_TEXT;
    case FIELD_BYTES: return item->major == CBOR_MAJOR_BYTES;
    case FIELD_ARRAY: return item->major == CBOR_MAJOR_ARRAY;
    case FIELD_MAP: return item->major == CBOR_MAJOR_MAP;
    case FIELD_BOOL: return item->major == CBOR_MAJOR_SIMPLE &&
                            (item->value == CBOR_FALSE || item->value == CBOR_TRUE);
    case FIELD_MAP_OR_NULL: return item->major == CBOR_MAJOR_MAP ||
                                   (item->major == CBOR_MAJOR_SIMPLE && item->value == CBOR_NULL);
    }
    return 0;
}

static int cbor_find_field(const uint8_t *data, size_t length, const char *name,
                           cbor_item *out_item, int *out_present)
{
    cbor_cursor cursor;
    uint8_t major;
    uint64_t pairs;
    uint64_t index;
    const char *seen[CBOR_MAX_MAP_FIELDS];
    size_t seen_lengths[CBOR_MAX_MAP_FIELDS];
    size_t seen_count = 0u;
    size_t wanted_length;

    if (data == NULL || name == NULL || out_item == NULL || out_present == NULL) return 0;
    cursor.data = data;
    cursor.length = length;
    cursor.offset = 0u;
    if (!cbor_read_head(&cursor, &major, &pairs) || major != CBOR_MAJOR_MAP || pairs > CBOR_MAX_MAP_FIELDS) return 0;
    wanted_length = strlen(name);
    *out_present = 0;
    memset(out_item, 0, sizeof(*out_item));

    for (index = 0u; index < pairs; index++) {
        cbor_item key_item;
        cbor_item value_item;
        const uint8_t *key_bytes;
        size_t key_length;
        size_t j;
        int duplicate = 0;
        if (!cbor_skip(&cursor, 1u, &key_item) || key_item.major != CBOR_MAJOR_TEXT) return 0;
        key_bytes = data + key_item.start + 1u;
        /* Recover the actual text start after a possibly multi-byte CBOR head. */
        {
            cbor_cursor key_cursor = { data, length, key_item.start };
            uint8_t key_major;
            uint64_t key_size;
            if (!cbor_read_head(&key_cursor, &key_major, &key_size) || key_major != CBOR_MAJOR_TEXT ||
                key_size > SIZE_MAX || key_cursor.offset + (size_t)key_size != key_item.end) return 0;
            key_bytes = data + key_cursor.offset;
            key_length = (size_t)key_size;
        }
        for (j = 0u; j < seen_count; j++) {
            if (seen_lengths[j] == key_length && memcmp(seen[j], key_bytes, key_length) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) return 0;
        seen[seen_count] = (const char *)key_bytes;
        seen_lengths[seen_count] = key_length;
        seen_count++;

        if (!cbor_skip(&cursor, 1u, &value_item)) return 0;
        if (key_length == wanted_length && memcmp(key_bytes, name, key_length) == 0) {
            *out_item = value_item;
            *out_present = 1;
        }
    }
    return cursor.offset == length;
}

static int validate_field(const uint8_t *payload, size_t length, const field_rule *rule)
{
    cbor_item item;
    int present;
    if (!cbor_find_field(payload, length, rule->name, &item, &present)) return 0;
    if (!present) return !rule->required;
    return item_matches(&item, rule->kind);
}

#define RULE(name_, kind_) { name_, kind_, 1 }
#define OPT(name_, kind_) { name_, kind_, 0 }

static const field_rule schema_hello[] = {
    RULE("minimum_major", FIELD_UINT), RULE("minimum_minor", FIELD_UINT),
    RULE("maximum_major", FIELD_UINT), RULE("maximum_minor", FIELD_UINT),
    RULE("frontend_name", FIELD_TEXT), RULE("capabilities", FIELD_ARRAY)
};
static const field_rule schema_hello_ack[] = {
    RULE("major", FIELD_UINT), RULE("minor", FIELD_UINT), RULE("session_id", FIELD_UINT),
    RULE("client_id", FIELD_UINT), RULE("driver", FIELD_BOOL), RULE("accepted_capabilities", FIELD_ARRAY),
    OPT("driver_role_epoch", FIELD_UINT)
};
static const field_rule schema_attach[] = {
    RULE("session_id", FIELD_UINT), RULE("units", FIELD_TEXT), RULE("width", FIELD_UINT), RULE("height", FIELD_UINT)
};
static const field_rule schema_detach[] = { OPT("reason", FIELD_TEXT) };
static const field_rule schema_input_key[] = {
    RULE("key", FIELD_TEXT), RULE("action", FIELD_TEXT), RULE("modifiers", FIELD_ARRAY)
};
static const field_rule schema_input_text[] = { RULE("text", FIELD_TEXT), OPT("composition_id", FIELD_UINT) };
static const field_rule schema_input_mouse[] = {
    RULE("action", FIELD_TEXT), RULE("button", FIELD_TEXT), RULE("modifiers", FIELD_ARRAY),
    OPT("target", FIELD_TEXT), OPT("position", FIELD_MAP)
};
static const field_rule schema_input_paste[] = { RULE("bytes", FIELD_BYTES) };
static const field_rule schema_resize[] = {
    RULE("width", FIELD_UINT), RULE("height", FIELD_UINT), RULE("units", FIELD_TEXT)
};
static const field_rule schema_focus[] = { RULE("focused", FIELD_BOOL) };
static const field_rule schema_request_lines[] = {
    RULE("view_id", FIELD_TEXT), RULE("first_line", FIELD_UINT),
    RULE("line_count", FIELD_UINT), RULE("version", FIELD_UINT),
    OPT("anchor_byte", FIELD_UINT)
};
static const field_rule schema_set_position[] = {
    RULE("view_id", FIELD_TEXT), RULE("anchor", FIELD_UINT),
    RULE("caret", FIELD_UINT), RULE("version", FIELD_UINT)
};
static const field_rule schema_command[] = {
    RULE("name", FIELD_TEXT), RULE("arguments", FIELD_MAP)
};
static const field_rule schema_pane_splitter_resize[] = {
    RULE("ratio_bp", FIELD_UINT), RULE("split_id", FIELD_TEXT),
    RULE("request_id", FIELD_UINT), RULE("layout_revision", FIELD_UINT)
};
static const field_rule schema_pane_splitter_result[] = {
    RULE("status", FIELD_TEXT), RULE("request_id", FIELD_UINT), RULE("layout_revision", FIELD_UINT)
};
static const field_rule schema_ime_composition_start[] = {
    RULE("id", FIELD_UINT), RULE("view_id", FIELD_TEXT), RULE("document_version", FIELD_UINT)
};
static const field_rule schema_ime_composition_update[] = {
    RULE("id", FIELD_UINT), RULE("preedit", FIELD_TEXT), RULE("cursor_byte", FIELD_UINT),
    RULE("selection_start_byte", FIELD_UINT), RULE("selection_end_byte", FIELD_UINT)
};
static const field_rule schema_ime_composition_cancel[] = {
    RULE("id", FIELD_UINT), RULE("reason", FIELD_TEXT)
};
static const field_rule schema_response_clipboard[] = {
    RULE("request_id", FIELD_UINT), RULE("bytes", FIELD_BYTES)
};
static const field_rule schema_response_dialog[] = {
    RULE("request_id", FIELD_UINT), RULE("accepted", FIELD_BOOL), OPT("selected", FIELD_TEXT)
};
static const field_rule schema_ack[] = { RULE("sequence", FIELD_UINT) };
static const field_rule schema_ui_options[] = { RULE("revision", FIELD_UINT), RULE("theme", FIELD_TEXT), RULE("options", FIELD_MAP) };
static const field_rule schema_ui_styles[] = { RULE("revision", FIELD_UINT), RULE("styles", FIELD_MAP) };
static const field_rule schema_ui_layout[] = { RULE("revision", FIELD_UINT), RULE("layout", FIELD_MAP) };
static const field_rule schema_view_lines[] = {
    RULE("view_id", FIELD_TEXT), RULE("version", FIELD_UINT),
    RULE("first_line", FIELD_UINT), RULE("lines", FIELD_ARRAY),
    OPT("total_lines", FIELD_UINT), OPT("first_byte", FIELD_UINT)
};
static const field_rule schema_view_invalidate[] = {
    RULE("view_id", FIELD_TEXT), RULE("version", FIELD_UINT), RULE("ranges", FIELD_ARRAY)
};
static const field_rule schema_ui_statusline[] = { RULE("revision", FIELD_UINT), RULE("status", FIELD_MAP) };
static const field_rule schema_ui_prompt[] = { RULE("revision", FIELD_UINT), RULE("prompt", FIELD_MAP_OR_NULL) };
static const field_rule schema_ui_panel[] = {
    RULE("revision", FIELD_UINT), RULE("panel_id", FIELD_TEXT), RULE("panel", FIELD_MAP_OR_NULL)
};
static const field_rule schema_ui_notify[] = {
    RULE("level", FIELD_TEXT), RULE("text", FIELD_TEXT), OPT("notification_id", FIELD_UINT)
};
static const field_rule schema_clipboard_get[] = { RULE("request_id", FIELD_UINT) };
static const field_rule schema_clipboard_set[] = { RULE("request_id", FIELD_UINT), RULE("bytes", FIELD_BYTES) };
static const field_rule schema_dialog[] = {
    RULE("request_id", FIELD_UINT), RULE("kind", FIELD_TEXT), RULE("options", FIELD_MAP)
};
static const field_rule schema_error[] = {
    RULE("code", FIELD_TEXT), RULE("message", FIELD_TEXT), OPT("request_id", FIELD_UINT)
};
static const field_rule schema_ui_flush[] = {
    RULE("scope", FIELD_TEXT), OPT("revision", FIELD_UINT), RULE("sequence", FIELD_UINT)
};
static const field_rule schema_cursor[] = {
    RULE("view_id", FIELD_TEXT), RULE("version", FIELD_UINT), RULE("position", FIELD_UINT),
    OPT("scroll_anchor", FIELD_UINT)
};
static const field_rule schema_selection[] = {
    RULE("view_id", FIELD_TEXT), RULE("version", FIELD_UINT),
    RULE("anchor", FIELD_UINT), RULE("caret", FIELD_UINT)
};
static const field_rule schema_driver_role_changed[] = {
    RULE("epoch", FIELD_UINT), RULE("driver", FIELD_BOOL)
};

static int cbor_validate_string_array(const uint8_t *data, size_t length)
{
    cbor_cursor cursor = { data, length, 0u };
    const uint8_t *seen[CBOR_MAX_MAP_FIELDS];
    size_t seen_lengths[CBOR_MAX_MAP_FIELDS];
    size_t seen_count = 0u;
    uint8_t major;
    uint64_t count;
    uint64_t i;
    if (!cbor_read_head(&cursor, &major, &count) || major != CBOR_MAJOR_ARRAY || count > CBOR_MAX_MAP_FIELDS) return 0;
    for (i = 0u; i < count; i++) {
        cbor_item item;
        cbor_cursor text_cursor;
        uint8_t text_major;
        uint64_t text_length;
        size_t previous;
        if (!cbor_skip(&cursor, 1u, &item) || item.major != CBOR_MAJOR_TEXT) return 0;
        text_cursor.data = data + item.start;
        text_cursor.length = item.end - item.start;
        text_cursor.offset = 0u;
        if (!cbor_read_head(&text_cursor, &text_major, &text_length) || text_major != CBOR_MAJOR_TEXT ||
            text_length == 0u || text_length > 64u || text_length > SIZE_MAX ||
            text_cursor.offset + (size_t)text_length != text_cursor.length ||
            memchr(text_cursor.data + text_cursor.offset, '\0', (size_t)text_length) != NULL) return 0;
        for (previous = 0u; previous < seen_count; previous++) {
            if (seen_lengths[previous] == (size_t)text_length &&
                memcmp(seen[previous], text_cursor.data + text_cursor.offset, (size_t)text_length) == 0) return 0;
        }
        seen[seen_count] = text_cursor.data + text_cursor.offset;
        seen_lengths[seen_count] = (size_t)text_length;
        seen_count++;
    }
    return cursor.offset == length;
}

static int cbor_field_uint(const uint8_t *data, size_t length, const char *name,
                           uint64_t *out_value)
{
    cbor_item item;
    int present;
    cbor_cursor cursor;
    uint8_t major;
    if (!cbor_find_field(data, length, name, &item, &present) || !present ||
        item.major != CBOR_MAJOR_UINT) return 0;
    cursor.data = data + item.start;
    cursor.length = item.end - item.start;
    cursor.offset = 0u;
    return cbor_read_head(&cursor, &major, out_value) &&
           major == CBOR_MAJOR_UINT && cursor.offset == cursor.length;
}

static int cbor_field_text_equals(const uint8_t *data, size_t length,
                                  const char *name, const char *expected)
{
    cbor_item item;
    int present;
    cbor_cursor cursor;
    uint8_t major;
    uint64_t text_length;
    size_t expected_length;
    if (!cbor_find_field(data, length, name, &item, &present) || !present ||
        item.major != CBOR_MAJOR_TEXT) return 0;
    cursor.data = data + item.start;
    cursor.length = item.end - item.start;
    cursor.offset = 0u;
    if (!cbor_read_head(&cursor, &major, &text_length) ||
        major != CBOR_MAJOR_TEXT || text_length > SIZE_MAX ||
        cursor.offset + (size_t)text_length != cursor.length) return 0;
    expected_length = strlen(expected);
    return (size_t)text_length == expected_length &&
           memcmp(cursor.data + cursor.offset, expected, expected_length) == 0;
}

static int cbor_field_text_in(const uint8_t *data, size_t length,
                              const char *name, const char *const *allowed,
                              size_t allowed_count)
{
    size_t index;
    for (index = 0u; index < allowed_count; index++) {
        if (cbor_field_text_equals(data, length, name, allowed[index])) return 1;
    }
    return 0;
}

static int cbor_field_text_nonempty(const uint8_t *data, size_t length,
                                    const char *name, size_t maximum_length)
{
    cbor_item item;
    int present;
    cbor_cursor cursor;
    uint8_t major;
    uint64_t text_length;
    if (!cbor_find_field(data, length, name, &item, &present) || !present ||
        item.major != CBOR_MAJOR_TEXT) return 0;
    cursor.data = data + item.start;
    cursor.length = item.end - item.start;
    cursor.offset = 0u;
    if (!cbor_read_head(&cursor, &major, &text_length) || major != CBOR_MAJOR_TEXT ||
        text_length == 0u || text_length > maximum_length || text_length > SIZE_MAX ||
        cursor.offset + (size_t)text_length != cursor.length) return 0;
    return memchr(cursor.data + cursor.offset, '\0', (size_t)text_length) == NULL;
}

static int cbor_field_uint_range(const uint8_t *data, size_t length,
                                 const char *name, uint64_t minimum,
                                 uint64_t maximum)
{
    uint64_t value;
    return cbor_field_uint(data, length, name, &value) &&
           value >= minimum && value <= maximum;
}

static int cbor_array_text_values(const uint8_t *data, size_t length,
                                  const char *field, const char *const *allowed,
                                  size_t allowed_count)
{
    cbor_item item;
    int present;
    cbor_cursor cursor;
    uint8_t major;
    uint64_t count;
    uint64_t index;
    const uint8_t *seen[64];
    size_t seen_lengths[64];
    size_t seen_count = 0u;
    if (!cbor_find_field(data, length, field, &item, &present) || !present ||
        item.major != CBOR_MAJOR_ARRAY) return 0;
    cursor.data = data + item.start;
    cursor.length = item.end - item.start;
    cursor.offset = 0u;
    if (!cbor_read_head(&cursor, &major, &count) || major != CBOR_MAJOR_ARRAY || count > 64u) return 0;
    for (index = 0u; index < count; index++) {
        cbor_item value;
        cbor_cursor text_cursor;
        uint8_t text_major;
        uint64_t text_length;
        size_t allowed_index;
        int found = allowed_count == 0u;
        if (!cbor_skip(&cursor, 1u, &value) || value.major != CBOR_MAJOR_TEXT) return 0;
        text_cursor.data = cursor.data + value.start;
        text_cursor.length = value.end - value.start;
        text_cursor.offset = 0u;
        if (!cbor_read_head(&text_cursor, &text_major, &text_length) ||
            text_major != CBOR_MAJOR_TEXT || text_length == 0u || text_length > 64u ||
            text_length > SIZE_MAX || text_cursor.offset + (size_t)text_length != text_cursor.length ||
            memchr(text_cursor.data + text_cursor.offset, '\0', (size_t)text_length) != NULL) return 0;
        for (allowed_index = 0u; allowed_index < allowed_count; allowed_index++) {
            size_t candidate_length = strlen(allowed[allowed_index]);
            if (candidate_length == (size_t)text_length &&
                memcmp(text_cursor.data + text_cursor.offset, allowed[allowed_index], candidate_length) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
        for (allowed_index = 0u; allowed_index < seen_count; allowed_index++) {
            if (seen_lengths[allowed_index] == (size_t)text_length &&
                memcmp(seen[allowed_index], text_cursor.data + text_cursor.offset, (size_t)text_length) == 0) return 0;
        }
        seen[seen_count] = text_cursor.data + text_cursor.offset;
        seen_lengths[seen_count] = (size_t)text_length;
        seen_count++;
    }
    return cursor.offset == cursor.length;
}

static int cbor_map_uint_field(const uint8_t *data, size_t length,
                               const char *name, uint64_t *out_value)
{
    return cbor_field_uint(data, length, name, out_value);
}

static int cbor_validate_line_spans(const uint8_t *data, size_t length,
                                    uint64_t line_bytes)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t count;
    uint64_t index;
    if (!cbor_read_head(&cursor, &major, &count) || major != CBOR_MAJOR_ARRAY || count > 4096u) return 0;
    for (index = 0u; index < count; index++) {
        cbor_item span;
        uint64_t start;
        uint64_t end;
        if (!cbor_skip(&cursor, 1u, &span) || span.major != CBOR_MAJOR_MAP ||
            !cbor_map_uint_field(data + span.start, span.end - span.start, "start", &start) ||
            !cbor_map_uint_field(data + span.start, span.end - span.start, "end", &end) ||
            !cbor_field_text_nonempty(data + span.start, span.end - span.start, "role", 64u) ||
            start > end || end > line_bytes) return 0;
    }
    return cursor.offset == cursor.length;
}

static int cbor_validate_line_array(const uint8_t *data, size_t length)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t count;
    uint64_t index;
    if (!cbor_read_head(&cursor, &major, &count) || major != CBOR_MAJOR_ARRAY || count > 4096u) return 0;
    for (index = 0u; index < count; index++) {
        cbor_item line;
        cbor_item bytes;
        cbor_item spans;
        int bytes_present;
        int spans_present;
        if (!cbor_skip(&cursor, 1u, &line) || line.major != CBOR_MAJOR_MAP ||
            !cbor_find_field(data + line.start, line.end - line.start, "bytes", &bytes, &bytes_present) ||
            !bytes_present || bytes.major != CBOR_MAJOR_BYTES ||
            !cbor_find_field(data + line.start, line.end - line.start, "spans", &spans, &spans_present)) return 0;
        if (spans_present && (spans.major != CBOR_MAJOR_ARRAY ||
            !cbor_validate_line_spans(data + line.start + spans.start, spans.end - spans.start, bytes.value))) return 0;
    }
    return cursor.offset == cursor.length;
}

static int cbor_field_text_value(const uint8_t *data, size_t length, const char *name,
                                 const uint8_t **out_text, size_t *out_length)
{
    cbor_item item;
    cbor_cursor cursor;
    uint8_t major;
    uint64_t text_length;
    int present;
    if (out_text == NULL || out_length == NULL ||
        !cbor_find_field(data, length, name, &item, &present) || !present ||
        item.major != CBOR_MAJOR_TEXT) return 0;
    cursor.data = data + item.start;
    cursor.length = item.end - item.start;
    cursor.offset = 0u;
    if (!cbor_read_head(&cursor, &major, &text_length) || major != CBOR_MAJOR_TEXT ||
        text_length > SIZE_MAX || cursor.offset + (size_t)text_length != cursor.length) return 0;
    *out_text = cursor.data + cursor.offset;
    *out_length = (size_t)text_length;
    return 1;
}

static int cbor_array_has_text(const uint8_t *data, size_t length,
                               const uint8_t *wanted, size_t wanted_length)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t count;
    uint64_t index;
    if (!cbor_read_head(&cursor, &major, &count) || major != CBOR_MAJOR_ARRAY || count > 64u) return 0;
    for (index = 0u; index < count; index++) {
        cbor_item item;
        cbor_cursor text_cursor;
        uint8_t text_major;
        uint64_t text_length;
        if (!cbor_skip(&cursor, 1u, &item) || item.major != CBOR_MAJOR_TEXT) return 0;
        text_cursor.data = cursor.data + item.start;
        text_cursor.length = item.end - item.start;
        text_cursor.offset = 0u;
        if (!cbor_read_head(&text_cursor, &text_major, &text_length) || text_major != CBOR_MAJOR_TEXT ||
            text_length > SIZE_MAX || text_cursor.offset + (size_t)text_length != text_cursor.length) return 0;
        if ((size_t)text_length == wanted_length &&
            memcmp(text_cursor.data + text_cursor.offset, wanted, wanted_length) == 0) return 1;
    }
    return 0;
}

/* Validate the semantic split tree separately from generic CBOR well-formedness.
 * A malformed tree must never reach a renderer as a recursive traversal hazard. */
static int cbor_validate_pane_node(const uint8_t *data, size_t length, unsigned depth,
                                   const uint8_t *active_leaf, size_t active_leaf_length,
                                   int *active_leaf_found)
{
    static const char *const axes[] = { "horizontal", "vertical" };
    cbor_item first;
    cbor_item second;
    const uint8_t *node_id;
    size_t node_id_length;
    const uint8_t *kind;
    size_t kind_length;
    int present;
    if (data == NULL || active_leaf_found == NULL || depth > CDIN_PROTOCOL_MAX_DEPTH ||
        !cbor_field_text_value(data, length, "kind", &kind, &kind_length) ||
        !cbor_field_text_nonempty(data, length, "id", 128u) ||
        !cbor_field_text_value(data, length, "id", &node_id, &node_id_length)) return 0;

    if (kind_length == 4u && memcmp(kind, "leaf", 4u) == 0) {
        cbor_item views;
        const uint8_t *active_view;
        size_t active_view_length;
        cbor_cursor views_cursor;
        uint8_t major;
        uint64_t view_count;
        cbor_item active_item;
        int active_present;
        int active_is_null;
        if (!cbor_find_field(data, length, "views", &views, &present) || !present ||
            views.major != CBOR_MAJOR_ARRAY ||
            !cbor_find_field(data, length, "active_view", &active_item, &active_present) ||
            !active_present) return 0;
        active_is_null = active_item.major == CBOR_MAJOR_SIMPLE && active_item.value == CBOR_NULL;
        if (!active_is_null &&
            (active_item.major != CBOR_MAJOR_TEXT ||
             !cbor_field_text_value(data, length, "active_view", &active_view, &active_view_length))) return 0;
        views_cursor.data = data + views.start;
        views_cursor.length = views.end - views.start;
        views_cursor.offset = 0u;
        if (!cbor_read_head(&views_cursor, &major, &view_count) || major != CBOR_MAJOR_ARRAY ||
            view_count > 64u ||
            !cbor_validate_string_array(data + views.start, views.end - views.start) ||
            (!active_is_null && !cbor_array_has_text(data + views.start, views.end - views.start,
                                                       active_view, active_view_length))) return 0;
        if (node_id_length == active_leaf_length &&
            memcmp(node_id, active_leaf, active_leaf_length) == 0) *active_leaf_found = 1;
        return 1;
    }

    if (kind_length == 5u && memcmp(kind, "split", 5u) == 0) {
        if (!cbor_field_text_in(data, length, "axis", axes, sizeof(axes) / sizeof(axes[0])) ||
            !cbor_field_uint_range(data, length, "ratio_bp", 1000u, 8999u) ||
            !cbor_find_field(data, length, "first", &first, &present) || !present ||
            first.major != CBOR_MAJOR_MAP ||
            !cbor_find_field(data, length, "second", &second, &present) || !present ||
            second.major != CBOR_MAJOR_MAP) return 0;
        if (!cbor_validate_pane_node(data + first.start, first.end - first.start,
                                     depth + 1u, active_leaf, active_leaf_length,
                                     active_leaf_found)) return 0;
        return cbor_validate_pane_node(data + second.start, second.end - second.start,
                                       depth + 1u, active_leaf, active_leaf_length,
                                       active_leaf_found);
    }
    return 0;
}

static int cbor_validate_ui_layout(const uint8_t *data, size_t length)
{
    cbor_item layout;
    cbor_item root;
    cbor_item active;
    const uint8_t *active_leaf = NULL;
    size_t active_leaf_length = 0u;
    int layout_present;
    int root_present;
    int active_present;
    int found = 0;
    if (!cbor_find_field(data, length, "layout", &layout, &layout_present) ||
        !layout_present || layout.major != CBOR_MAJOR_MAP ||
        !cbor_find_field(data + layout.start, layout.end - layout.start,
                         "root", &root, &root_present) || !root_present ||
        !cbor_find_field(data + layout.start, layout.end - layout.start,
                         "active_leaf", &active, &active_present) || !active_present) return 0;
    if (root.major == CBOR_MAJOR_SIMPLE && root.value == CBOR_NULL) {
        return active.major == CBOR_MAJOR_SIMPLE && active.value == CBOR_NULL;
    }
    if (root.major != CBOR_MAJOR_MAP || active.major != CBOR_MAJOR_TEXT) return 0;
    if (!cbor_field_text_value(data + layout.start, layout.end - layout.start,
                               "active_leaf", &active_leaf, &active_leaf_length) ||
        active_leaf_length == 0u || active_leaf_length > 128u) return 0;
    if (!cbor_validate_pane_node(data + layout.start + root.start,
                                 root.end - root.start, 1u,
                                 active_leaf, active_leaf_length, &found)) return 0;
    return found;
}

static int cbor_validate_range_array(const uint8_t *data, size_t length)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t count;
    uint64_t index;
    if (!cbor_read_head(&cursor, &major, &count) || major != CBOR_MAJOR_ARRAY || count > 4096u) return 0;
    for (index = 0u; index < count; index++) {
        cbor_item range;
        uint64_t start;
        uint64_t end;
        if (!cbor_skip(&cursor, 1u, &range) || range.major != CBOR_MAJOR_MAP ||
            !cbor_map_uint_field(data + range.start, range.end - range.start, "start", &start) ||
            !cbor_map_uint_field(data + range.start, range.end - range.start, "end", &end) ||
            start > end) return 0;
    }
    return cursor.offset == cursor.length;
}

static int cbor_option_bool(const uint8_t *data, size_t length, const char *name)
{
    cbor_item item;
    int present;
    return cbor_find_field(data, length, name, &item, &present) &&
           (!present || (item.major == CBOR_MAJOR_SIMPLE &&
                         (item.value == CBOR_FALSE || item.value == CBOR_TRUE)));
}

static int cbor_validate_ui_options(const uint8_t *payload, size_t payload_length)
{
    cbor_item options;
    int present;
    const uint8_t *data;
    size_t length;
    static const char *const cursor_shapes[] = { "block", "bar", "underline" };
    if (!cbor_find_field(payload, payload_length, "options", &options, &present) ||
        !present || options.major != CBOR_MAJOR_MAP) return 0;
    data = payload + options.start;
    length = options.end - options.start;
    if (!cbor_field_uint_range(data, length, "tab_width", 1u, 64u) &&
        cbor_find_field(data, length, "tab_width", &options, &present) && present) return 0;
    if (!cbor_option_bool(data, length, "wrap") ||
        !cbor_option_bool(data, length, "line_numbers") ||
        !cbor_option_bool(data, length, "relative_line_numbers") ||
        !cbor_option_bool(data, length, "cursor_blink")) return 0;
    if (cbor_find_field(data, length, "cursor_shape", &options, &present) && present &&
        !cbor_field_text_in(data, length, "cursor_shape", cursor_shapes,
                            sizeof(cursor_shapes) / sizeof(cursor_shapes[0]))) return 0;
    if (cbor_find_field(data, length, "title", &options, &present) && present &&
        options.major != CBOR_MAJOR_TEXT) return 0;
    return 1;
}

static int ime_utf8_boundary(const uint8_t *bytes, size_t length, uint64_t offset)
{
    if (offset > (uint64_t)length) return 0;
    if (offset == 0u || offset == (uint64_t)length) return 1;
    return (bytes[(size_t)offset] & 0xc0u) != 0x80u;
}

static int cbor_validate_ime_update(const uint8_t *payload, size_t payload_length)
{
    const uint8_t *preedit = NULL;
    size_t preedit_length = 0u;
    uint64_t id = 0u, cursor = 0u, selection_start = 0u, selection_end = 0u;
    if (!cbor_field_uint(payload, payload_length, "id", &id) || id == 0u ||
        !cbor_field_text_value(payload, payload_length, "preedit", &preedit, &preedit_length) ||
        preedit_length > CDIN_PROTOCOL_IME_MAX_PREEDIT_BYTES ||
        !cbor_field_uint(payload, payload_length, "cursor_byte", &cursor) ||
        !cbor_field_uint(payload, payload_length, "selection_start_byte", &selection_start) ||
        !cbor_field_uint(payload, payload_length, "selection_end_byte", &selection_end) ||
        selection_start > selection_end ||
        !ime_utf8_boundary(preedit, preedit_length, cursor) ||
        !ime_utf8_boundary(preedit, preedit_length, selection_start) ||
        !ime_utf8_boundary(preedit, preedit_length, selection_end)) return 0;
    return 1;
}


typedef struct ui_id_set {
    size_t count;
    size_t lengths[128];
    uint8_t values[128][129];
} ui_id_set;

static int cbor_item_text_view(const uint8_t *data, size_t length,
                               const cbor_item *item,
                               const uint8_t **out_text, size_t *out_length)
{
    cbor_cursor cursor;
    uint8_t major;
    uint64_t text_length;
    if (data == NULL || item == NULL || out_text == NULL || out_length == NULL ||
        item->major != CBOR_MAJOR_TEXT || item->end < item->start || item->end > length)
        return 0;
    cursor.data = data + item->start;
    cursor.length = item->end - item->start;
    cursor.offset = 0u;
    if (!cbor_read_head(&cursor, &major, &text_length) || major != CBOR_MAJOR_TEXT ||
        text_length > SIZE_MAX || cursor.offset + (size_t)text_length != cursor.length)
        return 0;
    *out_text = cursor.data + cursor.offset;
    *out_length = (size_t)text_length;
    return 1;
}

static int cbor_optional_text(const uint8_t *data, size_t length, const char *name,
                              size_t maximum, int allow_null)
{
    cbor_item item;
    int present;
    const uint8_t *text;
    size_t text_length;
    if (!cbor_find_field(data, length, name, &item, &present)) return 0;
    if (!present) return 1;
    if (allow_null && item.major == CBOR_MAJOR_SIMPLE && item.value == CBOR_NULL) return 1;
    if (!cbor_item_text_view(data, length, &item, &text, &text_length) ||
        text_length > maximum || memchr(text, 0, text_length) != NULL) return 0;
    return 1;
}

static int cbor_optional_bool(const uint8_t *data, size_t length, const char *name)
{
    cbor_item item;
    int present;
    if (!cbor_find_field(data, length, name, &item, &present)) return 0;
    return !present || (item.major == CBOR_MAJOR_SIMPLE &&
        (item.value == CBOR_FALSE || item.value == CBOR_TRUE));
}

static int ui_id_set_add(ui_id_set *set, const uint8_t *id, size_t id_length)
{
    size_t i;
    if (set == NULL || id == NULL || id_length == 0u || id_length > 128u ||
        memchr(id, 0, id_length) != NULL || set->count >= 128u) return 0;
    for (i = 0u; i < set->count; i++) {
        if (set->lengths[i] == id_length &&
            memcmp(set->values[i], id, id_length) == 0) return 0;
    }
    memcpy(set->values[set->count], id, id_length);
    set->lengths[set->count] = id_length;
    set->count++;
    return 1;
}

static int cbor_validate_ui_id_field(const uint8_t *data, size_t length,
                                     const char *field, ui_id_set *set)
{
    const uint8_t *id = NULL;
    size_t id_length = 0u;
    return cbor_field_text_value(data, length, field, &id, &id_length) &&
           ui_id_set_add(set, id, id_length);
}

static int cbor_validate_ui_rgba(const uint8_t *data, size_t length)
{
    static const char *const channels[] = { "r", "g", "b", "a" };
    size_t i;
    for (i = 0u; i < sizeof(channels) / sizeof(channels[0]); i++) {
        if (!cbor_field_uint_range(data, length, channels[i], 0u, 255u)) return 0;
    }
    return 1;
}

static int ui_role_id_valid(const uint8_t *text, size_t length)
{
    size_t i;
    if (text == NULL || length == 0u || length > 64u) return 0;
    for (i = 0u; i < length; i++) {
        uint8_t c = text[i];
        int alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        int digit = c >= '0' && c <= '9';
        if (i == 0u) {
            if (!alpha && !digit) return 0;
        } else if (!alpha && !digit && c != '.' && c != '_' && c != '-') return 0;
    }
    return 1;
}

static int cbor_validate_ui_style_role(const uint8_t *data, size_t length)
{
    static const char *const bool_fields[] = {
        "bold", "italic", "underline", "strikethrough", "dim", "inverse"
    };
    static const char *const color_fields[] = { "foreground", "background" };
    size_t i;
    for (i = 0u; i < sizeof(bool_fields) / sizeof(bool_fields[0]); i++) {
        if (!cbor_optional_bool(data, length, bool_fields[i])) return 0;
    }
    for (i = 0u; i < sizeof(color_fields) / sizeof(color_fields[0]); i++) {
        cbor_item color;
        int present;
        if (!cbor_find_field(data, length, color_fields[i], &color, &present)) return 0;
        if (present && (color.major != CBOR_MAJOR_MAP ||
            !cbor_validate_ui_rgba(data + color.start, color.end - color.start))) return 0;
    }
    return 1;
}

static int cbor_validate_ui_style_table(const uint8_t *payload, size_t length)
{
    cbor_item styles;
    int present;
    cbor_cursor cursor;
    uint8_t major;
    uint64_t roles;
    uint64_t index;
    if (length > 32768u ||
        !cbor_find_field(payload, length, "styles", &styles, &present) ||
        !present || styles.major != CBOR_MAJOR_MAP) return 0;
    cursor.data = payload + styles.start;
    cursor.length = styles.end - styles.start;
    cursor.offset = 0u;
    if (!cbor_read_head(&cursor, &major, &roles) || major != CBOR_MAJOR_MAP || roles > 256u)
        return 0;
    for (index = 0u; index < roles; index++) {
        cbor_item key;
        cbor_item value;
        const uint8_t *role;
        size_t role_length;
        if (!cbor_skip(&cursor, 1u, &key) ||
            !cbor_item_text_view(cursor.data, cursor.length, &key, &role, &role_length) ||
            !ui_role_id_valid(role, role_length) ||
            !cbor_skip(&cursor, 1u, &value) || value.major != CBOR_MAJOR_MAP ||
            !cbor_validate_ui_style_role(cursor.data + value.start, value.end - value.start))
            return 0;
    }
    return cursor.offset == cursor.length;
}

static int cbor_validate_ui_action(const uint8_t *data, size_t length)
{
    cbor_item arguments;
    int present;
    if (!cbor_field_text_nonempty(data, length, "command", 128u) ||
        !cbor_find_field(data, length, "arguments", &arguments, &present)) return 0;
    return !present || (arguments.major == CBOR_MAJOR_MAP && arguments.end - arguments.start <= 32768u);
}

static int cbor_validate_ui_tree_node(const uint8_t *data, size_t length,
                                      unsigned depth, size_t *nodes, ui_id_set *ids);

static int cbor_validate_ui_tree_nodes(const uint8_t *data, size_t length,
                                       unsigned depth, size_t *nodes, ui_id_set *ids)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t count;
    uint64_t index;
    if (depth > 16u || !cbor_read_head(&cursor, &major, &count) ||
        major != CBOR_MAJOR_ARRAY || count > 128u) return 0;
    for (index = 0u; index < count; index++) {
        cbor_item node;
        if (!cbor_skip(&cursor, 1u, &node) || node.major != CBOR_MAJOR_MAP ||
            !cbor_validate_ui_tree_node(cursor.data + node.start, node.end - node.start,
                                        depth, nodes, ids)) return 0;
    }
    return cursor.offset == cursor.length;
}

static int cbor_validate_ui_tree_node(const uint8_t *data, size_t length,
                                      unsigned depth, size_t *nodes, ui_id_set *ids)
{
    cbor_item children;
    cbor_item action;
    int children_present;
    int action_present;
    if (depth > 16u || *nodes >= 128u ||
        !cbor_validate_ui_id_field(data, length, "id", ids) ||
        !cbor_field_text_value(data, length, "label", &(const uint8_t *){0}, &(size_t){0}) ||
        !cbor_optional_bool(data, length, "expanded") ||
        !cbor_optional_bool(data, length, "enabled") ||
        !cbor_find_field(data, length, "activate", &action, &action_present)) return 0;
    if (action_present && (action.major != CBOR_MAJOR_MAP ||
        !cbor_validate_ui_action(data + action.start, action.end - action.start))) return 0;
    (*nodes)++;
    if (!cbor_find_field(data, length, "children", &children, &children_present)) return 0;
    if (children_present && (children.major != CBOR_MAJOR_ARRAY ||
        !cbor_validate_ui_tree_nodes(data + children.start, children.end - children.start,
                                     depth + 1u, nodes, ids))) return 0;
    return 1;
}

static int cbor_validate_ui_widget(const uint8_t *data, size_t length,
                                   size_t *widgets, ui_id_set *widget_ids)
{
    static const char *const supported_kinds[] = {
        "label", "text", "list", "tree", "button", "input"
    };
    const uint8_t *kind = NULL;
    size_t kind_length = 0u;
    cbor_item item;
    int present;
    size_t i;
    int known = 0;
    if (*widgets >= 128u ||
        !cbor_validate_ui_id_field(data, length, "id", widget_ids) ||
        !cbor_field_text_value(data, length, "kind", &kind, &kind_length) ||
        kind_length == 0u || kind_length > 64u ||
        !cbor_optional_bool(data, length, "enabled") ||
        !cbor_optional_bool(data, length, "visible") ||
        !cbor_optional_text(data, length, "accessible_label", 16384u, 0)) return 0;
    (*widgets)++;
    for (i = 0u; i < sizeof(supported_kinds) / sizeof(supported_kinds[0]); i++) {
        size_t candidate_length = strlen(supported_kinds[i]);
        if (kind_length == candidate_length && memcmp(kind, supported_kinds[i], kind_length) == 0) {
            known = 1;
            break;
        }
    }
    if (!known) {
        return cbor_optional_text(data, length, "label", 16384u, 0) &&
               cbor_optional_text(data, length, "text", 16384u, 0);
    }
    if (kind_length == 5u && memcmp(kind, "label", 5u) == 0) {
        return cbor_field_text_value(data, length, "text", &kind, &kind_length) &&
               kind_length <= 16384u && cbor_optional_text(data, length, "role", 64u, 0);
    }
    if (kind_length == 4u && memcmp(kind, "text", 4u) == 0) {
        cbor_item runs;
        cbor_cursor cursor;
        uint8_t array_major;
        uint64_t count;
        uint64_t index;
        if (!cbor_find_field(data, length, "runs", &runs, &present) ||
            !present || runs.major != CBOR_MAJOR_ARRAY) return 0;
        cursor.data = data + runs.start;
        cursor.length = runs.end - runs.start;
        cursor.offset = 0u;
        if (!cbor_read_head(&cursor, &array_major, &count) ||
            array_major != CBOR_MAJOR_ARRAY || count > 128u) return 0;
        for (index = 0u; index < count; index++) {
            cbor_item run;
            cbor_item role;
            int role_present;
            if (!cbor_skip(&cursor, 1u, &run) || run.major != CBOR_MAJOR_MAP ||
                !cbor_field_text_value(cursor.data + run.start, run.end - run.start,
                                       "text", &kind, &kind_length) ||
                kind_length > 16384u ||
                !cbor_find_field(cursor.data + run.start, run.end - run.start,
                                 "role", &role, &role_present) || !role_present ||
                (role.major != CBOR_MAJOR_TEXT &&
                 !(role.major == CBOR_MAJOR_SIMPLE && role.value == CBOR_NULL))) return 0;
        }
        return cursor.offset == cursor.length;
    }
    if (kind_length == 4u && memcmp(kind, "list", 4u) == 0) {
        cbor_item items;
        cbor_cursor cursor;
        uint8_t array_major;
        uint64_t count;
        uint64_t index;
        ui_id_set ids = {0};
        if (!cbor_find_field(data, length, "items", &items, &present) ||
            !present || items.major != CBOR_MAJOR_ARRAY) return 0;
        cursor.data = data + items.start;
        cursor.length = items.end - items.start;
        cursor.offset = 0u;
        if (!cbor_read_head(&cursor, &array_major, &count) ||
            array_major != CBOR_MAJOR_ARRAY || count > 128u) return 0;
        for (index = 0u; index < count; index++) {
            cbor_item entry;
            cbor_item action;
            int action_present;
            if (!cbor_skip(&cursor, 1u, &entry) || entry.major != CBOR_MAJOR_MAP ||
                !cbor_validate_ui_id_field(cursor.data + entry.start, entry.end - entry.start,
                                           "id", &ids) ||
                !cbor_field_text_value(cursor.data + entry.start, entry.end - entry.start,
                                       "label", &(const uint8_t *){0}, &(size_t){0}) ||
                !cbor_optional_text(cursor.data + entry.start, entry.end - entry.start,
                                    "detail", 16384u, 0) ||
                !cbor_optional_bool(cursor.data + entry.start, entry.end - entry.start, "enabled") ||
                !cbor_find_field(cursor.data + entry.start, entry.end - entry.start,
                                 "activate", &action, &action_present)) return 0;
            if (action_present && (action.major != CBOR_MAJOR_MAP ||
                !cbor_validate_ui_action(cursor.data + entry.start + action.start,
                                         action.end - action.start))) return 0;
        }
        if (cursor.offset != cursor.length ||
            !cbor_optional_text(data, length, "selected_id", 128u, 1)) return 0;
        return 1;
    }
    if (kind_length == 4u && memcmp(kind, "tree", 4u) == 0) {
        cbor_item nodes;
        size_t node_count = 0u;
        ui_id_set node_ids = {0};
        if (!cbor_find_field(data, length, "nodes", &nodes, &present) ||
            !present || nodes.major != CBOR_MAJOR_ARRAY) return 0;
        return cbor_validate_ui_tree_nodes(data + nodes.start, nodes.end - nodes.start,
                                           1u, &node_count, &node_ids);
    }
    if (kind_length == 6u && memcmp(kind, "button", 6u) == 0) {
        cbor_item action;
        const uint8_t *label;
        size_t label_length;
        if (!cbor_field_text_value(data, length, "label", &label, &label_length) ||
            label_length > 16384u ||
            !cbor_find_field(data, length, "activate", &action, &present) ||
            !present || action.major != CBOR_MAJOR_MAP) return 0;
        return cbor_validate_ui_action(data + action.start, action.end - action.start);
    }
    if (kind_length == 5u && memcmp(kind, "input", 5u) == 0) {
        cbor_item submit;
        uint64_t max_bytes = 4096u;
        if (!cbor_field_text_value(data, length, "value", &kind, &kind_length) ||
            kind_length > 16384u ||
            !cbor_optional_text(data, length, "placeholder", 16384u, 0) ||
            !cbor_optional_bool(data, length, "multiline") ||
            !cbor_find_field(data, length, "max_bytes", &item, &present)) return 0;
        if (present && (!cbor_field_uint(data, length, "max_bytes", &max_bytes) ||
            max_bytes > 16384u)) return 0;
        if (kind_length > max_bytes) return 0;
        if (!cbor_find_field(data, length, "submit", &submit, &present) ||
            !present || submit.major != CBOR_MAJOR_MAP ||
            !cbor_validate_ui_action(data + submit.start, submit.end - submit.start)) return 0;
        {
            cbor_item arguments;
            int arguments_present;
            if (cbor_find_field(data + submit.start, submit.end - submit.start,
                                "arguments", &arguments, &arguments_present) && arguments_present) {
                cbor_item existing;
                int value_present;
                if (!cbor_find_field(data + submit.start + arguments.start,
                                     arguments.end - arguments.start,
                                     "value", &existing, &value_present)) return 0;
                if (value_present) return 0;
            }
        }
        return 1;
    }
    return 1;
}

static int cbor_validate_ui_widgets(const uint8_t *data, size_t length,
                                    size_t *out_widgets)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t count;
    uint64_t index;
    size_t widgets = 0u;
    ui_id_set widget_ids = {0};
    if (!cbor_read_head(&cursor, &major, &count) ||
        major != CBOR_MAJOR_ARRAY || count > 128u) return 0;
    for (index = 0u; index < count; index++) {
        cbor_item widget;
        if (!cbor_skip(&cursor, 1u, &widget) || widget.major != CBOR_MAJOR_MAP ||
            !cbor_validate_ui_widget(cursor.data + widget.start, widget.end - widget.start,
                                     &widgets, &widget_ids)) return 0;
    }
    if (cursor.offset != cursor.length) return 0;
    if (out_widgets != NULL) *out_widgets = widgets;
    return 1;
}

static int cbor_validate_ui_panel(const uint8_t *payload, size_t length,
                                  const char *outer_id, size_t outer_id_length,
                                  size_t *out_widgets)
{
    const uint8_t *panel_id = NULL;
    size_t panel_id_length = 0u;
    const uint8_t *kind = NULL;
    size_t kind_length = 0u;
    cbor_item item;
    int present;
    if (length > 131072u ||
        !cbor_field_text_value(payload, length, "kind", &kind, &kind_length) ||
        kind_length == 0u || kind_length > 64u ||
        !cbor_field_text_value(payload, length, "title", &panel_id, &panel_id_length) ||
        panel_id_length > 16384u ||
        !cbor_optional_bool(payload, length, "visible") ||
        !cbor_optional_bool(payload, length, "focusable") ||
        !cbor_optional_bool(payload, length, "scrollable") ||
        !cbor_optional_text(payload, length, "selected_widget_id", 128u, 1)) return 0;
    if (cbor_find_field(payload, length, "id", &item, &present) && present) {
        if (!cbor_item_text_view(payload, length, &item, &panel_id, &panel_id_length) ||
            panel_id_length != outer_id_length ||
            memcmp(panel_id, outer_id, outer_id_length) != 0) return 0;
    }
    if (!cbor_find_field(payload, length, "widgets", &item, &present) ||
        !present || item.major != CBOR_MAJOR_ARRAY ||
        !cbor_validate_ui_widgets(payload + item.start, item.end - item.start, out_widgets)) return 0;
    return 1;
}

static int cbor_validate_ui_prompt(const uint8_t *payload, size_t length,
                                   size_t *out_widgets)
{
    const uint8_t *title = NULL;
    size_t title_length = 0u;
    cbor_item widgets;
    cbor_item cancel;
    int present;
    if (length > 131072u ||
        !cbor_field_text_nonempty(payload, length, "id", 128u) ||
        !cbor_field_text_value(payload, length, "title", &title, &title_length) ||
        title_length > 16384u ||
        !cbor_optional_text(payload, length, "message", 16384u, 0) ||
        !cbor_optional_bool(payload, length, "visible") ||
        !cbor_find_field(payload, length, "widgets", &widgets, &present) ||
        !present || widgets.major != CBOR_MAJOR_ARRAY ||
        !cbor_validate_ui_widgets(payload + widgets.start, widgets.end - widgets.start, out_widgets) ||
        !cbor_find_field(payload, length, "cancel", &cancel, &present)) return 0;
    if (present && cancel.major != CBOR_MAJOR_SIMPLE && cancel.major != CBOR_MAJOR_MAP) return 0;
    if (present && cancel.major == CBOR_MAJOR_MAP &&
        !cbor_validate_ui_action(payload + cancel.start, cancel.end - cancel.start)) return 0;
    if (present && cancel.major == CBOR_MAJOR_SIMPLE && cancel.value != CBOR_NULL) return 0;
    return 1;
}

static int cbor_validate_ui_style_table_message(const uint8_t *payload, size_t length)
{
    return cbor_field_uint(payload, length, "revision", &(uint64_t){0u}) &&
           cbor_validate_ui_style_table(payload, length);
}

static int cbor_validate_ui_panel_message(const uint8_t *payload, size_t length,
                                          size_t *out_widgets)
{
    const uint8_t *panel_id;
    size_t panel_id_length;
    cbor_item panel;
    int present;
    if (!cbor_field_text_value(payload, length, "panel_id", &panel_id, &panel_id_length) ||
        panel_id_length == 0u || panel_id_length > 128u ||
        !cbor_find_field(payload, length, "panel", &panel, &present)) return 0;
    if (panel.major == CBOR_MAJOR_SIMPLE && panel.value == CBOR_NULL) {
        if (out_widgets != NULL) *out_widgets = 0u;
        return 1;
    }
    if (panel.major != CBOR_MAJOR_MAP) return 0;
    return cbor_validate_ui_panel(payload + panel.start, panel.end - panel.start,
                                  (const char *)panel_id, panel_id_length, out_widgets);
}

static int cbor_validate_ui_prompt_message(const uint8_t *payload, size_t length,
                                           size_t *out_widgets)
{
    cbor_item prompt;
    int present;
    if (!cbor_find_field(payload, length, "prompt", &prompt, &present)) return 0;
    if (prompt.major == CBOR_MAJOR_SIMPLE && prompt.value == CBOR_NULL) {
        if (out_widgets != NULL) *out_widgets = 0u;
        return 1;
    }
    if (prompt.major != CBOR_MAJOR_MAP) return 0;
    return cbor_validate_ui_prompt(payload + prompt.start, prompt.end - prompt.start, out_widgets);
}

static int cbor_validate_protocol_semantics(cdin_protocol_message_type type,
                                            const uint8_t *payload,
                                            size_t payload_length)
{
    static const char *const modifiers[] = { "ctrl", "alt", "altgr", "shift" };
    static const char *const key_actions[] = { "press", "release", "repeat" };
    static const char *const mouse_actions[] = { "press", "release", "move", "scroll" };
    static const char *const mouse_buttons[] = { "none", "left", "middle", "right", "wheel_up", "wheel_down", "x1", "x2" };
    static const char *const units[] = { "pixels", "cells" };
    static const char *const notify_levels[] = { "debug", "info", "warn", "error" };
    uint64_t minimum_major;
    uint64_t minimum_minor;
    uint64_t maximum_major;
    uint64_t maximum_minor;
    uint64_t role_epoch;
    uint64_t ratio_bp;
    uint64_t request_id;
    uint64_t composition_id;
    const uint8_t *committed_ime_text = NULL;
    size_t committed_ime_text_length = 0u;
    static const char *const ime_cancel_reasons[] = {
        "ime_cancelled", "focus_lost", "view_changed", "stale_document",
        "selection_changed", "view_removed", "driver_revoked", "session_reset"
    };
    switch (type) {
    case CDIN_PROTOCOL_HELLO:
        if (!cbor_field_text_nonempty(payload, payload_length, "frontend_name", 96u) ||
            !cbor_field_uint(payload, payload_length, "minimum_major", &minimum_major) ||
            !cbor_field_uint(payload, payload_length, "minimum_minor", &minimum_minor) ||
            !cbor_field_uint(payload, payload_length, "maximum_major", &maximum_major) ||
            !cbor_field_uint(payload, payload_length, "maximum_minor", &maximum_minor) ||
            minimum_major > maximum_major ||
            (minimum_major == maximum_major && minimum_minor > maximum_minor)) return 0;
        break;
    case CDIN_PROTOCOL_INPUT_TEXT:
        if (cbor_field_uint(payload, payload_length, "composition_id", &composition_id) &&
            (composition_id == 0u ||
             !cbor_field_text_value(payload, payload_length, "text",
                                    &committed_ime_text, &committed_ime_text_length) ||
             committed_ime_text_length == 0u)) return 0;
        break;
    case CDIN_PROTOCOL_IME_COMPOSITION_START:
        if (!cbor_field_uint(payload, payload_length, "id", &composition_id) ||
            composition_id == 0u ||
            !cbor_field_text_nonempty(payload, payload_length, "view_id", 128u) ||
            !cbor_field_uint(payload, payload_length, "document_version", &role_epoch)) return 0;
        break;
    case CDIN_PROTOCOL_IME_COMPOSITION_UPDATE:
        if (!cbor_validate_ime_update(payload, payload_length)) return 0;
        break;
    case CDIN_PROTOCOL_IME_COMPOSITION_CANCEL:
        if (!cbor_field_uint(payload, payload_length, "id", &composition_id) ||
            composition_id == 0u ||
            !cbor_field_text_in(payload, payload_length, "reason", ime_cancel_reasons,
                                sizeof(ime_cancel_reasons) / sizeof(ime_cancel_reasons[0]))) return 0;
        break;
    case CDIN_PROTOCOL_DRIVER_ROLE_CHANGED:
        if (!cbor_field_uint(payload, payload_length, "epoch", &role_epoch) ||
            role_epoch == 0u) return 0;
        break;
    case CDIN_PROTOCOL_PANE_SPLITTER_RESIZE:
        if (!cbor_field_uint(payload, payload_length, "request_id", &request_id) ||
            request_id == 0u ||
            !cbor_field_text_nonempty(payload, payload_length, "split_id", 128u) ||
            !cbor_field_uint(payload, payload_length, "layout_revision", &role_epoch) ||
            !cbor_field_uint(payload, payload_length, "ratio_bp", &ratio_bp)) return 0;
        break;
    case CDIN_PROTOCOL_PANE_SPLITTER_RESULT: {
        static const char *const result_statuses[] = {
            "applied", "stale_revision", "not_found", "invalid_ratio", "not_driver", "unsupported", "busy"
        };
        if (!cbor_field_uint(payload, payload_length, "request_id", &request_id) ||
            request_id == 0u ||
            !cbor_field_uint(payload, payload_length, "layout_revision", &role_epoch) ||
            !cbor_field_text_in(payload, payload_length, "status", result_statuses,
                                sizeof(result_statuses) / sizeof(result_statuses[0]))) return 0;
        break;
    }
    case CDIN_PROTOCOL_ATTACH:
        if (!cbor_field_text_in(payload, payload_length, "units", units,
                                sizeof(units) / sizeof(units[0]))) return 0;
        if (!cbor_field_uint_range(payload, payload_length, "width", 1u, 32768u) ||
            !cbor_field_uint_range(payload, payload_length, "height", 1u, 32768u)) return 0;
        break;
    case CDIN_PROTOCOL_INPUT_KEY:
        if (!cbor_field_text_nonempty(payload, payload_length, "key", 64u) ||
            !cbor_field_text_in(payload, payload_length, "action", key_actions,
                                sizeof(key_actions) / sizeof(key_actions[0]))) return 0;
        if (!cbor_array_text_values(payload, payload_length, "modifiers", modifiers,
                                    sizeof(modifiers) / sizeof(modifiers[0]))) return 0;
        break;
    case CDIN_PROTOCOL_INPUT_MOUSE:
        if (!cbor_field_text_nonempty(payload, payload_length, "action", 16u) ||
            !cbor_field_text_nonempty(payload, payload_length, "button", 16u) ||
            !cbor_field_text_in(payload, payload_length, "action", mouse_actions,
                                sizeof(mouse_actions) / sizeof(mouse_actions[0])) ||
            !cbor_field_text_in(payload, payload_length, "button", mouse_buttons,
                                sizeof(mouse_buttons) / sizeof(mouse_buttons[0])) ||
            !cbor_array_text_values(payload, payload_length, "modifiers", modifiers,
                                    sizeof(modifiers) / sizeof(modifiers[0]))) return 0;
        break;
    case CDIN_PROTOCOL_WINDOW_RESIZE:
        if (!cbor_field_text_in(payload, payload_length, "units", units,
                                sizeof(units) / sizeof(units[0])) ||
            !cbor_field_uint_range(payload, payload_length, "width", 1u, 32768u) ||
            !cbor_field_uint_range(payload, payload_length, "height", 1u, 32768u)) return 0;
        break;
    case CDIN_PROTOCOL_VIEW_REQUEST_LINES: {
        uint64_t first_line;
        uint64_t line_count;
        if (!cbor_field_text_nonempty(payload, payload_length, "view_id", 128u) ||
            !cbor_field_uint(payload, payload_length, "first_line", &first_line) ||
            !cbor_field_uint_range(payload, payload_length, "line_count", 1u, 4096u) ||
            !cbor_field_uint(payload, payload_length, "line_count", &line_count) ||
            first_line > UINT64_MAX - line_count) return 0;
        break;
    }
    case CDIN_PROTOCOL_VIEW_SET_POSITION:
        if (!cbor_field_text_nonempty(payload, payload_length, "view_id", 128u)) return 0;
        break;
    case CDIN_PROTOCOL_REQUEST_COMMAND:
        if (!cbor_field_text_nonempty(payload, payload_length, "name", 128u)) return 0;
        break;
    case CDIN_PROTOCOL_UI_LAYOUT:
        if (!cbor_validate_ui_layout(payload, payload_length)) return 0;
        break;
    case CDIN_PROTOCOL_UI_OPTIONS:
        if (!cbor_validate_ui_options(payload, payload_length)) return 0;
        break;
    case CDIN_PROTOCOL_UI_STYLE_TABLE:
        if (!cbor_validate_ui_style_table_message(payload, payload_length)) return 0;
        break;
    case CDIN_PROTOCOL_UI_PANEL: {
        size_t widget_count = 0u;
        uint64_t revision;
        if (!cbor_field_uint(payload, payload_length, "revision", &revision) ||
            !cbor_validate_ui_panel_message(payload, payload_length, &widget_count) ||
            widget_count > 128u) return 0;
        break;
    }
    case CDIN_PROTOCOL_UI_PROMPT: {
        size_t widget_count = 0u;
        uint64_t revision;
        if (!cbor_field_uint(payload, payload_length, "revision", &revision) ||
            !cbor_validate_ui_prompt_message(payload, payload_length, &widget_count) ||
            widget_count > 128u) return 0;
        break;
    }
    case CDIN_PROTOCOL_UI_NOTIFY:
        if (!cbor_field_text_nonempty(payload, payload_length, "text", 4096u) ||
            (!cbor_field_text_equals(payload, payload_length, "level", notify_levels[0]) &&
             !cbor_field_text_equals(payload, payload_length, "level", notify_levels[1]) &&
             !cbor_field_text_equals(payload, payload_length, "level", notify_levels[2]) &&
             !cbor_field_text_equals(payload, payload_length, "level", notify_levels[3]))) return 0;
        break;
    case CDIN_PROTOCOL_VIEW_LINES: {
        cbor_item lines;
        cbor_cursor line_cursor;
        uint8_t line_major;
        uint64_t line_count;
        uint64_t first_line;
        uint64_t total_lines;
        int present;
        if (!cbor_find_field(payload, payload_length, "lines", &lines, &present) || !present ||
            !cbor_validate_line_array(payload + lines.start, lines.end - lines.start)) return 0;
        if (!cbor_field_uint(payload, payload_length, "first_line", &first_line) ||
            !cbor_find_field(payload, payload_length, "total_lines", &lines, &present)) return 0;
        if (present) {
            if (!cbor_field_uint(payload, payload_length, "total_lines", &total_lines) ||
                total_lines == 0u || first_line >= total_lines) return 0;
            /* The count must fit within the version-pinned logical document. */
            if (!cbor_find_field(payload, payload_length, "lines", &lines, &present) || !present) return 0;
            line_cursor.data = payload + lines.start;
            line_cursor.length = lines.end - lines.start;
            line_cursor.offset = 0u;
            if (!cbor_read_head(&line_cursor, &line_major, &line_count) ||
                line_major != CBOR_MAJOR_ARRAY || line_count > total_lines - first_line) return 0;
        }
        break;
    }
    case CDIN_PROTOCOL_VIEW_INVALIDATE: {
        cbor_item ranges;
        int present;
        if (!cbor_find_field(payload, payload_length, "ranges", &ranges, &present) || !present ||
            !cbor_validate_range_array(payload + ranges.start, ranges.end - ranges.start)) return 0;
        break;
    }
    default:
        break;
    }
    return 1;
}

static int schema_rules(const uint8_t *payload, size_t payload_length,
                        const field_rule *rules, size_t count)
{
    size_t index;
    cbor_cursor cursor = { payload, payload_length, 0u };
    uint8_t major;
    uint64_t pairs;
    if (!cbor_read_head(&cursor, &major, &pairs) || major != CBOR_MAJOR_MAP || pairs > CBOR_MAX_MAP_FIELDS) return 0;
    for (index = 0u; index < count; index++) {
        if (!validate_field(payload, payload_length, &rules[index])) return 0;
    }
    /* cbor_find_field performs the complete map walk and rejects duplicate keys. */
    {
        cbor_item ignored;
        int present;
        if (!cbor_find_field(payload, payload_length, "__cdin_internal_never_a_field__", &ignored, &present)) return 0;
    }
    return 1;
}

#define VALIDATE(schema_) schema_rules(payload, payload_length, schema_, sizeof(schema_) / sizeof((schema_)[0]))

cdin_protocol_status cdin_protocol_validate_payload(cdin_protocol_message_type type,
                                                     const uint8_t *payload,
                                                     size_t payload_length)
{
    int valid = 0;
    if (payload == NULL || payload_length == 0u) return CDIN_PROTOCOL_INVALID_ARGUMENT;
    switch (type) {
    case CDIN_PROTOCOL_HELLO: valid = VALIDATE(schema_hello); break;
    case CDIN_PROTOCOL_HELLO_ACK: valid = VALIDATE(schema_hello_ack); break;
    case CDIN_PROTOCOL_ATTACH: valid = VALIDATE(schema_attach); break;
    case CDIN_PROTOCOL_DETACH: valid = VALIDATE(schema_detach); break;
    case CDIN_PROTOCOL_INPUT_KEY: valid = VALIDATE(schema_input_key); break;
    case CDIN_PROTOCOL_INPUT_TEXT: valid = VALIDATE(schema_input_text); break;
    case CDIN_PROTOCOL_INPUT_MOUSE: valid = VALIDATE(schema_input_mouse); break;
    case CDIN_PROTOCOL_INPUT_PASTE: valid = VALIDATE(schema_input_paste); break;
    case CDIN_PROTOCOL_WINDOW_RESIZE: valid = VALIDATE(schema_resize); break;
    case CDIN_PROTOCOL_FOCUS: valid = VALIDATE(schema_focus); break;
    case CDIN_PROTOCOL_VIEW_REQUEST_LINES: valid = VALIDATE(schema_request_lines); break;
    case CDIN_PROTOCOL_VIEW_SET_POSITION: valid = VALIDATE(schema_set_position); break;
    case CDIN_PROTOCOL_REQUEST_COMMAND: valid = VALIDATE(schema_command); break;
    case CDIN_PROTOCOL_PANE_SPLITTER_RESIZE: valid = VALIDATE(schema_pane_splitter_resize); break;
    case CDIN_PROTOCOL_PANE_SPLITTER_RESULT: valid = VALIDATE(schema_pane_splitter_result); break;
    case CDIN_PROTOCOL_IME_COMPOSITION_START: valid = VALIDATE(schema_ime_composition_start); break;
    case CDIN_PROTOCOL_IME_COMPOSITION_UPDATE: valid = VALIDATE(schema_ime_composition_update); break;
    case CDIN_PROTOCOL_IME_COMPOSITION_CANCEL: valid = VALIDATE(schema_ime_composition_cancel); break;
    case CDIN_PROTOCOL_RESPONSE_CLIPBOARD: valid = VALIDATE(schema_response_clipboard); break;
    case CDIN_PROTOCOL_RESPONSE_DIALOG: valid = VALIDATE(schema_response_dialog); break;
    case CDIN_PROTOCOL_ACK: valid = VALIDATE(schema_ack); break;
    case CDIN_PROTOCOL_UI_OPTIONS: valid = VALIDATE(schema_ui_options); break;
    case CDIN_PROTOCOL_UI_STYLE_TABLE: valid = VALIDATE(schema_ui_styles); break;
    case CDIN_PROTOCOL_UI_LAYOUT: valid = VALIDATE(schema_ui_layout); break;
    case CDIN_PROTOCOL_VIEW_LINES: valid = VALIDATE(schema_view_lines); break;
    case CDIN_PROTOCOL_VIEW_INVALIDATE: valid = VALIDATE(schema_view_invalidate); break;
    case CDIN_PROTOCOL_UI_STATUSLINE: valid = VALIDATE(schema_ui_statusline); break;
    case CDIN_PROTOCOL_UI_PROMPT: valid = VALIDATE(schema_ui_prompt); break;
    case CDIN_PROTOCOL_UI_PANEL: valid = VALIDATE(schema_ui_panel); break;
    case CDIN_PROTOCOL_UI_NOTIFY: valid = VALIDATE(schema_ui_notify); break;
    case CDIN_PROTOCOL_REQUEST_CLIPBOARD_GET: valid = VALIDATE(schema_clipboard_get); break;
    case CDIN_PROTOCOL_REQUEST_CLIPBOARD_SET: valid = VALIDATE(schema_clipboard_set); break;
    case CDIN_PROTOCOL_REQUEST_DIALOG: valid = VALIDATE(schema_dialog); break;
    case CDIN_PROTOCOL_ERROR: valid = VALIDATE(schema_error); break;
    case CDIN_PROTOCOL_UI_CURSOR: valid = VALIDATE(schema_cursor); break;
    case CDIN_PROTOCOL_UI_SELECTION: valid = VALIDATE(schema_selection); break;
    case CDIN_PROTOCOL_UI_FLUSH: valid = VALIDATE(schema_ui_flush); break;
    case CDIN_PROTOCOL_DRIVER_ROLE_CHANGED: valid = VALIDATE(schema_driver_role_changed); break;
    case CDIN_PROTOCOL_DETACH_ACK: {
        cbor_cursor cursor = { payload, payload_length, 0u };
        uint8_t major;
        uint64_t pairs;
        valid = cbor_read_head(&cursor, &major, &pairs) && major == CBOR_MAJOR_MAP && pairs == 0u && cursor.offset == payload_length;
        break;
    }
    default: return CDIN_PROTOCOL_UNSUPPORTED;
    }
    if (!valid) return CDIN_PROTOCOL_INVALID_SCHEMA;
    if (type == CDIN_PROTOCOL_HELLO) {
        cbor_item item;
        int present;
        if (!cbor_find_field(payload, payload_length, "capabilities", &item, &present) || !present ||
            !cbor_validate_string_array(payload + item.start, item.end - item.start)) return CDIN_PROTOCOL_INVALID_SCHEMA;
    }
    if (type == CDIN_PROTOCOL_HELLO_ACK) {
        cbor_item item;
        int present;
        if (!cbor_find_field(payload, payload_length, "accepted_capabilities", &item, &present) || !present ||
            !cbor_validate_string_array(payload + item.start, item.end - item.start)) return CDIN_PROTOCOL_INVALID_SCHEMA;
    }
    if (type == CDIN_PROTOCOL_UI_FLUSH) {
        cbor_item scope_item, revision_item;
        cbor_cursor scope_cursor;
        uint8_t scope_major;
        uint64_t scope_length;
        int scope_present, revision_present;
        const uint8_t *scope_bytes;
        if (!cbor_find_field(payload, payload_length, "scope", &scope_item, &scope_present) || !scope_present ||
            scope_item.major != CBOR_MAJOR_TEXT ||
            !cbor_find_field(payload, payload_length, "revision", &revision_item, &revision_present))
            return CDIN_PROTOCOL_INVALID_SCHEMA;
        scope_cursor.data = payload + scope_item.start;
        scope_cursor.length = scope_item.end - scope_item.start;
        scope_cursor.offset = 0u;
        if (!cbor_read_head(&scope_cursor, &scope_major, &scope_length) ||
            scope_major != CBOR_MAJOR_TEXT || scope_length > scope_cursor.length - scope_cursor.offset)
            return CDIN_PROTOCOL_INVALID_SCHEMA;
        scope_bytes = scope_cursor.data + scope_cursor.offset;
        if (scope_length == 8u && memcmp(scope_bytes, "snapshot", 8u) == 0) {
            if (!revision_present || !item_matches(&revision_item, FIELD_UINT))
                return CDIN_PROTOCOL_INVALID_SCHEMA;
        } else if (scope_length == 8u && memcmp(scope_bytes, "response", 8u) == 0) {
            if (revision_present) return CDIN_PROTOCOL_INVALID_SCHEMA;
        } else return CDIN_PROTOCOL_INVALID_SCHEMA;
    }
    if (type == CDIN_PROTOCOL_INPUT_KEY) {
        cbor_item item;
        int present;
        if (!cbor_find_field(payload, payload_length, "modifiers", &item, &present) || !present ||
            !cbor_validate_string_array(payload + item.start, item.end - item.start)) return CDIN_PROTOCOL_INVALID_SCHEMA;
    }
    if (type == CDIN_PROTOCOL_INPUT_MOUSE) {
        cbor_item item;
        int present;
        if (!cbor_find_field(payload, payload_length, "modifiers", &item, &present) || !present ||
            !cbor_validate_string_array(payload + item.start, item.end - item.start)) return CDIN_PROTOCOL_INVALID_SCHEMA;
    }
    if (!cbor_validate_protocol_semantics(type, payload, payload_length)) return CDIN_PROTOCOL_INVALID_SCHEMA;
    return CDIN_PROTOCOL_OK;
}

cdin_protocol_status cdin_protocol_payload_get_uint(const uint8_t *payload, size_t payload_length,
                                                      const char *field, uint64_t *out_value)
{
    cbor_item item;
    int present;
    if (payload == NULL || field == NULL || out_value == NULL) return CDIN_PROTOCOL_INVALID_ARGUMENT;
    if (!cbor_find_field(payload, payload_length, field, &item, &present) || !present || item.major != CBOR_MAJOR_UINT) {
        return CDIN_PROTOCOL_INVALID_SCHEMA;
    }
    {
        cbor_cursor cursor = { payload + item.start, item.end - item.start, 0u };
        uint8_t major;
        if (!cbor_read_head(&cursor, &major, out_value) || major != CBOR_MAJOR_UINT || cursor.offset != cursor.length) {
            return CDIN_PROTOCOL_INVALID_SCHEMA;
        }
    }
    return CDIN_PROTOCOL_OK;
}

cdin_protocol_status cdin_protocol_payload_get_text(const uint8_t *payload, size_t payload_length,
                                                    const char *field, char *output,
                                                    size_t output_capacity, size_t *out_length)
{
    cbor_item item;
    cbor_cursor cursor;
    uint8_t major;
    uint64_t length_u64;
    int present;
    size_t length;
    if (payload == NULL || field == NULL || output == NULL || output_capacity == 0u || out_length == NULL)
        return CDIN_PROTOCOL_INVALID_ARGUMENT;
    *out_length = 0u;
    if (!cbor_find_field(payload, payload_length, field, &item, &present) ||
        !present || item.major != CBOR_MAJOR_TEXT) return CDIN_PROTOCOL_INVALID_SCHEMA;
    cursor.data = payload;
    cursor.length = payload_length;
    cursor.offset = item.start;
    if (!cbor_read_head(&cursor, &major, &length_u64) || major != CBOR_MAJOR_TEXT || length_u64 > SIZE_MAX)
        return CDIN_PROTOCOL_INVALID_SCHEMA;
    length = (size_t)length_u64;
    if (length >= output_capacity) return CDIN_PROTOCOL_BUFFER_TOO_SMALL;
    if (cursor.offset > payload_length || length > payload_length - cursor.offset || cursor.offset + length != item.end)
        return CDIN_PROTOCOL_INVALID_SCHEMA;
    if (memchr(payload + cursor.offset, '\0', length) != NULL) return CDIN_PROTOCOL_INVALID_SCHEMA;
    memcpy(output, payload + cursor.offset, length);
    output[length] = '\0';
    *out_length = length;
    return CDIN_PROTOCOL_OK;
}

cdin_protocol_status cdin_protocol_payload_get_bool(const uint8_t *payload, size_t payload_length,
                                                    const char *field, int *out_value)
{
    cbor_item item;
    int present;
    if (payload == NULL || field == NULL || out_value == NULL) return CDIN_PROTOCOL_INVALID_ARGUMENT;
    if (!cbor_find_field(payload, payload_length, field, &item, &present) || !present ||
        item.major != CBOR_MAJOR_SIMPLE || (item.value != CBOR_FALSE && item.value != CBOR_TRUE))
        return CDIN_PROTOCOL_INVALID_SCHEMA;
    *out_value = item.value == CBOR_TRUE;
    return CDIN_PROTOCOL_OK;
}

cdin_protocol_status cdin_protocol_payload_get_raw(const uint8_t *payload, size_t payload_length,
                                                    const char *field, const uint8_t **out_value,
                                                    size_t *out_value_length)
{
    cbor_item item;
    int present;
    if (payload == NULL || field == NULL || out_value == NULL || out_value_length == NULL)
        return CDIN_PROTOCOL_INVALID_ARGUMENT;
    *out_value = NULL;
    *out_value_length = 0u;
    if (!cbor_find_field(payload, payload_length, field, &item, &present) || !present ||
        item.end < item.start || item.end > payload_length) return CDIN_PROTOCOL_INVALID_SCHEMA;
    *out_value = payload + item.start;
    *out_value_length = item.end - item.start;
    return CDIN_PROTOCOL_OK;
}

const char *cdin_protocol_message_name(cdin_protocol_message_type type)
{
    switch (type) {
    case CDIN_PROTOCOL_UI_OPTIONS: return "ui.options";
    case CDIN_PROTOCOL_UI_STYLE_TABLE: return "ui.style_table";
    case CDIN_PROTOCOL_UI_LAYOUT: return "ui.layout";
    case CDIN_PROTOCOL_VIEW_LINES: return "view.lines";
    case CDIN_PROTOCOL_VIEW_INVALIDATE: return "view.invalidate";
    case CDIN_PROTOCOL_UI_STATUSLINE: return "ui.statusline";
    case CDIN_PROTOCOL_UI_PROMPT: return "ui.prompt";
    case CDIN_PROTOCOL_UI_PANEL: return "ui.panel";
    case CDIN_PROTOCOL_UI_NOTIFY: return "ui.notify";
    case CDIN_PROTOCOL_REQUEST_CLIPBOARD_GET: return "request.clipboard_get";
    case CDIN_PROTOCOL_REQUEST_CLIPBOARD_SET: return "request.clipboard_set";
    case CDIN_PROTOCOL_REQUEST_DIALOG: return "request.dialog";
    case CDIN_PROTOCOL_ERROR: return "error";
    case CDIN_PROTOCOL_UI_CURSOR: return "view.cursor";
    case CDIN_PROTOCOL_UI_SELECTION: return "view.selection";
    case CDIN_PROTOCOL_UI_FLUSH: return "ui.flush";
    case CDIN_PROTOCOL_HELLO_ACK: return "hello_ack";
    case CDIN_PROTOCOL_DETACH_ACK: return "detach_ack";
    case CDIN_PROTOCOL_DRIVER_ROLE_CHANGED: return "driver.role_changed";
    case CDIN_PROTOCOL_PANE_SPLITTER_RESULT: return "pane.splitter.result";
    case CDIN_PROTOCOL_IME_COMPOSITION_START: return "ime.composition.start";
    case CDIN_PROTOCOL_IME_COMPOSITION_UPDATE: return "ime.composition.update";
    case CDIN_PROTOCOL_IME_COMPOSITION_CANCEL: return "ime.composition.cancel";
    case CDIN_PROTOCOL_PANE_SPLITTER_RESIZE: return "pane.splitter.resize";
    case CDIN_PROTOCOL_HELLO: return "hello";
    case CDIN_PROTOCOL_ATTACH: return "attach";
    case CDIN_PROTOCOL_DETACH: return "detach";
    case CDIN_PROTOCOL_INPUT_KEY: return "input.key";
    case CDIN_PROTOCOL_INPUT_TEXT: return "input.text";
    case CDIN_PROTOCOL_INPUT_MOUSE: return "input.mouse";
    case CDIN_PROTOCOL_INPUT_PASTE: return "input.paste";
    case CDIN_PROTOCOL_WINDOW_RESIZE: return "window.resize";
    case CDIN_PROTOCOL_FOCUS: return "focus";
    case CDIN_PROTOCOL_VIEW_REQUEST_LINES: return "view.request_lines";
    case CDIN_PROTOCOL_VIEW_SET_POSITION: return "view.set_position";
    case CDIN_PROTOCOL_REQUEST_COMMAND: return "request.command";
    case CDIN_PROTOCOL_RESPONSE_CLIPBOARD: return "response.clipboard";
    case CDIN_PROTOCOL_RESPONSE_DIALOG: return "response.dialog";
    case CDIN_PROTOCOL_ACK: return "ack";
    default: return NULL;
    }
}

int cdin_protocol_message_is_inbound(cdin_protocol_message_type type)
{
    switch (type) {
    case CDIN_PROTOCOL_HELLO:
    case CDIN_PROTOCOL_ATTACH:
    case CDIN_PROTOCOL_DETACH:
    case CDIN_PROTOCOL_INPUT_KEY:
    case CDIN_PROTOCOL_INPUT_TEXT:
    case CDIN_PROTOCOL_INPUT_MOUSE:
    case CDIN_PROTOCOL_INPUT_PASTE:
    case CDIN_PROTOCOL_WINDOW_RESIZE:
    case CDIN_PROTOCOL_FOCUS:
    case CDIN_PROTOCOL_VIEW_REQUEST_LINES:
    case CDIN_PROTOCOL_VIEW_SET_POSITION:
    case CDIN_PROTOCOL_REQUEST_COMMAND:
    case CDIN_PROTOCOL_RESPONSE_CLIPBOARD:
    case CDIN_PROTOCOL_RESPONSE_DIALOG:
    case CDIN_PROTOCOL_ACK:
    case CDIN_PROTOCOL_PANE_SPLITTER_RESIZE:
    case CDIN_PROTOCOL_IME_COMPOSITION_START:
    case CDIN_PROTOCOL_IME_COMPOSITION_UPDATE:
    case CDIN_PROTOCOL_IME_COMPOSITION_CANCEL:
        return 1;
    default:
        return 0;
    }
}

int cdin_protocol_message_requires_driver(cdin_protocol_message_type type)
{
    switch (type) {
    case CDIN_PROTOCOL_INPUT_KEY:
    case CDIN_PROTOCOL_INPUT_TEXT:
    case CDIN_PROTOCOL_INPUT_MOUSE:
    case CDIN_PROTOCOL_INPUT_PASTE:
    case CDIN_PROTOCOL_WINDOW_RESIZE:
    case CDIN_PROTOCOL_FOCUS:
    case CDIN_PROTOCOL_VIEW_SET_POSITION:
    case CDIN_PROTOCOL_REQUEST_COMMAND:
    case CDIN_PROTOCOL_PANE_SPLITTER_RESIZE:
    case CDIN_PROTOCOL_IME_COMPOSITION_START:
    case CDIN_PROTOCOL_IME_COMPOSITION_UPDATE:
    case CDIN_PROTOCOL_IME_COMPOSITION_CANCEL:
        return 1;
    default:
        return 0;
    }
}

const char *cdin_protocol_status_name(cdin_protocol_status status)
{
    switch (status) {
    case CDIN_PROTOCOL_OK: return "OK";
    case CDIN_PROTOCOL_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case CDIN_PROTOCOL_INVALID_FRAME: return "INVALID_FRAME";
    case CDIN_PROTOCOL_INVALID_SCHEMA: return "INVALID_SCHEMA";
    case CDIN_PROTOCOL_VERSION_MISMATCH: return "VERSION_MISMATCH";
    case CDIN_PROTOCOL_UNSUPPORTED: return "UNSUPPORTED";
    case CDIN_PROTOCOL_LIMIT_EXCEEDED: return "LIMIT_EXCEEDED";
    case CDIN_PROTOCOL_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case CDIN_PROTOCOL_IO_ERROR: return "IO_ERROR";
    case CDIN_PROTOCOL_EOF: return "EOF";
    default: return "UNKNOWN";
    }
}

static size_t cbor_uint_size(uint64_t value)
{
    if (value < 24u) return 1u;
    if (value <= UINT8_MAX) return 2u;
    if (value <= UINT16_MAX) return 3u;
    if (value <= UINT32_MAX) return 5u;
    return 9u;
}

static size_t cbor_write_head(uint8_t *output, uint8_t major, uint64_t value)
{
    size_t size;
    size_t i;
    if (value < 24u) {
        output[0] = (uint8_t)((major << 5) | (uint8_t)value);
        return 1u;
    }
    if (value <= UINT8_MAX) { output[0] = (uint8_t)(((uint32_t)major << 5u) | 24u); size = 1u; }
    else if (value <= UINT16_MAX) { output[0] = (uint8_t)(((uint32_t)major << 5u) | 25u); size = 2u; }
    else if (value <= UINT32_MAX) { output[0] = (uint8_t)(((uint32_t)major << 5u) | 26u); size = 4u; }
    else { output[0] = (uint8_t)(((uint32_t)major << 5u) | 27u); size = 8u; }
    for (i = 0u; i < size; i++) {
        output[1u + i] = (uint8_t)(value >> ((size - 1u - i) * 8u));
    }
    return size + 1u;
}

static size_t cbor_write_text(uint8_t *output, const char *text)
{
    size_t length = strlen(text);
    size_t head = cbor_write_head(output, CBOR_MAJOR_TEXT, (uint64_t)length);
    memcpy(output + head, text, length);
    return head + length;
}

static int cbor_read_text_equals(const uint8_t *data, size_t length,
                                 const char *expected, size_t *out_after)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t text_length;
    size_t expected_length = strlen(expected);
    if (!cbor_read_head(&cursor, &major, &text_length) || major != CBOR_MAJOR_TEXT || text_length != expected_length ||
        text_length > (uint64_t)(cursor.length - cursor.offset)) return 0;
    if (memcmp(cursor.data + cursor.offset, expected, expected_length) != 0) return 0;
    cursor.offset += expected_length;
    if (out_after != NULL) *out_after = cursor.offset;
    return cursor.offset == cursor.length;
}

static int cbor_read_uint_item(const uint8_t *data, size_t length, uint64_t *out_value)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t value;
    if (!cbor_read_head(&cursor, &major, &value) || major != CBOR_MAJOR_UINT || cursor.offset != length) return 0;
    *out_value = value;
    return 1;
}

static int cbor_read_version(const uint8_t *data, size_t length, uint32_t *major_out, uint32_t *minor_out)
{
    cbor_cursor cursor = { data, length, 0u };
    uint8_t major;
    uint64_t count;
    uint64_t major_version;
    uint64_t minor_version;
    if (!cbor_read_head(&cursor, &major, &count) || major != CBOR_MAJOR_ARRAY || count != 2u) return 0;
    if (!cbor_read_head(&cursor, &major, &major_version) || major != CBOR_MAJOR_UINT || major_version > UINT32_MAX) return 0;
    if (!cbor_read_head(&cursor, &major, &minor_version) || major != CBOR_MAJOR_UINT || minor_version > UINT32_MAX) return 0;
    if (cursor.offset != cursor.length) return 0;
    *major_out = (uint32_t)major_version;
    *minor_out = (uint32_t)minor_version;
    return 1;
}

cdin_protocol_status cdin_protocol_encode_frame(const cdin_protocol_message_view *message,
                                                 uint8_t *output,
                                                 size_t output_capacity,
                                                 size_t *out_length,
                                                 size_t max_frame_bytes)
{
    const char *type_name;
    const char *name_space;
    size_t namespace_length;
    size_t body_size;
    size_t total_size;
    size_t offset;
    uint8_t *body;
    if (message == NULL || output == NULL || out_length == NULL || message->payload == NULL ||
        message->name_space == NULL) return CDIN_PROTOCOL_INVALID_ARGUMENT;
    *out_length = 0u;
    type_name = cdin_protocol_message_name(message->type);
    name_space = (message->type == CDIN_PROTOCOL_PANE_SPLITTER_RESIZE ||
                  message->type == CDIN_PROTOCOL_PANE_SPLITTER_RESULT) ? "pane" :
                 (message->type >= CDIN_PROTOCOL_IME_COMPOSITION_START &&
                  message->type <= CDIN_PROTOCOL_IME_COMPOSITION_CANCEL) ? "ime" : "core";
    namespace_length = strlen(name_space);
    if (type_name == NULL || strcmp(message->name_space, name_space) != 0) return CDIN_PROTOCOL_UNSUPPORTED;
    if (message->major != CDIN_PROTOCOL_VERSION_MAJOR || message->minor != CDIN_PROTOCOL_VERSION_MINOR) {
        return CDIN_PROTOCOL_VERSION_MISMATCH;
    }
    if (cdin_protocol_validate_payload(message->type, message->payload, message->payload_length) != CDIN_PROTOCOL_OK) {
        return CDIN_PROTOCOL_INVALID_SCHEMA;
    }
    body_size = 1u; /* Fixed five-field envelope map. */
    body_size += 1u + cbor_uint_size(message->major) + cbor_uint_size(message->minor); /* version array */
    body_size += 1u + 1u; /* key "v" */
    body_size += 1u + 2u + 1u + namespace_length; /* key "ns" and namespace text */
    body_size += 1u + 3u + cbor_uint_size(message->sequence); /* key "seq" and value */
    body_size += 1u + 4u + 1u + strlen(type_name); /* key "type" and text value */
    body_size += 1u + 7u + message->payload_length; /* key "payload" and encoded map */
    if (body_size > max_frame_bytes || body_size > UINT32_MAX) return CDIN_PROTOCOL_LIMIT_EXCEEDED;
    if (body_size > SIZE_MAX - CDIN_PROTOCOL_FRAME_PREFIX_BYTES) return CDIN_PROTOCOL_LIMIT_EXCEEDED;
    total_size = body_size + CDIN_PROTOCOL_FRAME_PREFIX_BYTES;
    if (total_size > output_capacity) return CDIN_PROTOCOL_BUFFER_TOO_SMALL;

    body = output + CDIN_PROTOCOL_FRAME_PREFIX_BYTES;
    offset = 0u;
    body[offset++] = 0xa5u; /* Fixed five-field envelope map. */
    offset += cbor_write_text(body + offset, "v");
    body[offset++] = 0x82u;
    offset += cbor_write_head(body + offset, CBOR_MAJOR_UINT, message->major);
    offset += cbor_write_head(body + offset, CBOR_MAJOR_UINT, message->minor);
    offset += cbor_write_text(body + offset, "ns");
    offset += cbor_write_text(body + offset, name_space);
    offset += cbor_write_text(body + offset, "seq");
    offset += cbor_write_head(body + offset, CBOR_MAJOR_UINT, message->sequence);
    offset += cbor_write_text(body + offset, "type");
    offset += cbor_write_text(body + offset, type_name);
    offset += cbor_write_text(body + offset, "payload");
    memcpy(body + offset, message->payload, message->payload_length);
    offset += message->payload_length;
    if (offset != body_size) return CDIN_PROTOCOL_INVALID_FRAME;
    output[0] = (uint8_t)(body_size >> 24);
    output[1] = (uint8_t)(body_size >> 16);
    output[2] = (uint8_t)(body_size >> 8);
    output[3] = (uint8_t)body_size;
    *out_length = total_size;
    return CDIN_PROTOCOL_OK;
}

static int type_from_name(const uint8_t *data, size_t length, cdin_protocol_message_type *out_type)
{
    cdin_protocol_message_type type;
    for (type = CDIN_PROTOCOL_UI_OPTIONS; type <= CDIN_PROTOCOL_IME_COMPOSITION_CANCEL; type = (cdin_protocol_message_type)((int)type + 1)) {
        const char *name = cdin_protocol_message_name(type);
        if (name != NULL && cbor_read_text_equals(data, length, name, NULL)) {
            *out_type = type;
            return 1;
        }
    }
    return 0;
}

cdin_protocol_status cdin_protocol_decode_frame(const uint8_t *frame,
                                                 size_t frame_length,
                                                 cdin_protocol_message_view *out_message,
                                                 size_t max_frame_bytes)
{
    uint32_t declared;
    cbor_cursor cursor;
    uint8_t major;
    uint64_t pairs;
    uint64_t version_length;
    uint32_t version_major = 0u;
    uint32_t version_minor = 0u;
    uint64_t sequence = 0u;
    cbor_item version_item = {0};
    cbor_item namespace_item = {0};
    cbor_item sequence_item = {0};
    cbor_item type_item = {0};
    cbor_item payload_item = {0};
    int have_version = 0, have_namespace = 0, have_sequence = 0, have_type = 0, have_payload = 0;
    uint64_t index;
    const uint8_t *last_key = NULL;
    size_t last_key_length = 0u;
    const uint8_t *body;
    size_t body_length;
    cdin_protocol_message_type type;

    if (frame == NULL || out_message == NULL || frame_length < CDIN_PROTOCOL_FRAME_PREFIX_BYTES) return CDIN_PROTOCOL_INVALID_ARGUMENT;
    memset(out_message, 0, sizeof(*out_message));
    declared = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16) |
               ((uint32_t)frame[2] << 8) | (uint32_t)frame[3];
    if (declared == 0u || (size_t)declared > max_frame_bytes) return CDIN_PROTOCOL_LIMIT_EXCEEDED;
    if ((size_t)declared != frame_length - CDIN_PROTOCOL_FRAME_PREFIX_BYTES) return CDIN_PROTOCOL_INVALID_FRAME;
    body = frame + CDIN_PROTOCOL_FRAME_PREFIX_BYTES;
    body_length = (size_t)declared;
    cursor.data = body;
    cursor.length = body_length;
    cursor.offset = 0u;
    if (!cbor_read_head(&cursor, &major, &pairs) || major != CBOR_MAJOR_MAP || pairs != 5u) return CDIN_PROTOCOL_INVALID_FRAME;
    for (index = 0u; index < pairs; index++) {
        cbor_item key_item;
        cbor_item *destination = NULL;
        int *present = NULL;
        const uint8_t *key_data;
        size_t key_length;
        cbor_cursor key_cursor;
        uint8_t key_major;
        uint64_t key_size;
        if (!cbor_skip(&cursor, 1u, &key_item) || key_item.major != CBOR_MAJOR_TEXT) return CDIN_PROTOCOL_INVALID_FRAME;
        key_cursor.data = body;
        key_cursor.length = body_length;
        key_cursor.offset = key_item.start;
        if (!cbor_read_head(&key_cursor, &key_major, &key_size) || key_major != CBOR_MAJOR_TEXT || key_size > SIZE_MAX) return CDIN_PROTOCOL_INVALID_FRAME;
        key_data = body + key_cursor.offset;
        key_length = (size_t)key_size;
        {
            size_t encoded_length = key_item.end - key_item.start;
            const uint8_t *encoded_key = body + key_item.start;
            if (last_key != NULL && (encoded_length < last_key_length ||
                (encoded_length == last_key_length && memcmp(last_key, encoded_key, encoded_length) >= 0)))
                return CDIN_PROTOCOL_INVALID_FRAME;
            last_key = encoded_key;
            last_key_length = encoded_length;
        }
        if (key_length == 1u && memcmp(key_data, "v", 1u) == 0) { destination = &version_item; present = &have_version; }
        else if (key_length == 2u && memcmp(key_data, "ns", 2u) == 0) { destination = &namespace_item; present = &have_namespace; }
        else if (key_length == 3u && memcmp(key_data, "seq", 3u) == 0) { destination = &sequence_item; present = &have_sequence; }
        else if (key_length == 4u && memcmp(key_data, "type", 4u) == 0) { destination = &type_item; present = &have_type; }
        else if (key_length == 7u && memcmp(key_data, "payload", 7u) == 0) { destination = &payload_item; present = &have_payload; }
        else return CDIN_PROTOCOL_INVALID_FRAME;
        if (*present) return CDIN_PROTOCOL_INVALID_FRAME;
        if (!cbor_skip(&cursor, 1u, destination)) return CDIN_PROTOCOL_INVALID_FRAME;
        *present = 1;
    }
    if (cursor.offset != body_length || !have_version || !have_namespace || !have_sequence || !have_type || !have_payload) {
        return CDIN_PROTOCOL_INVALID_FRAME;
    }
    if (version_item.end > body_length ||
        !cbor_read_version(body + version_item.start, version_item.end - version_item.start, &version_major, &version_minor)) {
        return CDIN_PROTOCOL_INVALID_FRAME;
    }
    if (version_major != CDIN_PROTOCOL_VERSION_MAJOR || version_minor != CDIN_PROTOCOL_VERSION_MINOR) {
        return CDIN_PROTOCOL_VERSION_MISMATCH;
    }
    if (!cbor_read_uint_item(body + sequence_item.start, sequence_item.end - sequence_item.start, &sequence)) {
        return CDIN_PROTOCOL_INVALID_FRAME;
    }
    if (!type_from_name(body + type_item.start, type_item.end - type_item.start, &type)) {
        return CDIN_PROTOCOL_UNSUPPORTED;
    }
    {
        const char *expected_namespace =
            (type == CDIN_PROTOCOL_PANE_SPLITTER_RESIZE || type == CDIN_PROTOCOL_PANE_SPLITTER_RESULT)
                ? "pane" :
            (type >= CDIN_PROTOCOL_IME_COMPOSITION_START && type <= CDIN_PROTOCOL_IME_COMPOSITION_CANCEL)
                ? "ime" : "core";
        if (!cbor_read_text_equals(body + namespace_item.start,
                                   namespace_item.end - namespace_item.start,
                                   expected_namespace, NULL)) return CDIN_PROTOCOL_UNSUPPORTED;
    }
    /* The version array is already bounded; this value keeps compiler diagnostics local. */
    version_length = (uint64_t)(version_item.end - version_item.start);
    (void)version_length;
    if (payload_item.major != CBOR_MAJOR_MAP) return CDIN_PROTOCOL_INVALID_SCHEMA;
    {
        const uint8_t *payload = body + payload_item.start;
        size_t payload_length = payload_item.end - payload_item.start;
        cdin_protocol_status schema_status = cdin_protocol_validate_payload(type, payload, payload_length);
        if (schema_status != CDIN_PROTOCOL_OK) return schema_status;
        out_message->major = version_major;
        out_message->minor = version_minor;
        out_message->sequence = sequence;
        out_message->type = type;
        out_message->name_space =
            (type == CDIN_PROTOCOL_PANE_SPLITTER_RESIZE || type == CDIN_PROTOCOL_PANE_SPLITTER_RESULT)
                ? "pane" :
            (type >= CDIN_PROTOCOL_IME_COMPOSITION_START && type <= CDIN_PROTOCOL_IME_COMPOSITION_CANCEL)
                ? "ime" : "core";
        out_message->payload = payload;
        out_message->payload_length = payload_length;
    }
    return CDIN_PROTOCOL_OK;
}

cdin_protocol_status cdin_protocol_read_frame(const cdin_protocol_io *io,
                                               uint8_t *buffer,
                                               size_t capacity,
                                               size_t *out_length,
                                               size_t max_frame_bytes)
{
    size_t got = 0u;
    size_t total;
    size_t body_length;
    if (io == NULL || io->read == NULL || buffer == NULL || out_length == NULL || capacity < 4u) return CDIN_PROTOCOL_INVALID_ARGUMENT;
    *out_length = 0u;
    while (got < 4u) {
        ptrdiff_t count = io->read(io->context, buffer + got, 4u - got);
        if (count < 0) return CDIN_PROTOCOL_IO_ERROR;
        if (count == 0) return got == 0u ? CDIN_PROTOCOL_EOF : CDIN_PROTOCOL_INVALID_FRAME;
        if ((size_t)count > 4u - got) return CDIN_PROTOCOL_IO_ERROR;
        got += (size_t)count;
    }
    body_length = ((size_t)buffer[0] << 24) | ((size_t)buffer[1] << 16) |
                  ((size_t)buffer[2] << 8) | (size_t)buffer[3];
    if (body_length == 0u || body_length > max_frame_bytes) return CDIN_PROTOCOL_LIMIT_EXCEEDED;
    if (body_length > capacity - 4u) return CDIN_PROTOCOL_BUFFER_TOO_SMALL;
    total = 4u + body_length;
    while (got < total) {
        ptrdiff_t count = io->read(io->context, buffer + got, total - got);
        if (count < 0) return CDIN_PROTOCOL_IO_ERROR;
        if (count == 0) return CDIN_PROTOCOL_INVALID_FRAME;
        if ((size_t)count > total - got) return CDIN_PROTOCOL_IO_ERROR;
        got += (size_t)count;
    }
    *out_length = total;
    return CDIN_PROTOCOL_OK;
}

cdin_protocol_status cdin_protocol_write_frame(const cdin_protocol_io *io,
                                                const uint8_t *frame,
                                                size_t frame_length)
{
    size_t sent = 0u;
    if (io == NULL || io->write == NULL || frame == NULL || frame_length < 4u) return CDIN_PROTOCOL_INVALID_ARGUMENT;
    while (sent < frame_length) {
        ptrdiff_t count = io->write(io->context, frame + sent, frame_length - sent);
        if (count < 0) return CDIN_PROTOCOL_IO_ERROR;
        if (count == 0 || (size_t)count > frame_length - sent) return CDIN_PROTOCOL_IO_ERROR;
        sent += (size_t)count;
    }
    return CDIN_PROTOCOL_OK;
}
