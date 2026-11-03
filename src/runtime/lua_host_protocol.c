/*
 * Runtime/Frontend Protocol bridge.
 *
 * Runtime values are copied into deterministic CBOR on the Lua owner thread.
 * This module never retains Lua table references as queued message storage;
 * the protocol server takes an owned byte copy for each atomic UI batch.
 */
#ifndef CDIN_WITH_LUA
typedef int lua_host_protocol_disabled_translation_unit;
#else
#include "lua_host_internal.h"
#include "../protocol/server.h"
#include <cdin/protocol.h>

#define UI_MODEL_SCHEMA "cdin.ui-model.v1"
#define UI_MAX_MODEL_MAP_FIELDS 64u
#define UI_MAX_MODEL_VIEWS 32u
#define UI_MAX_MODEL_PANELS 16u
#define UI_MAX_PROTOCOL_DEPTH CDIN_PROTOCOL_MAX_DEPTH

typedef struct cbor_buffer {
    uint8_t *data;
    size_t length;
    size_t capacity;
    size_t limit;
} cbor_buffer;

typedef struct cbor_map_entry {
    const char *key;
    size_t key_length;
    const uint8_t *value;
    size_t value_length;
} cbor_map_entry;

typedef struct lua_map_key {
    const char *value;
    size_t length;
    int is_ratio;
} lua_map_key;

typedef struct ui_message {
    cdin_protocol_message_type type;
    cbor_buffer payload;
} ui_message;

static int buffer_reserve(cbor_buffer *buffer, size_t additional)
{
    size_t required;
    size_t next;
    uint8_t *grown;
    if (buffer == NULL || additional > SIZE_MAX - buffer->length) return 0;
    required = buffer->length + additional;
    if (required > buffer->limit) return 0;
    if (required <= buffer->capacity) return 1;
    next = buffer->capacity == 0u ? 128u : buffer->capacity;
    while (next < required) {
        if (next > buffer->limit / 2u) { next = buffer->limit; break; }
        next *= 2u;
    }
    if (next < required || next == 0u) return 0;
    grown = (uint8_t *)realloc(buffer->data, next);
    if (grown == NULL) return 0;
    buffer->data = grown;
    buffer->capacity = next;
    return 1;
}

