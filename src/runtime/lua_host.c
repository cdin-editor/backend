/*
 * Runtime Host bridge. BE-007 keeps Lua on the owner thread; workers return
 * plain C data and are delivered through cdin_backend_pump().
 *
 * ADR-0019 selects the PUC Lua 5.4 C API for the v1 private bridge. The bridge
 * remains behind these Backend-owned modules: extensions see only the
 * versioned Runtime Host API, never Kernel pointers or private C entry points.
 * Worker threads never enter this file or touch the Lua state.
 * https://github.com/cdin-editor/spec/blob/main/docs/adr/ADR-0019-runtime-bridge.md
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 */
#include "../backend_internal.h"
#include "../diagnostics/log.h"
#include "../os/fs.h"
#include "../os/process.h"
#include "../platform/clock.h"

#ifndef CDIN_WITH_LUA
struct cdin_lua_host { int unavailable; };
cdin_backend_status cdin_lua_host_start(cdin_backend *backend, cdin_lua_host **out_host)
{
    (void)backend;
    if (out_host != NULL) *out_host = NULL;
    return CDIN_BACKEND_UNSUPPORTED;
}
cdin_backend_status cdin_lua_host_pump(cdin_lua_host *host, size_t max_events, size_t *out_processed)
{
    (void)host; (void)max_events;
    if (out_processed != NULL) *out_processed = 0u;
    return CDIN_BACKEND_UNSUPPORTED;
}
cdin_backend_status cdin_lua_host_dispatch_frontend(cdin_lua_host *host, size_t max_events, size_t *out_processed)
{
    (void)host; (void)max_events;
    if (out_processed != NULL) *out_processed = 0u;
    return CDIN_BACKEND_UNSUPPORTED;
}
cdin_backend_status cdin_lua_host_publish_latest_ui(cdin_lua_host *host, cdin_protocol_client *client)
{
    (void)host; (void)client;
    return CDIN_BACKEND_UNSUPPORTED;
}
cdin_backend_status cdin_lua_host_shutdown(cdin_lua_host *host)
{
    (void)host;
    return CDIN_BACKEND_OK;
}
void cdin_lua_host_destroy(cdin_lua_host *host)
{
    (void)host;
}
#else
#include "lua_host_internal.h"

static int set_package_path(lua_State *L, const char *root)
{
    size_t length = strlen(root);
    size_t old_length = 0u;
    size_t need;
    char *path;
    int written;
    int package_index;
    const char *old_path;
    if (length > (SIZE_MAX - 128u) / 3u) return -1;
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return -1; }
    package_index = lua_absindex(L, -1);
    lua_getfield(L, package_index, "path");
    old_path = lua_tostring(L, -1);
    if (old_path != NULL) old_length = strlen(old_path);
    if (old_length > SIZE_MAX - (length * 3u) - 128u) {
        lua_pop(L, 2);
        return -1;
    }
    need = length * 3u + old_length + 128u;
    path = (char *)malloc(need);
    if (path == NULL) { lua_pop(L, 2); return -1; }
#ifdef _WIN32
    written = snprintf(path, need, "%s\\?.lua;%s\\?\\init.lua;%s\\?\\?.lua;%s",
                       root, root, root, old_path != NULL ? old_path : "");
#else
    written = snprintf(path, need, "%s/?.lua;%s/?/init.lua;%s/?/?.lua;%s",
                       root, root, root, old_path != NULL ? old_path : "");
#endif
    lua_pop(L, 1); /* Previous package.path. */
    if (written < 0 || (size_t)written >= need) {
        free(path);
        lua_pop(L, 1); /* package */
        return -1;
    }
    lua_pushstring(L, path);
    lua_setfield(L, package_index, "path");
    lua_pop(L, 1);
    free(path);
    return 0;
}

static int runtime_load_and_start(cdin_lua_host *host)
{
    lua_State *L = host->state;
    int error_index;
    lua_getglobal(L, "require");
    lua_pushstring(L, host->backend->runtime_entry);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) return -1;
    lua_getfield(L, -1, "new");
    lua_createtable(L, 0, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, host->host_ref);
    lua_setfield(L, -2, "host");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) return -1;
    lua_remove(L, -2); /* Runtime module */
    host->runtime_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, LUA_REGISTRYINDEX, host->runtime_ref);
    lua_getfield(L, -1, "start");
    lua_pushvalue(L, -2);
    error_index = lua_gettop(L) - 2;
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        (void)error_index;
        return -1;
    }
    lua_pop(L, 1);
    host->started = 1;
    return 0;
}

