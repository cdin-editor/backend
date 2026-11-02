/* lua-host-common: isolated implementation of the optional Lua 5.4 bridge. */
#ifndef CDIN_WITH_LUA
typedef int lua_host_common_disabled_translation_unit;
#else
#include "lua_host_internal.h"

static const char *status_code(cdin_backend_status status)
{
    switch (status) {
        case CDIN_BACKEND_OK: return "OK";
        case CDIN_BACKEND_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case CDIN_BACKEND_INVALID_STATE: return "INVALID_STATE";
        case CDIN_BACKEND_NO_MEMORY: return "NO_MEMORY";
        case CDIN_BACKEND_UNSUPPORTED: return "UNSUPPORTED";
        case CDIN_BACKEND_VERSION_MISMATCH: return "VERSION_MISMATCH";
        case CDIN_BACKEND_LIMIT_EXCEEDED: return "LIMIT_EXCEEDED";
        case CDIN_BACKEND_QUEUE_FULL: return "BUSY";
        case CDIN_BACKEND_NOT_FOUND: return "INVALID_HANDLE";
        case CDIN_BACKEND_BUSY: return "BUSY";
        case CDIN_BACKEND_STALE_VERSION: return "STALE_VERSION";
        case CDIN_BACKEND_OUT_OF_RANGE: return "OUT_OF_RANGE";
        case CDIN_BACKEND_INVALID_POSITION: return "INVALID_POSITION";
        case CDIN_BACKEND_PATTERN_INVALID: return "PATTERN_INVALID";
        case CDIN_BACKEND_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
        case CDIN_BACKEND_SHUTDOWN: return "SHUTDOWN";
        case CDIN_BACKEND_IO_ERROR: return "IO_ERROR";
        case CDIN_BACKEND_CANCELLED: return "CANCELLED";
        case CDIN_BACKEND_INTERNAL: default: return "INTERNAL";
    }
}

/* Translate Kernel-adapter error details into the Lua bridge's structured
 * detail model. Shared with lua_host_documents.c. */
void lua_error_details_from_host(cdin_lua_error_details *target,
                                        const cdin_host_error_details *source,
                                        cdin_backend_status status)
{
    memset(target, 0, sizeof(*target));
    if (source == NULL) return;
    if (status == CDIN_BACKEND_STALE_VERSION) {
        target->has_expected = 1; target->expected = source->expected;
        target->has_current = 1; target->current = source->current;
    } else if (status == CDIN_BACKEND_OUT_OF_RANGE || status == CDIN_BACKEND_INVALID_POSITION) {
        target->has_requested = 1; target->requested = source->requested;
        target->has_length = 1; target->length = source->length;
    } else if (status == CDIN_BACKEND_LIMIT_EXCEEDED) {
        target->has_limit = source->limit_id != 0u; target->limit = source->limit_id;
        target->has_required = source->required != 0u; target->required = source->required;
    } else if (status == CDIN_BACKEND_BUFFER_TOO_SMALL) {
        target->has_required = 1; target->required = source->required;
    }
}

int raise_status(lua_State *L, cdin_backend_status status, const char *operation,
                        const cdin_lua_error_details *details)
{
    const char *code = status_code(status);
    char message[256];
    int written = snprintf(message, sizeof(message), "%s failed: %s", operation, code);
    if (written < 0) (void)snprintf(message, sizeof(message), "%s", code);
    lua_createtable(L, 0, 4);
    lua_pushliteral(L, "cdin.runtime"); lua_setfield(L, -2, "domain");
    lua_pushstring(L, code); lua_setfield(L, -2, "code");
    lua_pushstring(L, message); lua_setfield(L, -2, "message");
    if (details != NULL) {
        lua_createtable(L, 0, 6);
        if (details->has_expected) { lua_pushinteger(L, (lua_Integer)details->expected); lua_setfield(L, -2, "expected"); }
        if (details->has_current) { lua_pushinteger(L, (lua_Integer)details->current); lua_setfield(L, -2, "current"); }
        if (details->has_requested) { lua_pushinteger(L, (lua_Integer)details->requested); lua_setfield(L, -2, "requested"); }
        if (details->has_length) { lua_pushinteger(L, (lua_Integer)details->length); lua_setfield(L, -2, "length"); }
        if (details->has_limit) { lua_pushinteger(L, (lua_Integer)details->limit); lua_setfield(L, -2, "limit_id"); }
        if (details->has_required) { lua_pushinteger(L, (lua_Integer)details->required); lua_setfield(L, -2, "required"); }
        lua_setfield(L, -2, "details");
    }
    return lua_error(L);
}

int raise_argument(lua_State *L, const char *operation)
{
    return raise_status(L, CDIN_BACKEND_INVALID_ARGUMENT, operation, NULL);
}