static int buffer_append(cbor_buffer *buffer, const void *bytes, size_t length)
{
    if (length == 0u) return 1;
    if (bytes == NULL || !buffer_reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length += length;
    return 1;
}

static void buffer_free(cbor_buffer *buffer)
{
    if (buffer == NULL) return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int cbor_write_head(cbor_buffer *buffer, uint8_t major, uint64_t value)
{
    uint8_t bytes[9];
    size_t count = 1u;
    size_t width = 0u;
    size_t index;
    if (value < 24u) bytes[0] = (uint8_t)((major << 5) | (uint8_t)value);
    else {
        if (value <= UINT8_MAX) width = 1u;
        else if (value <= UINT16_MAX) width = 2u;
        else if (value <= UINT32_MAX) width = 4u;
        else width = 8u;
        bytes[0] = (uint8_t)((major << 5) | (width == 1u ? 24u : width == 2u ? 25u : width == 4u ? 26u : 27u));
        for (index = 0u; index < width; index++)
            bytes[index + 1u] = (uint8_t)(value >> ((width - index - 1u) * 8u));
        count += width;
    }
    return buffer_append(buffer, bytes, count);
}

static int cbor_write_uint(cbor_buffer *buffer, uint64_t value)
{ return cbor_write_head(buffer, 0u, value); }

static int cbor_write_text(cbor_buffer *buffer, const char *text, size_t length)
{
    return cbor_write_head(buffer, 3u, (uint64_t)length) && buffer_append(buffer, text, length);
}

static int cbor_write_bytes(cbor_buffer *buffer, const uint8_t *bytes, size_t length)
{
    return cbor_write_head(buffer, 2u, (uint64_t)length) && buffer_append(buffer, bytes, length);
}

static int cbor_write_null(cbor_buffer *buffer)
{
    static const uint8_t null_byte = 0xf6u;
    return buffer_append(buffer, &null_byte, 1u);
}

static int cbor_write_bool(cbor_buffer *buffer, int value)
{
    const uint8_t byte = value ? 0xf5u : 0xf4u;
    return buffer_append(buffer, &byte, 1u);
}

static int cbor_write_signed(cbor_buffer *buffer, lua_Integer value)
{
    if (value >= 0) return cbor_write_head(buffer, 0u, (uint64_t)value);
    return cbor_write_head(buffer, 1u, (uint64_t)(-(value + 1)));
}

static int map_entry_compare(const void *left, const void *right)
{
    const cbor_map_entry *a = (const cbor_map_entry *)left;
    const cbor_map_entry *b = (const cbor_map_entry *)right;
    if (a->key_length != b->key_length) return a->key_length < b->key_length ? -1 : 1;
    return memcmp(a->key, b->key, a->key_length);
}

static int cbor_write_map_entries(cbor_buffer *output, cbor_map_entry *entries, size_t count)
{
    size_t index;
    if (count > UI_MAX_MODEL_MAP_FIELDS || !cbor_write_head(output, 5u, (uint64_t)count)) return 0;
    qsort(entries, count, sizeof(*entries), map_entry_compare);
    for (index = 0u; index < count; index++) {
        if (!cbor_write_text(output, entries[index].key, entries[index].key_length) ||
            !buffer_append(output, entries[index].value, entries[index].value_length)) return 0;
    }
    return 1;
}

static int lua_key_compare(const void *left, const void *right)
{
    const lua_map_key *a = (const lua_map_key *)left;
    const lua_map_key *b = (const lua_map_key *)right;
    const char *a_value = a->is_ratio ? "ratio_bp" : a->value;
    const char *b_value = b->is_ratio ? "ratio_bp" : b->value;
    size_t a_length = a->is_ratio ? 8u : a->length;
    size_t b_length = b->is_ratio ? 8u : b->length;
    size_t common = a_length < b_length ? a_length : b_length;
    int order;
    if (a_length != b_length) return a_length < b_length ? -1 : 1;
    order = memcmp(a_value, b_value, common);
    return order;
}

static int lua_table_has_metatable(lua_State *L, int index)
{
    int has = lua_getmetatable(L, index);
    if (has) lua_pop(L, 1);
    return has;
}

static int encode_lua_value(lua_State *L, int index, cbor_buffer *output,
                            const void **ancestors, unsigned depth, const char *field_name);

static int encode_lua_table(lua_State *L, int index, cbor_buffer *output,
                            const void **ancestors, unsigned depth)
{
    lua_map_key keys[UI_MAX_MODEL_MAP_FIELDS];
    size_t key_count = 0u;
    size_t array_count = 0u;
    size_t iteration_count = 0u;
    size_t position;
    int is_array = 1;
    int absolute = lua_absindex(L, index);
    const void *identity = lua_topointer(L, absolute);
    unsigned ancestor;

    if (depth >= UI_MAX_PROTOCOL_DEPTH || lua_table_has_metatable(L, absolute)) return 0;
    for (ancestor = 0u; ancestor < depth; ancestor++) if (ancestors[ancestor] == identity) return 0;
    ancestors[depth] = identity;

    array_count = lua_rawlen(L, absolute);
    lua_pushnil(L);
    while (lua_next(L, absolute) != 0) {
        iteration_count++;
        if (lua_isinteger(L, -2)) {
            lua_Integer key = lua_tointeger(L, -2);
            if (key < 1 || (uint64_t)key > (uint64_t)array_count) is_array = 0;
        } else is_array = 0;
        lua_pop(L, 1);
        if (iteration_count > UI_MAX_MODEL_MAP_FIELDS) { lua_pop(L, 1); return 0; }
    }

    /* Lua's empty table is ambiguous; in the UI model every empty table is
     * a map (empty status/style/panel properties), never an empty sequence. */
    if (array_count != 0u && is_array && iteration_count == array_count) {
        if (!cbor_write_head(output, 4u, (uint64_t)array_count)) return 0;
        for (position = 1u; position <= array_count; position++) {
            lua_rawgeti(L, absolute, (lua_Integer)position);
            if (!encode_lua_value(L, -1, output, ancestors, depth + 1u, NULL)) {
                lua_pop(L, 1);
                return 0;
            }
            lua_pop(L, 1);
        }
        return 1;
    }

    if (iteration_count > UI_MAX_MODEL_MAP_FIELDS) return 0;
    lua_pushnil(L);
    while (lua_next(L, absolute) != 0) {
        size_t key_length = 0u;
        const char *key;
        if (lua_type(L, -2) != LUA_TSTRING || key_count >= UI_MAX_MODEL_MAP_FIELDS) {
            lua_pop(L, 2);
            return 0;
        }
        key = lua_tolstring(L, -2, &key_length);
        if (key_length == 0u || memchr(key, '\0', key_length) != NULL) {
            lua_pop(L, 2);
            return 0;
        }
        keys[key_count].value = key;
        keys[key_count].length = key_length;
        keys[key_count].is_ratio = key_length == 5u && memcmp(key, "ratio", 5u) == 0;
        key_count++;
        lua_pop(L, 1);
    }
    qsort(keys, key_count, sizeof(keys[0]), lua_key_compare);
    for (position = 1u; position < key_count; position++) {
        if (lua_key_compare(&keys[position - 1u], &keys[position]) == 0) return 0;
    }
    if (!cbor_write_head(output, 5u, (uint64_t)key_count)) return 0;
    for (position = 0u; position < key_count; position++) {
        const lua_map_key *key = &keys[position];
        const char *encoded_key = key->is_ratio ? "ratio_bp" : key->value;
        size_t encoded_length = key->is_ratio ? 8u : key->length;
        lua_pushlstring(L, key->value, key->length);
        lua_rawget(L, absolute);
        if (!cbor_write_text(output, encoded_key, encoded_length)) {
            lua_pop(L, 1);
            return 0;
        }
        if (key->is_ratio) {
            lua_Number ratio;
            uint64_t ratio_bp;
            if (!lua_isnumber(L, -1)) { lua_pop(L, 1); return 0; }
            ratio = lua_tonumber(L, -1);
            if (!isfinite((double)ratio) || ratio <= 0.1 || ratio >= 0.9) {
                lua_pop(L, 1);
                return 0;
            }
            ratio_bp = (uint64_t)floor((double)ratio * 10000.0 + 0.5);
            if (ratio_bp < 1000u || ratio_bp > 8999u || !cbor_write_uint(output, ratio_bp)) {
                lua_pop(L, 1);
                return 0;
            }
        } else if (!encode_lua_value(L, -1, output, ancestors, depth + 1u, key->value)) {
            lua_pop(L, 1);
            return 0;
        }
        lua_pop(L, 1);
    }
    return 1;
}

static int encode_lua_value(lua_State *L, int index, cbor_buffer *output,
                            const void **ancestors, unsigned depth, const char *field_name)
{
    int type = lua_type(L, index);
    if (depth > UI_MAX_PROTOCOL_DEPTH) return 0;
    switch (type) {
    case LUA_TNIL: return cbor_write_null(output);
    case LUA_TBOOLEAN: return cbor_write_bool(output, lua_toboolean(L, index));
    case LUA_TNUMBER:
        if (lua_isinteger(L, index)) return cbor_write_signed(output, lua_tointeger(L, index));
        if (field_name != NULL && strcmp(field_name, "ratio") == 0) {
            lua_Number ratio = lua_tonumber(L, index);
            uint64_t basis_points;
            if (!isfinite((double)ratio) || ratio <= 0.1 || ratio >= 0.9) return 0;
            basis_points = (uint64_t)floor((double)ratio * 10000.0 + 0.5);
            return basis_points >= 1000u && basis_points <= 8999u && cbor_write_uint(output, basis_points);
        }
        return 0; /* Deterministic CBOR deliberately excludes floating-point values. */
    case LUA_TSTRING: {
        size_t length = 0u;
        const char *value = lua_tolstring(L, index, &length);
        return cbor_write_text(output, value, length);
    }
    case LUA_TTABLE: return encode_lua_table(L, index, output, ancestors, depth);
    default: return 0; /* Functions, userdata, threads and lightuserdata are not serializable. */
    }
}

static int encode_lua_item(lua_State *L, int index, cbor_buffer *output)
{
    const void *ancestors[UI_MAX_PROTOCOL_DEPTH + 1u] = { 0 };
    return encode_lua_value(L, index, output, ancestors, 0u, NULL);
}

static int make_map_payload(cbor_buffer *output, cbor_map_entry *entries, size_t count, size_t limit)
{
    output->limit = limit;
    return cbor_write_map_entries(output, entries, count);
}

static int make_uint_value(uint64_t value, cbor_buffer *output, size_t limit)
{
    output->limit = limit;
    return cbor_write_uint(output, value);
}

static int make_text_value(const char *value, size_t length, cbor_buffer *output, size_t limit)
{
    output->limit = limit;
    return cbor_write_text(output, value, length);
}

static int model_get_uint(lua_State *L, int table, const char *field, uint64_t *out)
{
    int ok = 0;
    lua_getfield(L, table, field);
    if (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 0) {
        *out = (uint64_t)lua_tointeger(L, -1);
        ok = 1;
    }
    lua_pop(L, 1);
    return ok;
}

static int model_get_text(lua_State *L, int table, const char *field,
                          const char **out_text, size_t *out_length)
{
    int ok = 0;
    lua_getfield(L, table, field);
    if (lua_type(L, -1) == LUA_TSTRING) {
        *out_text = lua_tolstring(L, -1, out_length);
        ok = 1;
    }
    lua_pop(L, 1);
    return ok;
}

static int encode_table_field(lua_State *L, int table, const char *field,
                              cbor_buffer *output, size_t limit, int allow_nil)
{
    int ok;
    output->limit = limit;
    lua_getfield(L, table, field);
    if (lua_isnil(L, -1) && !allow_nil) ok = 0;
    else ok = encode_lua_item(L, -1, output);
    lua_pop(L, 1);
    return ok;
}

static int map_payload_revision_table(lua_State *L, int table, const char *field,
                                      uint64_t revision, cbor_buffer *output,
                                      size_t limit)
{
    cbor_buffer revision_value = { 0 };
    cbor_buffer object_value = { 0 };
    cbor_map_entry entries[2];
    int ok = 0;
    if (!make_uint_value(revision, &revision_value, limit) ||
        !encode_table_field(L, table, field, &object_value, limit, 0)) goto done;
    entries[0].key = field; entries[0].key_length = strlen(field);
    entries[0].value = object_value.data; entries[0].value_length = object_value.length;
    entries[1].key = "revision"; entries[1].key_length = 8u;
    entries[1].value = revision_value.data; entries[1].value_length = revision_value.length;
    ok = make_map_payload(output, entries, 2u, limit);
done:
    buffer_free(&revision_value);
    buffer_free(&object_value);
    return ok;
}

static int append_ui_message(ui_message *messages, size_t capacity, size_t *count,
                             cdin_protocol_message_type type, cbor_buffer *payload)
{
    if (*count >= capacity || payload == NULL || payload->data == NULL || payload->length == 0u) return 0;
    messages[*count].type = type;
    messages[*count].payload = *payload;
    memset(payload, 0, sizeof(*payload));
    (*count)++;
    return 1;
}

static int build_simple_model_message(lua_State *L, int model, uint64_t revision,
                                      const char *field, cdin_protocol_message_type type,
                                      ui_message *messages, size_t capacity, size_t *count,
                                      size_t limit)
{
    cbor_buffer payload = { 0 };
    int ok = map_payload_revision_table(L, model, field, revision, &payload, limit) &&
        append_ui_message(messages, capacity, count, type, &payload);
    buffer_free(&payload);
    return ok;
}

/* Lua tables cannot retain a key whose value is nil. Build the two nullable
 * PaneTree fields explicitly so an empty editor still satisfies ui.layout. */
static int build_layout_message(lua_State *L, int model, uint64_t revision,
                                ui_message *messages, size_t capacity, size_t *count,
                                size_t limit)
{
    cbor_buffer root_value = { 0 }, active_value = { 0 };
    cbor_buffer revision_value = { 0 }, layout_value = { 0 }, payload = { 0 };
    cbor_map_entry layout_entries[2];
    cbor_map_entry entries[2];
    int ok = 0;
    int layout_index;
    int root_type;
    int active_type;
    root_value.limit = limit;
    active_value.limit = limit;

    lua_getfield(L, model, "layout");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); goto done; }
    layout_index = lua_absindex(L, -1);
    lua_getfield(L, layout_index, "root");
    root_type = lua_type(L, -1);
    if (root_type == LUA_TNIL) {
        if (!cbor_write_null(&root_value)) { lua_pop(L, 2); goto done; }
    } else if (root_type == LUA_TTABLE) {
        if (!encode_lua_item(L, -1, &root_value)) { lua_pop(L, 2); goto done; }
    } else { lua_pop(L, 2); goto done; }
    lua_pop(L, 1);

    lua_getfield(L, layout_index, "active_leaf");
    active_type = lua_type(L, -1);
    if (active_type == LUA_TNIL) {
        if (!cbor_write_null(&active_value)) { lua_pop(L, 2); goto done; }
    } else if (active_type == LUA_TSTRING) {
        size_t length = 0u;
        const char *value = lua_tolstring(L, -1, &length);
        if (!cbor_write_text(&active_value, value, length)) { lua_pop(L, 2); goto done; }
    } else { lua_pop(L, 2); goto done; }
    lua_pop(L, 2); /* active_leaf and layout */

    layout_entries[0].key = "root"; layout_entries[0].key_length = 4u;
    layout_entries[0].value = root_value.data; layout_entries[0].value_length = root_value.length;
    layout_entries[1].key = "active_leaf"; layout_entries[1].key_length = 11u;
    layout_entries[1].value = active_value.data; layout_entries[1].value_length = active_value.length;
    if (!make_map_payload(&layout_value, layout_entries, 2u, limit) ||
        !make_uint_value(revision, &revision_value, limit)) goto done;
    entries[0].key = "layout"; entries[0].key_length = 6u;
    entries[0].value = layout_value.data; entries[0].value_length = layout_value.length;
    entries[1].key = "revision"; entries[1].key_length = 8u;
    entries[1].value = revision_value.data; entries[1].value_length = revision_value.length;
    if (!make_map_payload(&payload, entries, 2u, limit)) goto done;
    ok = append_ui_message(messages, capacity, count, CDIN_PROTOCOL_UI_LAYOUT, &payload);
done:
    buffer_free(&root_value);
    buffer_free(&active_value);
    buffer_free(&revision_value);
    buffer_free(&layout_value);
    buffer_free(&payload);
    return ok;
}