cdin_backend_status cdin_lua_host_start(cdin_backend *backend, cdin_lua_host **out_host)
{
    cdin_lua_host *host;
    if (backend == NULL || out_host == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_host = NULL;
    host = (cdin_lua_host *)calloc(1u, sizeof(*host));
    if (host == NULL) return CDIN_BACKEND_NO_MEMORY;
    host->backend = backend;
    host->host_ref = LUA_NOREF;
    host->runtime_ref = LUA_NOREF;
    host->latest_ui_ref = LUA_NOREF;
    host->state = luaL_newstate();
    if (host->state == NULL) { free(host); return CDIN_BACKEND_NO_MEMORY; }
    luaL_openlibs(host->state);
    if (set_package_path(host->state, backend->runtime_root) != 0) {
        lua_close(host->state); free(host); return CDIN_BACKEND_NO_MEMORY;
    }
    cdin_lua_register_host(host);
    if (runtime_load_and_start(host) != 0) {
        const char *message = lua_tostring(host->state, -1);
        CDIN_BACKEND_ERROR(backend->kernel, "RUNTIME", "Lua Runtime startup failed reason=%s",
                           message != NULL ? message : "no Lua error message was provided");
        if (host->runtime_ref != LUA_NOREF) luaL_unref(host->state, LUA_REGISTRYINDEX, host->runtime_ref);
        if (host->host_ref != LUA_NOREF) luaL_unref(host->state, LUA_REGISTRYINDEX, host->host_ref);
        lua_close(host->state); free(host);
        return CDIN_BACKEND_INVALID_STATE;
    }
    *out_host = host;
    CDIN_BACKEND_INFO(backend->kernel, "RUNTIME", "Lua state created and Runtime startup completed");
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_lua_host_pump(cdin_lua_host *host, size_t max_events, size_t *out_processed)
{
    lua_State *L;
    int base;
    size_t frontend_processed = 0u;
    size_t notification_processed = 0u;
    cdin_backend_status dispatch_status;
    if (host == NULL || out_processed == NULL || !host->started) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (!cdin_backend_is_owner(host->backend)) return CDIN_BACKEND_INVALID_STATE;
    *out_processed = 0u;
    dispatch_status = cdin_lua_host_dispatch_frontend(host, max_events, &frontend_processed);
    if (dispatch_status != CDIN_BACKEND_OK) return dispatch_status;
    L = host->state;
    base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, host->runtime_ref);
    lua_getfield(L, -1, "pump_notifications");
    lua_pushvalue(L, -2);
    lua_pushinteger(L, (lua_Integer)(max_events == 0u ? 64u : max_events));
    if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        CDIN_BACKEND_ERROR(host->backend->kernel, "RUNTIME", "notification pump failed reason=%s",
                           message != NULL ? message : "Runtime did not provide an error message");
        lua_settop(L, base);
        return CDIN_BACKEND_INTERNAL;
    }
    if (lua_isnumber(L, -2)) {
        lua_Integer count = lua_tointeger(L, -2);
        if (count > 0) notification_processed = (size_t)count;
    }
    lua_settop(L, base);
    *out_processed = frontend_processed + notification_processed;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_lua_host_shutdown(cdin_lua_host *host)
{
    lua_State *L;
    int base;
    if (host == NULL || !host->started) return CDIN_BACKEND_OK;
    if (!cdin_backend_is_owner(host->backend)) return CDIN_BACKEND_INVALID_STATE;
    L = host->state;
    base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, host->runtime_ref);
    lua_getfield(L, -1, "shutdown");
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        CDIN_BACKEND_ERROR(host->backend->kernel, "RUNTIME", "Runtime shutdown hook raised an error reason=%s",
                           message != NULL ? message : "Runtime did not provide an error message");
        lua_settop(L, base);
        host->started = 0;
        return CDIN_BACKEND_INTERNAL;
    }
    if (!lua_toboolean(L, -2)) {
        CDIN_BACKEND_WARN(host->backend->kernel, "RUNTIME",
                          "Runtime shutdown requested retry status=BUSY reason=shutdown hook returned false");
        lua_settop(L, base);
        host->started = 0;
        return CDIN_BACKEND_BUSY;
    }
    lua_settop(L, base);
    host->started = 0;
    CDIN_BACKEND_DEBUG(host->backend->kernel, "RUNTIME", "Runtime shutdown hook completed");
    return CDIN_BACKEND_OK;
}

void cdin_lua_host_destroy(cdin_lua_host *host)
{
    if (host == NULL) return;
    if (host->state != NULL) {
        if (host->latest_ui_ref != LUA_NOREF) luaL_unref(host->state, LUA_REGISTRYINDEX, host->latest_ui_ref);
        if (host->runtime_ref != LUA_NOREF) luaL_unref(host->state, LUA_REGISTRYINDEX, host->runtime_ref);
        if (host->host_ref != LUA_NOREF) luaL_unref(host->state, LUA_REGISTRYINDEX, host->host_ref);
        lua_close(host->state);
    }
    free(host);
}
#endif
