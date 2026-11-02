/* lua-host-api: isolated implementation of the optional Lua 5.4 bridge. */
#ifndef CDIN_WITH_LUA
typedef int lua_host_api_disabled_translation_unit;
#else
#include "lua_host_internal.h"

int lua_host_info(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    uint32_t major = 0u, minor = 0u, patch = 0u;
    cdin_kernel_host_abi_version(host->backend->kernel, &major, &minor, &patch);
    lua_createtable(L, 0, 7);
    push_version(L, CDIN_BACKEND_HOST_API_MAJOR, CDIN_BACKEND_HOST_API_MINOR, CDIN_BACKEND_HOST_API_PATCH);
    lua_setfield(L, -2, "host_api");
    push_version(L, major, minor, patch); lua_setfield(L, -2, "kernel_abi");
    push_version(L, 1u, 0u, 0u); lua_setfield(L, -2, "extension_contract");
    push_version(L, CDIN_BACKEND_PROTOCOL_MAJOR, CDIN_BACKEND_PROTOCOL_MINOR, CDIN_BACKEND_PROTOCOL_PATCH);
    lua_setfield(L, -2, "frontend_protocol");
    lua_createtable(L, 0, 10);
    lua_pushboolean(L, 1); lua_setfield(L, -2, "documents");
    lua_pushboolean(L, 1); lua_setfield(L, -2, "document_lines");
    lua_pushboolean(L, host->backend->config.enable_headless_protocol != 0);
    lua_setfield(L, -2, "frontend_events");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_NOTIFICATIONS)); lua_setfield(L, -2, "notifications");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_SNAPSHOTS)); lua_setfield(L, -2, "snapshots");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_TRANSACTIONS)); lua_setfield(L, -2, "transactions");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_BATCH)); lua_setfield(L, -2, "batch_edits");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_SEARCH_LITERAL)); lua_setfield(L, -2, "literal_search");
    lua_pushboolean(L, 1); lua_setfield(L, -2, "ui_model");
    lua_pushboolean(L, 1); lua_setfield(L, -2, "logging");
    lua_setfield(L, -2, "features");
    lua_createtable(L, 0, 7);
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_TRANSACTIONS)); lua_setfield(L, -2, "transactions");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_SNAPSHOTS)); lua_setfield(L, -2, "snapshots");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_NOTIFICATIONS)); lua_setfield(L, -2, "notifications");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_BATCH)); lua_setfield(L, -2, "batch");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_SEARCH_LITERAL)); lua_setfield(L, -2, "search_literal");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_SEARCH_BYTES)); lua_setfield(L, -2, "search_bytes");
    lua_pushboolean(L, cdin_kernel_host_has_feature(host->backend->kernel, CDIN_HOST_FEATURE_SEARCH_REGEX)); lua_setfield(L, -2, "search_regex");
    lua_setfield(L, -2, "kernel_features");
    lua_createtable(L, 0, 3);
    lua_pushliteral(L, "cdin-kernel"); lua_setfield(L, -2, "provider");
    lua_createtable(L, 6, 0);
    lua_pushliteral(L, "error"); lua_rawseti(L, -2, 1);
    lua_pushliteral(L, "warn"); lua_rawseti(L, -2, 2);
    lua_pushliteral(L, "info"); lua_rawseti(L, -2, 3);
    lua_pushliteral(L, "debug"); lua_rawseti(L, -2, 4);
    lua_pushliteral(L, "trace"); lua_rawseti(L, -2, 5);
    lua_pushliteral(L, "fatal"); lua_rawseti(L, -2, 6);
    lua_setfield(L, -2, "levels");
    lua_pushinteger(L, (lua_Integer)host->backend->config.max_log_message_bytes);
    lua_setfield(L, -2, "max_message_bytes");
    lua_setfield(L, -2, "logging");
    return 1;
}

static int log_level_from_name(const char *name, int *out_level, int *out_severity)
{
    static const struct { const char *name; int kernel_level; int severity; } levels[] = {
        {"error", CDIN_BACKEND_LOG_ERROR, 4}, {"warn", CDIN_BACKEND_LOG_WARN, 3},
        {"info", CDIN_BACKEND_LOG_INFO, 2}, {"debug", CDIN_BACKEND_LOG_DEBUG, 1},
        {"trace", CDIN_BACKEND_LOG_TRACE, 0}, {"fatal", CDIN_BACKEND_LOG_FATAL, 5}
    };
    size_t i;
    for (i = 0u; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(name, levels[i].name) == 0) {
            *out_level = levels[i].kernel_level;
            *out_severity = levels[i].severity;
            return 1;
        }
    }
    return 0;
}