static int build_options_message(const char *theme, size_t theme_length, uint64_t revision,
                                 ui_message *messages, size_t capacity, size_t *count, size_t limit)
{
    cbor_buffer theme_value = { 0 }, revision_value = { 0 }, payload = { 0 };
    cbor_map_entry entries[2];
    int ok = 0;
    if (!make_text_value(theme, theme_length, &theme_value, limit) ||
        !make_uint_value(revision, &revision_value, limit)) goto done;
    entries[0].key = "theme"; entries[0].key_length = 5u;
    entries[0].value = theme_value.data; entries[0].value_length = theme_value.length;
    entries[1].key = "revision"; entries[1].key_length = 8u;
    entries[1].value = revision_value.data; entries[1].value_length = revision_value.length;
    if (!make_map_payload(&payload, entries, 2u, limit)) goto done;
    ok = append_ui_message(messages, capacity, count, CDIN_PROTOCOL_UI_OPTIONS, &payload);
done:
    buffer_free(&theme_value); buffer_free(&revision_value); buffer_free(&payload);
    return ok;
}

static int get_view_selection(lua_State *L, int view, uint64_t *anchor, uint64_t *caret)
{
    int ok;
    lua_getfield(L, view, "selection");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    ok = model_get_uint(L, -1, "anchor", anchor) && model_get_uint(L, -1, "caret", caret);
    lua_pop(L, 1);
    return ok;
}