int raise_io(lua_State *L, const char *operation)
{
    return raise_status(L, CDIN_BACKEND_IO_ERROR, operation, NULL);
}

uint64_t check_u64(lua_State *L, int index, const char *field)
{
    lua_Integer value;
    if (!lua_isinteger(L, index)) {
        luaL_error(L, "%s must be a non-negative integer", field);
        return 0u;
    }
    value = lua_tointeger(L, index);
    if (value < 0) {
        luaL_error(L, "%s must be a non-negative integer", field);
        return 0u;
    }
    return (uint64_t)value;
}

uint64_t field_u64(lua_State *L, int table, const char *field, uint64_t fallback)
{
    uint64_t value;
    lua_getfield(L, table, field);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return fallback; }
    value = check_u64(L, -1, field);
    lua_pop(L, 1);
    return value;
}

int field_boolean(lua_State *L, int table, const char *field)
{
    int value;
    lua_getfield(L, table, field);
    value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

cdin_lua_handle *check_handle(lua_State *L, int index, cdin_lua_handle_kind kind)
{
    cdin_lua_handle *handle = (cdin_lua_handle *)luaL_checkudata(L, index, CDIN_LUA_HANDLE_METATABLE);
    if (handle->kind != kind || !handle->active) {
        luaL_error(L, "invalid or closed Backend handle");
        return NULL;
    }
    return handle;
}

cdin_lua_handle *push_handle(lua_State *L, cdin_host_handle value, cdin_lua_handle_kind kind)
{
    cdin_lua_handle *handle = (cdin_lua_handle *)lua_newuserdatauv(L, sizeof(*handle), 0);
    handle->value = value;
    handle->kind = kind;
    handle->active = 1;
    luaL_getmetatable(L, CDIN_LUA_HANDLE_METATABLE);
    lua_setmetatable(L, -2);
    return handle;
}

void push_version(lua_State *L, uint32_t major, uint32_t minor, uint32_t patch)
{
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)major); lua_setfield(L, -2, "major");
    lua_pushinteger(L, (lua_Integer)minor); lua_setfield(L, -2, "minor");
    lua_pushinteger(L, (lua_Integer)patch); lua_setfield(L, -2, "patch");
}

void push_read_result(lua_State *L, cdin_host_read_result *result, int include_total)
{
    lua_createtable(L, 0, include_total ? 4 : 2);
    lua_pushlstring(L, (const char *)result->bytes, result->length); lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, (lua_Integer)result->version); lua_setfield(L, -2, "version");
    if (include_total) {
        lua_pushinteger(L, (lua_Integer)result->total_length); lua_setfield(L, -2, "total_length");
        lua_pushinteger(L, (lua_Integer)result->flags); lua_setfield(L, -2, "flags");
    }
}

void push_edit_result(lua_State *L, cdin_host_edit_result *result)
{
    lua_createtable(L, 0, 10);
    lua_pushinteger(L, (lua_Integer)result->version); lua_setfield(L, -2, "version");
    lua_pushinteger(L, (lua_Integer)result->accepted_sequence); lua_setfield(L, -2, "accepted_seq");
    lua_pushinteger(L, (lua_Integer)result->start); lua_setfield(L, -2, "start");
    lua_pushinteger(L, (lua_Integer)result->removed); lua_setfield(L, -2, "removed");
    lua_pushinteger(L, (lua_Integer)result->inserted); lua_setfield(L, -2, "inserted");
    lua_pushinteger(L, (lua_Integer)result->new_length); lua_setfield(L, -2, "new_length");
    lua_pushinteger(L, (lua_Integer)result->edit_count); lua_setfield(L, -2, "edit_count");
    if (result->selection_blob != NULL) {
        lua_pushlstring(L, (const char *)result->selection_blob, result->selection_blob_length);
        lua_setfield(L, -2, "selection_blob");
    }
}

int get_optional_blob(lua_State *L, int table, const char *name,
                             const uint8_t **out_bytes, size_t *out_length)
{
    size_t length = 0u;
    const char *bytes;
    lua_getfield(L, table, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); *out_bytes = NULL; *out_length = 0u; return 0;
    }
    bytes = luaL_checklstring(L, -1, &length);
    *out_bytes = (const uint8_t *)bytes;
    *out_length = length;
    return 1; /* caller pops value */
}


size_t check_optional_size(lua_State *L, int index, lua_Integer fallback,
                                  size_t maximum, const char *field)
{
    lua_Integer value = lua_isnoneornil(L, index) ? fallback : luaL_checkinteger(L, index);
    if (value < 0 || (uint64_t)value > (uint64_t)maximum) {
        luaL_error(L, "%s is outside the supported range", field);
        return 0u;
    }
    return (size_t)value;
}


#endif