static int configured_log_severity(int level)
{
    switch (level) {
        case CDIN_BACKEND_LOG_ERROR: return 4;
        case CDIN_BACKEND_LOG_WARN: return 3;
        case CDIN_BACKEND_LOG_INFO: return 2;
        case CDIN_BACKEND_LOG_DEBUG: return 1;
        case CDIN_BACKEND_LOG_TRACE: return 0;
        case CDIN_BACKEND_LOG_FATAL: return 5;
        default: return 0;
    }
}

int lua_host_log(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    const char *level_name;
    size_t message_length = 0u;
    const char *message;
    if (lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TSTRING)
        return raise_argument(L, "host.log level/message must be strings");
    level_name = lua_tolstring(L, 2, NULL);
    message = lua_tolstring(L, 3, &message_length);
    int kernel_level = 0, severity = 0;
    char *component = NULL;
    char *buffer = NULL;
    size_t capacity = (size_t)host->backend->config.max_log_message_bytes + 1u;
    size_t used = 0u;
    int have_component = 0;
    cdin_backend_status status;
    if (!log_level_from_name(level_name, &kernel_level, &severity)) return raise_argument(L, "host.log level");
    if (message_length > (size_t)host->backend->config.max_log_message_bytes) return raise_argument(L, "host.log message");
    buffer = (char *)malloc(capacity);
    if (buffer == NULL) return raise_status(L, CDIN_BACKEND_NO_MEMORY, "host.log", NULL);
    memcpy(buffer, message, message_length);
    used = message_length;
    if (lua_istable(L, 4)) {
        unsigned int fields = 0u;
        lua_pushnil(L);
        while (lua_next(L, 4) != 0) {
            size_t key_length = 0u, value_length = 0u;
            const char *key = lua_type(L, -2) == LUA_TSTRING ?
                              lua_tolstring(L, -2, &key_length) : NULL;
            const char *value = NULL;
            char number[64];
            size_t available;
            if (key != NULL && fields < CDIN_LUA_MAX_LOG_FIELDS) {
                if (lua_type(L, -1) == LUA_TSTRING) value = lua_tolstring(L, -1, &value_length);
                else if (lua_type(L, -1) == LUA_TBOOLEAN) {
                    value = lua_toboolean(L, -1) ? "true" : "false";
                    value_length = strlen(value);
                } else if (lua_isnumber(L, -1)) {
                    int n = snprintf(number, sizeof(number), "%.17g", (double)lua_tonumber(L, -1));
                    if (n > 0) { value = number; value_length = (size_t)n; }
                }
                if (value != NULL && key_length != 0u && value_length <= CDIN_LUA_MAX_FIELD_BYTES) {
                    if (key_length == 9u && memcmp(key, "component", 9u) == 0 && lua_type(L, -1) == LUA_TSTRING) {
                        component = (char *)malloc(value_length + 1u);
                        if (component != NULL) { memcpy(component, value, value_length); component[value_length] = '\0'; have_component = 1; }
                    } else {
                        available = capacity > used ? capacity - used - 1u : 0u;
                        if (available > 0u) {
                            int n = snprintf(buffer + used, available + 1u, " %.*s=%.*s",
                                             (int)CDIN_LUA_MAX_FIELD_BYTES, key,
                                             (int)CDIN_LUA_MAX_FIELD_BYTES, value);
                            if (n > 0) used += (size_t)n < available ? (size_t)n : available;
                        }
                    }
                    fields++;
                }
            }
            lua_pop(L, 1);
        }
    }
    if (severity < configured_log_severity(host->backend->config.log_level)) {
        free(component); free(buffer); lua_pushboolean(L, 0); return 1;
    }
    status = cdin_kernel_host_log(host->backend->kernel, kernel_level,
                                  have_component ? component : "cdin-runtime", buffer);
    free(component);
    free(buffer);
    if (status == CDIN_BACKEND_UNSUPPORTED) { lua_pushboolean(L, 0); return 1; }
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "host.log", NULL);
    lua_pushboolean(L, 1);
    return 1;
}



#endif