static int build_view_message(const char *view_id, size_t view_id_length,
                              uint64_t version, uint64_t document_length,
                              uint64_t anchor, uint64_t caret, uint64_t scroll_anchor,
                              ui_message *messages, size_t capacity, size_t *count,
                              size_t limit)
{
    cbor_buffer view_value = { 0 }, version_value = { 0 }, anchor_value = { 0 },
        caret_value = { 0 }, position_value = { 0 }, scroll_value = { 0 }, ranges_value = { 0 };
    cbor_buffer cursor_payload = { 0 }, selection_payload = { 0 }, invalidate_payload = { 0 };
    cbor_buffer range_map = { 0 };
    cbor_map_entry entries[4];
    int ok = 0;
    if (!make_text_value(view_id, view_id_length, &view_value, limit) ||
        !make_uint_value(version, &version_value, limit) ||
        !make_uint_value(anchor, &anchor_value, limit) ||
        !make_uint_value(caret, &caret_value, limit) ||
        !make_uint_value(caret, &position_value, limit) ||
        !make_uint_value(scroll_anchor, &scroll_value, limit)) goto done;

    entries[0].key = "version"; entries[0].key_length = 7u;
    entries[0].value = version_value.data; entries[0].value_length = version_value.length;
    entries[1].key = "view_id"; entries[1].key_length = 7u;
    entries[1].value = view_value.data; entries[1].value_length = view_value.length;
    entries[2].key = "position"; entries[2].key_length = 8u;
    entries[2].value = position_value.data; entries[2].value_length = position_value.length;
    entries[3].key = "scroll_anchor"; entries[3].key_length = 13u;
    entries[3].value = scroll_value.data; entries[3].value_length = scroll_value.length;
    if (!make_map_payload(&cursor_payload, entries, 4u, limit)) goto done;

    entries[0].key = "anchor"; entries[0].key_length = 6u;
    entries[0].value = anchor_value.data; entries[0].value_length = anchor_value.length;
    entries[1].key = "caret"; entries[1].key_length = 5u;
    entries[1].value = caret_value.data; entries[1].value_length = caret_value.length;
    entries[2].key = "version"; entries[2].key_length = 7u;
    entries[2].value = version_value.data; entries[2].value_length = version_value.length;
    entries[3].key = "view_id"; entries[3].key_length = 7u;
    entries[3].value = view_value.data; entries[3].value_length = view_value.length;
    if (!make_map_payload(&selection_payload, entries, 4u, limit)) goto done;

    {
        cbor_buffer zero_value = { 0 }, end_value = { 0 };
        cbor_map_entry range_entries[2];
        if (!make_uint_value(0u, &zero_value, limit) ||
            !make_uint_value(document_length, &end_value, limit)) {
            buffer_free(&zero_value); buffer_free(&end_value); goto done;
        }
        range_entries[0].key = "end"; range_entries[0].key_length = 3u;
        range_entries[0].value = end_value.data; range_entries[0].value_length = end_value.length;
        range_entries[1].key = "start"; range_entries[1].key_length = 5u;
        range_entries[1].value = zero_value.data; range_entries[1].value_length = zero_value.length;
        ranges_value.limit = limit;
        if (!make_map_payload(&range_map, range_entries, 2u, limit) ||
            !cbor_write_head(&ranges_value, 4u, 1u) ||
            !buffer_append(&ranges_value, range_map.data, range_map.length)) {
            buffer_free(&zero_value); buffer_free(&end_value); goto done;
        }
        buffer_free(&zero_value); buffer_free(&end_value);
    }
    entries[0].key = "ranges"; entries[0].key_length = 6u;
    entries[0].value = ranges_value.data; entries[0].value_length = ranges_value.length;
    entries[1].key = "version"; entries[1].key_length = 7u;
    entries[1].value = version_value.data; entries[1].value_length = version_value.length;
    entries[2].key = "view_id"; entries[2].key_length = 7u;
    entries[2].value = view_value.data; entries[2].value_length = view_value.length;
    if (!make_map_payload(&invalidate_payload, entries, 3u, limit)) goto done;

    if (!append_ui_message(messages, capacity, count, CDIN_PROTOCOL_UI_CURSOR, &cursor_payload) ||
        !append_ui_message(messages, capacity, count, CDIN_PROTOCOL_UI_SELECTION, &selection_payload) ||
        !append_ui_message(messages, capacity, count, CDIN_PROTOCOL_VIEW_INVALIDATE, &invalidate_payload)) goto done;
    ok = 1;
done:
    buffer_free(&view_value); buffer_free(&version_value); buffer_free(&anchor_value);
    buffer_free(&caret_value); buffer_free(&position_value); buffer_free(&scroll_value); buffer_free(&ranges_value);
    buffer_free(&cursor_payload); buffer_free(&selection_payload); buffer_free(&invalidate_payload);
    buffer_free(&range_map);
    return ok;
}

static int collect_lua_map_keys(lua_State *L, int table, lua_map_key *keys,
                                size_t capacity, size_t *out_count)
{
    size_t count = 0u;
    int absolute = lua_absindex(L, table);
    lua_pushnil(L);
    while (lua_next(L, absolute) != 0) {
        size_t length = 0u;
        const char *key;
        if (lua_type(L, -2) != LUA_TSTRING || count >= capacity) {
            lua_pop(L, 2);
            return 0;
        }
        key = lua_tolstring(L, -2, &length);
        if (length == 0u || memchr(key, '\0', length) != NULL) {
            lua_pop(L, 2);
            return 0;
        }
        keys[count].value = key;
        keys[count].length = length;
        keys[count].is_ratio = 0;
        count++;
        lua_pop(L, 1);
    }
    qsort(keys, count, sizeof(keys[0]), lua_key_compare);
    *out_count = count;
    return 1;
}

static int build_panel_message(lua_State *L, const char *panel_id, size_t panel_id_length,
                               int panel_index, uint64_t revision,
                               ui_message *messages, size_t capacity, size_t *count, size_t limit)
{
    cbor_buffer revision_value = { 0 }, id_value = { 0 }, panel_value = { 0 }, payload = { 0 };
    cbor_map_entry entries[3];
    int ok = 0;
    panel_value.limit = limit;
    if (!make_uint_value(revision, &revision_value, limit) ||
        !make_text_value(panel_id, panel_id_length, &id_value, limit) ||
        !encode_lua_item(L, panel_index, &panel_value)) goto done;
    entries[0].key = "panel"; entries[0].key_length = 5u;
    entries[0].value = panel_value.data; entries[0].value_length = panel_value.length;
    entries[1].key = "panel_id"; entries[1].key_length = 8u;
    entries[1].value = id_value.data; entries[1].value_length = id_value.length;
    entries[2].key = "revision"; entries[2].key_length = 8u;
    entries[2].value = revision_value.data; entries[2].value_length = revision_value.length;
    if (!make_map_payload(&payload, entries, 3u, limit)) goto done;
    ok = append_ui_message(messages, capacity, count, CDIN_PROTOCOL_UI_PANEL, &payload);
done:
    buffer_free(&revision_value); buffer_free(&id_value); buffer_free(&panel_value); buffer_free(&payload);
    return ok;
}

static void free_ui_messages(ui_message *messages, size_t count)
{
    size_t index;
    if (messages == NULL) return;
    for (index = 0u; index < count; index++) buffer_free(&messages[index].payload);
    free(messages);
}

/* Bound the complete semantic revision, not just each individual CBOR frame.
 * Keep room for the final ui.flush envelope so one model cannot consume more
 * than the negotiated per-client message budget across its entire batch. */
static int ui_messages_fit_budget(const ui_message *messages, size_t count, size_t limit)
{
    size_t index;
    size_t total = 0u;
    size_t budget = limit > 48u ? limit - 48u : 0u;
    if (messages == NULL || budget == 0u) return 0;
    for (index = 0u; index < count; index++) {
        if (messages[index].payload.length > budget - total) return 0;
        total += messages[index].payload.length;
    }
    return 1;
}

static cdin_backend_status publish_model_to_client(cdin_lua_host *host, cdin_protocol_client *client,
                                                    int model_index)
{
    lua_State *L = host->state;
    uint64_t revision = 0u;
    uint64_t view_version, document_length, anchor, caret, scroll_anchor;
    const char *theme = NULL;
    size_t theme_length = 0u;
    lua_map_key view_keys[UI_MAX_MODEL_VIEWS];
    lua_map_key panel_keys[UI_MAX_MODEL_PANELS];
    size_t view_count = 0u, panel_count = 0u, message_count = 0u, capacity;
    size_t index;
    size_t limit = (size_t)host->backend->config.max_message_bytes;
    ui_message *messages = NULL;
    cdin_protocol_message *batch = NULL;
    uint64_t flush_sequence = 0u;
    cdin_backend_status status = CDIN_BACKEND_OK;
    int model = lua_absindex(L, model_index);
    int views_index, panels_index;
    cbor_buffer styles = { 0 };
    static const uint8_t empty_map = 0xa0u;

    if (!model_get_uint(L, model, "revision", &revision) ||
        !model_get_text(L, model, "theme", &theme, &theme_length)) return CDIN_BACKEND_INVALID_ARGUMENT;
    lua_getfield(L, model, "views");
    if (!lua_istable(L, -1) || !collect_lua_map_keys(L, -1, view_keys, UI_MAX_MODEL_VIEWS, &view_count)) {
        lua_pop(L, 1);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    views_index = lua_absindex(L, -1);
    lua_getfield(L, model, "panels");
    if (!lua_istable(L, -1) || !collect_lua_map_keys(L, -1, panel_keys, UI_MAX_MODEL_PANELS, &panel_count)) {
        lua_pop(L, 2);
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    panels_index = lua_absindex(L, -1);

    if (view_count > (SIZE_MAX - 6u - panel_count) / 3u) {
        status = CDIN_BACKEND_LIMIT_EXCEEDED;
        goto done;
    }
    capacity = 6u + panel_count + (view_count * 3u);
    if (capacity == 0u || capacity > SIZE_MAX / sizeof(*messages) ||
        capacity > SIZE_MAX / sizeof(*batch)) {
        status = CDIN_BACKEND_LIMIT_EXCEEDED;
        goto done;
    }
    messages = (ui_message *)calloc(capacity, sizeof(*messages));
    batch = (cdin_protocol_message *)calloc(capacity, sizeof(*batch));
    if (messages == NULL || batch == NULL) { status = CDIN_BACKEND_NO_MEMORY; goto done; }

    if (!build_options_message(theme, theme_length, revision, messages, capacity, &message_count, limit) ||
        !ui_messages_fit_budget(messages, message_count, limit)) {
        status = CDIN_BACKEND_LIMIT_EXCEEDED; goto done;
    }
    lua_getfield(L, model, "styles");
    styles.limit = limit;
    if (lua_isnil(L, -1)) {
        buffer_append(&styles, &empty_map, 1u);
    } else if (!encode_lua_item(L, -1, &styles)) {
        lua_pop(L, 1); status = CDIN_BACKEND_INVALID_ARGUMENT; goto done;
    }
    lua_pop(L, 1);
    {
        cbor_buffer revision_value = { 0 }, payload = { 0 };
        cbor_map_entry entries[2];
        if (!make_uint_value(revision, &revision_value, limit)) {
            buffer_free(&revision_value); status = CDIN_BACKEND_NO_MEMORY; goto done;
        }
        entries[0].key = "styles"; entries[0].key_length = 6u;
        entries[0].value = styles.data; entries[0].value_length = styles.length;
        entries[1].key = "revision"; entries[1].key_length = 8u;
        entries[1].value = revision_value.data; entries[1].value_length = revision_value.length;
        if (!make_map_payload(&payload, entries, 2u, limit) ||
            !append_ui_message(messages, capacity, &message_count, CDIN_PROTOCOL_UI_STYLE_TABLE, &payload)) {
            buffer_free(&revision_value); buffer_free(&payload); status = CDIN_BACKEND_NO_MEMORY; goto done;
        }
        buffer_free(&revision_value); buffer_free(&payload);
    }
    if (!ui_messages_fit_budget(messages, message_count, limit)) {
        status = CDIN_BACKEND_LIMIT_EXCEEDED; goto done;
    }
    if (!build_layout_message(L, model, revision, messages, capacity, &message_count, limit) ||
        !build_simple_model_message(L, model, revision, "status", CDIN_PROTOCOL_UI_STATUSLINE,
                                    messages, capacity, &message_count, limit) ||
        !ui_messages_fit_budget(messages, message_count, limit)) {
        status = CDIN_BACKEND_LIMIT_EXCEEDED; goto done;
    }
    {
        cbor_buffer revision_value = { 0 }, prompt_value = { 0 }, payload = { 0 };
        cbor_map_entry entries[2];
        if (!make_uint_value(revision, &revision_value, limit) ||
            !encode_table_field(L, model, "prompt", &prompt_value, limit, 1)) {
            buffer_free(&revision_value); buffer_free(&prompt_value); status = CDIN_BACKEND_INVALID_ARGUMENT; goto done;
        }
        entries[0].key = "prompt"; entries[0].key_length = 6u;
        entries[0].value = prompt_value.data; entries[0].value_length = prompt_value.length;
        entries[1].key = "revision"; entries[1].key_length = 8u;
        entries[1].value = revision_value.data; entries[1].value_length = revision_value.length;
        if (!make_map_payload(&payload, entries, 2u, limit) ||
            !append_ui_message(messages, capacity, &message_count, CDIN_PROTOCOL_UI_PROMPT, &payload)) {
            buffer_free(&revision_value); buffer_free(&prompt_value); buffer_free(&payload);
            status = CDIN_BACKEND_NO_MEMORY; goto done;
        }
        buffer_free(&revision_value); buffer_free(&prompt_value); buffer_free(&payload);
    }
    if (!ui_messages_fit_budget(messages, message_count, limit)) {
        status = CDIN_BACKEND_LIMIT_EXCEEDED; goto done;
    }

    for (index = 0u; index < view_count; index++) {
        uint64_t visible = 1u;
        lua_pushlstring(L, view_keys[index].value, view_keys[index].length);
        lua_rawget(L, views_index);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); status = CDIN_BACKEND_INVALID_ARGUMENT; goto done; }
        if (!model_get_uint(L, -1, "document_version", &view_version) ||
            !model_get_uint(L, -1, "document_length", &document_length) ||
            !model_get_uint(L, -1, "scroll_anchor", &scroll_anchor) ||
            scroll_anchor > document_length) {
            lua_pop(L, 1); status = CDIN_BACKEND_INVALID_ARGUMENT; goto done;
        }
        lua_getfield(L, -1, "visible");
        visible = lua_isnil(L, -1) ? 1u : (lua_toboolean(L, -1) ? 1u : 0u);
        lua_pop(L, 1);
        if (visible != 0u) {
            if (!get_view_selection(L, -1, &anchor, &caret) ||
                !build_view_message(view_keys[index].value, view_keys[index].length,
                                    view_version, document_length, anchor, caret, scroll_anchor,
                                    messages, capacity, &message_count, limit)) {
                lua_pop(L, 1); status = CDIN_BACKEND_INVALID_ARGUMENT; goto done;
            }
        }
        lua_pop(L, 1);
        if (!ui_messages_fit_budget(messages, message_count, limit)) {
            status = CDIN_BACKEND_LIMIT_EXCEEDED; goto done;
        }
    }

    for (index = 0u; index < panel_count; index++) {
        lua_pushlstring(L, panel_keys[index].value, panel_keys[index].length);
        lua_rawget(L, panels_index);
        if (!build_panel_message(L, panel_keys[index].value, panel_keys[index].length,
                                 -1, revision, messages, capacity, &message_count, limit)) {
            lua_pop(L, 1); status = CDIN_BACKEND_INVALID_ARGUMENT; goto done;
        }
        lua_pop(L, 1);
        if (!ui_messages_fit_budget(messages, message_count, limit)) {
            status = CDIN_BACKEND_LIMIT_EXCEEDED; goto done;
        }
    }

    for (index = 0u; index < message_count; index++) {
        batch[index].type = messages[index].type;
        batch[index].payload = messages[index].payload.data;
        batch[index].payload_length = messages[index].payload.length;
    }
    status = cdin_protocol_publish_snapshot(client, batch, message_count, revision, &flush_sequence);
    if (status == CDIN_BACKEND_OK) {
        CDIN_BACKEND_TRACE(host->backend->kernel, "PROTOCOL",
            "UI snapshot queued client_id=%llu revision=%llu messages=%llu flush=%llu",
            (unsigned long long)cdin_protocol_client_id(client),
            (unsigned long long)revision, (unsigned long long)message_count,
            (unsigned long long)flush_sequence);
    }

done:
    buffer_free(&styles);
    free(batch);
    free_ui_messages(messages, message_count);
    lua_settop(L, views_index - 1);
    return status;
}

static cdin_backend_status publish_latest_ui(cdin_lua_host *host, cdin_protocol_client *client)
{
    lua_State *L;
    int base;
    cdin_backend_status status;
    if (host == NULL || host->state == NULL || host->latest_ui_ref == LUA_NOREF || client == NULL)
        return CDIN_BACKEND_OK;
    if (!cdin_backend_is_owner(host->backend)) return CDIN_BACKEND_INVALID_STATE;
    if (!cdin_protocol_client_is_attached(client)) return CDIN_BACKEND_INVALID_STATE;
    L = host->state;
    base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, host->latest_ui_ref);
    {
        uint64_t latest_revision = 0u;
        uint64_t sent_revision = cdin_protocol_client_snapshot_revision(client);
        if (!model_get_uint(L, -1, "revision", &latest_revision)) {
            status = CDIN_BACKEND_INVALID_ARGUMENT;
        } else if (cdin_protocol_client_has_snapshot(client) && latest_revision <= sent_revision) {
            status = CDIN_BACKEND_OK;
        } else {
            status = publish_model_to_client(host, client, -1);
        }
    }
    lua_settop(L, base);
    return status;
}

int lua_ui_publish(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    int base = lua_gettop(L);
    int model = 2;
    size_t index;
    uint64_t revision = 0u;
    if (!lua_istable(L, model) || lua_table_has_metatable(L, model))
        return raise_argument(L, "ui.publish model");
    {
        const char *schema = NULL;
        size_t schema_length = 0u;
        if (!model_get_text(L, model, "schema", &schema, &schema_length) ||
            schema_length != sizeof(UI_MODEL_SCHEMA) - 1u ||
            memcmp(schema, UI_MODEL_SCHEMA, sizeof(UI_MODEL_SCHEMA) - 1u) != 0 ||
            !model_get_uint(L, model, "revision", &revision))
            return raise_argument(L, "ui.publish schema/revision");
    }

    /* Retain the newest semantic snapshot before attempting delivery. If a
     * slow observer is backpressured, a later publish/attach can resync it. */
    lua_pushvalue(L, model);
    if (host->latest_ui_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, host->latest_ui_ref);
    host->latest_ui_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (host->backend->protocol != NULL) {
        for (index = 0u; index < host->backend->config.max_frontends; index++) {
            cdin_protocol_client *client = cdin_protocol_server_client_at(host->backend->protocol, index);
            cdin_backend_status status;
            if (client == NULL || !cdin_protocol_client_is_attached(client)) continue;
            status = publish_model_to_client(host, client, model);
            if (status != CDIN_BACKEND_OK) {
                CDIN_BACKEND_WARN(host->backend->kernel, "PROTOCOL",
                    "UI publication deferred client_id=%llu revision=%llu status=%s reason=client backpressure or model serialization failure",
                    (unsigned long long)cdin_protocol_client_id(client),
                    (unsigned long long)revision,
                    cdin_backend_status_name(status));
            }
        }
    }
    lua_settop(L, base);
    /* Runtime state is authoritative; a slow frontend must not roll back a
     * command or document edit. The latest snapshot is retained for resync. */
    lua_pushboolean(L, 1);
    return 1;
}

cdin_backend_status cdin_lua_host_publish_latest_ui(cdin_lua_host *host, cdin_protocol_client *client)
{
    return publish_latest_ui(host, client);
}

typedef struct cbor_decode_cursor {
    const uint8_t *data;
    size_t length;
    size_t offset;
} cbor_decode_cursor;

static int cbor_decode_head(cbor_decode_cursor *cursor, uint8_t *out_major, uint64_t *out_value)
{
    uint8_t initial;
    uint8_t additional;
    size_t width;
    size_t index;
    uint64_t value = 0u;
    if (cursor == NULL || out_major == NULL || out_value == NULL || cursor->offset >= cursor->length)
        return 0;
    initial = cursor->data[cursor->offset++];
    *out_major = (uint8_t)(initial >> 5);
    additional = (uint8_t)(initial & 0x1fu);
    if (additional < 24u) {
        *out_value = additional;
        return 1;
    }
    if (additional == 24u) width = 1u;
    else if (additional == 25u) width = 2u;
    else if (additional == 26u) width = 4u;
    else if (additional == 27u) width = 8u;
    else return 0; /* Indefinite-length items and reserved additional values are forbidden. */
    if (width > cursor->length - cursor->offset) return 0;
    for (index = 0u; index < width; index++)
        value = (value << 8) | cursor->data[cursor->offset++];
    *out_value = value;
    return 1;
}

static int cbor_decode_lua_item(lua_State *L, cbor_decode_cursor *cursor, unsigned depth)
{
    uint8_t major;
    uint64_t value;
    uint64_t index;
    if (depth > UI_MAX_PROTOCOL_DEPTH || !cbor_decode_head(cursor, &major, &value)) return 0;
    switch (major) {
    case 0u:
        if (value > (uint64_t)LUA_MAXINTEGER) return 0;
        lua_pushinteger(L, (lua_Integer)value);
        return 1;
    case 1u:
        if (value > (uint64_t)LUA_MAXINTEGER) return 0;
        lua_pushinteger(L, (lua_Integer)(-1 - (lua_Integer)value));
        return 1;
    case 2u:
    case 3u:
        if (value > SIZE_MAX || (size_t)value > cursor->length - cursor->offset) return 0;
        lua_pushlstring(L, (const char *)(cursor->data + cursor->offset), (size_t)value);
        cursor->offset += (size_t)value;
        return 1;
    case 4u:
        if (value > 4096u || value > (uint64_t)INT_MAX) return 0;
        lua_createtable(L, (int)value, 0);
        for (index = 0u; index < value; index++) {
            if (!cbor_decode_lua_item(L, cursor, depth + 1u)) return 0;
            lua_rawseti(L, -2, (lua_Integer)(index + 1u));
        }
        return 1;
    case 5u:
        if (value > UI_MAX_MODEL_MAP_FIELDS || value > (uint64_t)INT_MAX) return 0;
        lua_createtable(L, 0, (int)value);
        for (index = 0u; index < value; index++) {
            uint8_t key_major;
            uint64_t key_length;
            if (!cbor_decode_head(cursor, &key_major, &key_length) || key_major != 3u ||
                key_length == 0u || key_length > SIZE_MAX ||
                (size_t)key_length > cursor->length - cursor->offset) return 0;
            lua_pushlstring(L, (const char *)(cursor->data + cursor->offset), (size_t)key_length);
            cursor->offset += (size_t)key_length;
            if (!cbor_decode_lua_item(L, cursor, depth + 1u)) return 0;
            lua_settable(L, -3);
        }
        return 1;
    case 7u:
        if (value == 20u) lua_pushboolean(L, 0);
        else if (value == 21u) lua_pushboolean(L, 1);
        else if (value == 22u) lua_pushnil(L);
        else return 0; /* Floats and unrecognized simple values are not in the contract. */
        return 1;
    default:
        return 0;
    }
}

static int decode_event_payload(lua_State *L, const uint8_t *payload, size_t length)
{
    cbor_decode_cursor cursor;
    if (payload == NULL || length == 0u) return 0;
    cursor.data = payload;
    cursor.length = length;
    cursor.offset = 0u;
    if (!cbor_decode_lua_item(L, &cursor, 0u) || cursor.offset != cursor.length || !lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

static void push_error_payload(const char *code, const char *message,
                               cbor_buffer *output, size_t limit)
{
    cbor_buffer code_value = { 0 };
    cbor_buffer message_value = { 0 };
    cbor_map_entry entries[2];
    output->limit = limit;
    if (code == NULL || code[0] == '\0') code = "INTERNAL";
    if (message == NULL || message[0] == '\0') message = "Frontend request failed";
    if (strlen(message) > 2048u) message = "Frontend request failed (diagnostic truncated)";
    if (!make_text_value(code, strlen(code), &code_value, limit) ||
        !make_text_value(message, strlen(message), &message_value, limit)) goto done;
    entries[0].key = "code"; entries[0].key_length = 4u;
    entries[0].value = code_value.data; entries[0].value_length = code_value.length;
    entries[1].key = "message"; entries[1].key_length = 7u;
    entries[1].value = message_value.data; entries[1].value_length = message_value.length;
    (void)make_map_payload(output, entries, 2u, limit);
done:
    buffer_free(&code_value);
    buffer_free(&message_value);
}

static void queue_protocol_error(cdin_lua_host *host, cdin_protocol_client *client,
                                 const char *code, const char *message)
{
    cbor_buffer payload = { 0 };
    cdin_protocol_message response;
    cdin_backend_status status;
    uint64_t flush_sequence = 0u;
    push_error_payload(code, message, &payload,
                       (size_t)cdin_protocol_client_payload_limit(client));
    if (payload.data == NULL || payload.length == 0u) {
        CDIN_BACKEND_WARN(host->backend->kernel, "PROTOCOL",
            "could not construct error response client_id=%llu code=%s",
            (unsigned long long)cdin_protocol_client_id(client), code != NULL ? code : "INTERNAL");
        buffer_free(&payload);
        return;
    }
    memset(&response, 0, sizeof(response));
    response.type = CDIN_PROTOCOL_ERROR;
    response.payload = payload.data;
    response.payload_length = payload.length;
    status = cdin_protocol_publish_batch(client, &response, 1u, &flush_sequence);
    if (status != CDIN_BACKEND_OK) {
        CDIN_BACKEND_WARN(host->backend->kernel, "PROTOCOL",
            "error response not queued client_id=%llu status=%s",
            (unsigned long long)cdin_protocol_client_id(client), cdin_backend_status_name(status));
    }
    buffer_free(&payload);
}

static void queue_lua_error(cdin_lua_host *host, cdin_protocol_client *client,
                            lua_State *L, int error_index)
{
    const char *code = "INTERNAL";
    const char *message = "Runtime rejected the Frontend request";
    error_index = lua_absindex(L, error_index);
    if (lua_istable(L, error_index)) {
        lua_getfield(L, error_index, "code");
        if (lua_type(L, -1) == LUA_TSTRING) code = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, error_index, "message");
        if (lua_type(L, -1) == LUA_TSTRING) message = lua_tostring(L, -1);
        lua_pop(L, 1);
    } else if (lua_type(L, error_index) == LUA_TSTRING) {
        message = lua_tostring(L, error_index);
    }
    queue_protocol_error(host, client, code, message);
}

static int encode_view_lines_payload(lua_State *L, int result_index, const char *view_id,
                                     size_t view_id_length, size_t limit, cbor_buffer *payload)
{
    cbor_buffer lines = { 0 }, version = { 0 }, first_line = { 0 }, total_lines = { 0 }, id = { 0 };
    cbor_map_entry entries[5];
    size_t count;
    size_t index;
    int result = lua_absindex(L, result_index);
    int lines_index;
    int ok = 0;
    uint64_t version_value;
    uint64_t first_line_value;
    uint64_t total_lines_value;
    lines.limit = limit;
    lua_getfield(L, result, "version");
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) { lua_pop(L, 1); goto done; }
    version_value = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, result, "first_line");
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) { lua_pop(L, 1); goto done; }
    first_line_value = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, result, "total_lines");
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1) { lua_pop(L, 1); goto done; }
    total_lines_value = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, result, "lines");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); goto done; }
    lines_index = lua_absindex(L, -1);
    count = lua_rawlen(L, lines_index);
    /* Reject inconsistent windows before constructing a wire payload. This
     * keeps the line range tied to the exact total_lines reported by Kernel. */
    if (count == 0u || count > 4096u || first_line_value >= total_lines_value ||
        (uint64_t)count > total_lines_value - first_line_value ||
        !cbor_write_head(&lines, 4u, (uint64_t)count)) {
        lua_pop(L, 1);
        goto done;
    }
    for (index = 1u; index <= count; index++) {
        size_t line_length = 0u;
        const char *line_bytes;
        lua_rawgeti(L, lines_index, (lua_Integer)index);
        if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 2); goto done; }
        line_bytes = lua_tolstring(L, -1, &line_length);
        if (!cbor_write_head(&lines, 5u, 1u) ||
            !cbor_write_text(&lines, "bytes", 5u) ||
            !cbor_write_bytes(&lines, (const uint8_t *)line_bytes, line_length)) {
            lua_pop(L, 2);
            goto done;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (!make_uint_value(version_value, &version, limit) ||
        !make_uint_value(first_line_value, &first_line, limit) ||
        !make_uint_value(total_lines_value, &total_lines, limit) ||
        !make_text_value(view_id, view_id_length, &id, limit)) goto done;
    entries[0].key = "lines"; entries[0].key_length = 5u;
    entries[0].value = lines.data; entries[0].value_length = lines.length;
    entries[1].key = "version"; entries[1].key_length = 7u;
    entries[1].value = version.data; entries[1].value_length = version.length;
    entries[2].key = "view_id"; entries[2].key_length = 7u;
    entries[2].value = id.data; entries[2].value_length = id.length;
    entries[3].key = "first_line"; entries[3].key_length = 10u;
    entries[3].value = first_line.data; entries[3].value_length = first_line.length;
    entries[4].key = "total_lines"; entries[4].key_length = 11u;
    entries[4].value = total_lines.data; entries[4].value_length = total_lines.length;
    ok = make_map_payload(payload, entries, 5u, limit);
done:
    buffer_free(&lines);
    buffer_free(&version);
    buffer_free(&first_line);
    buffer_free(&total_lines);
    buffer_free(&id);
    return ok;
}

static int lua_call_read_view_lines(cdin_lua_host *host, cdin_protocol_client *client,
                                    int event_index, uint64_t event_sequence)
{
    lua_State *L = host->state;
    int base = lua_gettop(L);
    int event = lua_absindex(L, event_index);
    int runtime_index;
    const char *view_id;
    size_t view_id_length;
    uint64_t first_line;
    uint64_t line_count;
    uint64_t expected_version;
    cbor_buffer payload = { 0 };
    cdin_protocol_message response;
    cdin_backend_status status;

    lua_getfield(L, event, "view_id");
    if (lua_type(L, -1) != LUA_TSTRING) { lua_settop(L, base); return 0; }
    view_id = lua_tolstring(L, -1, &view_id_length);
    lua_pushvalue(L, -1); /* Keep the id alive while the Lua method executes. */
    lua_pop(L, 1);
    lua_getfield(L, event, "first_line");
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) { lua_settop(L, base); return 0; }
    first_line = (uint64_t)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, event, "line_count");
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) <= 0) { lua_settop(L, base); return 0; }
    line_count = (uint64_t)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, event, "version");
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) { lua_settop(L, base); return 0; }
    expected_version = (uint64_t)lua_tointeger(L, -1); lua_pop(L, 1);

    lua_rawgeti(L, LUA_REGISTRYINDEX, host->runtime_ref);
    runtime_index = lua_absindex(L, -1);
    lua_getfield(L, runtime_index, "read_view_lines");
    if (!lua_isfunction(L, -1)) { lua_settop(L, base); return 0; }
    lua_pushvalue(L, runtime_index);
    lua_pushlstring(L, view_id, view_id_length);
    lua_pushinteger(L, (lua_Integer)first_line);
    lua_pushinteger(L, (lua_Integer)line_count);
    lua_pushinteger(L, (lua_Integer)expected_version);
    if (lua_pcall(L, 5, 1, 0) != LUA_OK) {
        queue_lua_error(host, client, L, -1);
        lua_settop(L, base);
        return 1;
    }
    if (!lua_istable(L, -1) || !encode_view_lines_payload(L, -1, view_id, view_id_length,
            (size_t)cdin_protocol_client_payload_limit(client), &payload)) {
        lua_settop(L, base);
        queue_protocol_error(host, client, "INTERNAL", "Runtime returned an invalid view.lines result");
        buffer_free(&payload);
        return 1;
    }
    memset(&response, 0, sizeof(response));
    response.type = CDIN_PROTOCOL_VIEW_LINES;
    response.sequence = event_sequence;
    response.payload = payload.data;
    response.payload_length = payload.length;
    {
        uint64_t flush_sequence = 0u;
        status = cdin_protocol_publish_batch(client, &response, 1u, &flush_sequence);
    }
    if (status != CDIN_BACKEND_OK) {
        CDIN_BACKEND_WARN(host->backend->kernel, "PROTOCOL",
            "view.lines response deferred client_id=%llu request_sequence=%llu status=%s",
            (unsigned long long)cdin_protocol_client_id(client),
            (unsigned long long)event_sequence, cdin_backend_status_name(status));
    }
    buffer_free(&payload);
    lua_settop(L, base);
    return 1;
}

static int dispatch_frontend_event(cdin_lua_host *host, cdin_protocol_client *client,
                                   const cdin_protocol_message *message)
{
    lua_State *L = host->state;
    int base = lua_gettop(L);
    int event_index;
    int runtime_index;
    const char *type_name = cdin_protocol_message_name((cdin_protocol_message_type)message->type);
    if (type_name == NULL || !decode_event_payload(L, message->payload, message->payload_length)) {
        lua_settop(L, base);
        queue_protocol_error(host, client, "INVALID_SCHEMA", "Frontend payload could not be decoded");
        return 1;
    }
    event_index = lua_absindex(L, -1);
    lua_pushstring(L, type_name); lua_setfield(L, event_index, "type");
    lua_pushinteger(L, (lua_Integer)message->sequence); lua_setfield(L, event_index, "sequence");
    lua_pushinteger(L, (lua_Integer)cdin_protocol_client_id(client)); lua_setfield(L, event_index, "client_id");

    if (message->type == CDIN_PROTOCOL_VIEW_REQUEST_LINES) {
        int handled = lua_call_read_view_lines(host, client, event_index, message->sequence);
        lua_settop(L, base);
        if (!handled) queue_protocol_error(host, client, "INVALID_ARGUMENT", "Invalid view.request_lines arguments");
        return 1;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, host->runtime_ref);
    runtime_index = lua_absindex(L, -1);
    lua_getfield(L, runtime_index, "handle_frontend_event");
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        queue_protocol_error(host, client, "UNSUPPORTED", "Runtime frontend event handler is unavailable");
        return 1;
    }
    lua_pushvalue(L, runtime_index);
    lua_pushvalue(L, event_index);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        queue_lua_error(host, client, L, -1);
        lua_settop(L, base);
        return 1;
    }
    lua_settop(L, base);
    return 1;
}

cdin_backend_status cdin_lua_host_dispatch_frontend(cdin_lua_host *host, size_t max_events,
                                                     size_t *out_processed)
{
    size_t budget;
    size_t processed = 0u;
    size_t count;
    size_t start;
    size_t offset;
    if (host == NULL || out_processed == NULL || !host->started || host->backend == NULL)
        return CDIN_BACKEND_INVALID_ARGUMENT;
    if (!cdin_backend_is_owner(host->backend)) return CDIN_BACKEND_INVALID_STATE;
    *out_processed = 0u;
    if (host->backend->protocol == NULL) return CDIN_BACKEND_OK;
    count = (size_t)host->backend->config.max_frontends;
    if (count == 0u) return CDIN_BACKEND_OK;
    budget = max_events == 0u ? 64u : max_events;
    start = host->next_protocol_client % count;
    for (offset = 0u; offset < count && processed < budget; offset++) {
        size_t client_index = (start + offset) % count;
        cdin_protocol_client *client = cdin_protocol_server_client_at(host->backend->protocol, client_index);
        size_t client_processed = 0u;
        if (client == NULL || !cdin_protocol_client_is_attached(client)) continue;
        while (client_processed < budget - processed && cdin_protocol_pending_events(client) != 0u) {
            cdin_protocol_message event;
            cdin_backend_status status = cdin_protocol_event_at(client, 0u, &event);
            if (status != CDIN_BACKEND_OK) break;
            (void)dispatch_frontend_event(host, client, &event);
            status = cdin_protocol_consume_event(client);
            if (status != CDIN_BACKEND_OK) {
                CDIN_BACKEND_ERROR(host->backend->kernel, "PROTOCOL",
                    "could not consume inbound event client_id=%llu sequence=%llu status=%s",
                    (unsigned long long)cdin_protocol_client_id(client),
                    (unsigned long long)event.sequence, cdin_backend_status_name(status));
                return status;
            }
            client_processed++;
            processed++;
        }
    }
    host->next_protocol_client = (start + 1u) % count;
    *out_processed = processed;
    if (processed != 0u) {
        CDIN_BACKEND_TRACE(host->backend->kernel, "PROTOCOL",
            "frontend events dispatched count=%llu",
            (unsigned long long)processed);
    }
    return CDIN_BACKEND_OK;
}

#endif
