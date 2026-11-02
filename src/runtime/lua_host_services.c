/* lua-host-services: isolated implementation of the optional Lua 5.4 bridge. */
#ifndef CDIN_WITH_LUA
typedef int lua_host_services_disabled_translation_unit;
#else
#include "lua_host_internal.h"

int lua_fs_begin_atomic_write(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    const char *path = luaL_checkstring(L, 2);
    int overwrite = 1;
    cdin_lua_writer *writer;
    cdin_atomic_writer *value = NULL;
    if (lua_istable(L, 3)) overwrite = field_boolean(L, 3, "overwrite");
    if (cdin_fs_begin_atomic_write(path, host->backend->config.max_document_bytes, overwrite, &value) != 0)
        return raise_io(L, "fs.begin_atomic_write");
    writer = (cdin_lua_writer *)lua_newuserdatauv(L, sizeof(*writer), 0);
    writer->value = value; writer->active = 1;
    luaL_getmetatable(L, CDIN_LUA_WRITER_METATABLE); lua_setmetatable(L, -2);
    return 1;
}
static cdin_lua_writer *check_writer(lua_State *L, int index)
{
    cdin_lua_writer *writer = (cdin_lua_writer *)luaL_checkudata(L, index, CDIN_LUA_WRITER_METATABLE);
    if (!writer->active || writer->value == NULL) luaL_error(L, "atomic writer is closed");
    return writer;
}
int lua_fs_write_chunk(lua_State *L)
{
    cdin_lua_writer *writer = check_writer(L, 2);
    size_t length;
    const char *bytes = luaL_checklstring(L, 3, &length);
    if (cdin_fs_write_chunk(writer->value, bytes, length) != 0) return raise_io(L, "fs.write_chunk");
    lua_pushboolean(L, 1); return 1;
}
int lua_fs_commit_atomic_write(lua_State *L)
{
    cdin_lua_writer *writer = check_writer(L, 2);
    cdin_atomic_writer *value = writer->value;
    writer->active = 0; writer->value = NULL;
    if (cdin_fs_commit_atomic_write(value) != 0) return raise_io(L, "fs.commit_atomic_write");
    lua_pushboolean(L, 1); return 1;
}
int lua_fs_abort_atomic_write(lua_State *L)
{
    cdin_lua_writer *writer = (cdin_lua_writer *)luaL_checkudata(L, 2, CDIN_LUA_WRITER_METATABLE);
    if (writer->active && writer->value != NULL) cdin_fs_abort_atomic_write(writer->value);
    writer->active = 0; writer->value = NULL;
    lua_pushboolean(L, 1); return 1;
}
int lua_writer_gc(lua_State *L)
{
    cdin_lua_writer *writer = (cdin_lua_writer *)luaL_checkudata(L, 1, CDIN_LUA_WRITER_METATABLE);
    if (writer->active && writer->value != NULL) cdin_fs_abort_atomic_write(writer->value);
    writer->active = 0; writer->value = NULL;
    return 0;
}

int lua_time_monotonic_ns(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)cdin_clock_monotonic_ns());
    return 1;
}
int lua_time_wall_seconds(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)cdin_clock_wall_seconds());
    return 1;
}

int lua_process_spawn(lua_State *L)
{
    char **owned_argv = NULL;
    const char **argv = NULL;
    size_t count, i;
    cdin_process_options options;
    cdin_process *process = NULL;
    cdin_lua_process *ud;
    int status;
    memset(&options, 0, sizeof(options));
    options.inherit_environment = 1;
    luaL_checktype(L, 2, LUA_TTABLE);
    count = lua_rawlen(L, 2);
    if (count == 0u || count > 256u || count > SIZE_MAX / sizeof(*argv)) return raise_argument(L, "process.spawn argv");
    argv = (const char **)calloc(count + 1u, sizeof(*argv));
    owned_argv = (char **)calloc(count, sizeof(*owned_argv));
    if (argv == NULL || owned_argv == NULL) {
        free((void *)argv); free(owned_argv);
        return raise_status(L, CDIN_BACKEND_NO_MEMORY, "process.spawn", NULL);
    }
    for (i = 0u; i < count; i++) {
        size_t length;
        const char *value;
        lua_rawgeti(L, 2, (lua_Integer)(i + 1u));
        value = luaL_checklstring(L, -1, &length);
        if (memchr(value, '\0', length) != NULL) {
            lua_pop(L, 1);
            for (size_t j = 0u; j < count; j++) free(owned_argv[j]);
            free(owned_argv); free((void *)argv);
            return raise_argument(L, "process.spawn argv");
        }
        owned_argv[i] = (char *)malloc(length + 1u);
        if (owned_argv[i] == NULL) {
            lua_pop(L, 1);
            for (size_t j = 0u; j < count; j++) free(owned_argv[j]);
            free(owned_argv); free((void *)argv);
            return raise_status(L, CDIN_BACKEND_NO_MEMORY, "process.spawn", NULL);
        }
        memcpy(owned_argv[i], value, length);
        owned_argv[i][length] = '\0';
        argv[i] = owned_argv[i];
        lua_pop(L, 1);
    }
    if (lua_istable(L, 3)) {
        options.capture_stdout = field_boolean(L, 3, "capture_stdout");
        options.capture_stderr = field_boolean(L, 3, "capture_stderr");
        lua_getfield(L, 3, "cwd");
        if (!lua_isnil(L, -1)) options.working_directory = luaL_checkstring(L, -1);
        status = cdin_process_spawn(argv, &options, &process);
        lua_pop(L, 1);
    } else {
        status = cdin_process_spawn(argv, &options, &process);
    }
    for (i = 0u; i < count; i++) free(owned_argv[i]);
    free(owned_argv);
    free((void *)argv);
    if (status != 0) return raise_io(L, "process.spawn");
    ud = (cdin_lua_process *)lua_newuserdatauv(L, sizeof(*ud), 0);
    ud->value = process; ud->active = 1;
    luaL_getmetatable(L, CDIN_LUA_PROCESS_METATABLE); lua_setmetatable(L, -2);
    return 1;
}

static cdin_lua_process *check_process(lua_State *L, int index)
{
    cdin_lua_process *process = (cdin_lua_process *)luaL_checkudata(L, index, CDIN_LUA_PROCESS_METATABLE);
    if (!process->active || process->value == NULL) luaL_error(L, "process handle is closed");
    return process;
}
int lua_process_poll(lua_State *L)
{
    cdin_lua_process *process = check_process(L, 1);
    int exit_code = 0;
    int state = cdin_process_poll(process->value, &exit_code);
    if (state < 0) return raise_io(L, "process.poll");
    lua_createtable(L, 0, 2);
    lua_pushstring(L, state == 1 ? "exited" : "running"); lua_setfield(L, -2, "state");
    if (state == 1) { lua_pushinteger(L, (lua_Integer)exit_code); lua_setfield(L, -2, "exit_code"); }
    return 1;
}
int lua_process_wait(lua_State *L)
{
    cdin_lua_process *process = check_process(L, 1);
    uint32_t timeout = (uint32_t)check_optional_size(L, 2, 0, UINT32_MAX, "timeout_ms");
    int exit_code = 0;
    int state = cdin_process_wait(process->value, timeout, &exit_code);
    if (state < 0) return raise_io(L, "process.wait");
    if (state == 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)exit_code); return 1;
}
int lua_process_terminate(lua_State *L)
{
    cdin_lua_process *process = check_process(L, 1);
    if (cdin_process_terminate(process->value) != 0) return raise_io(L, "process.terminate");
    lua_pushboolean(L, 1); return 1;
}
static int lua_process_read(lua_State *L, int is_stderr)
{
    cdin_lua_process *process = check_process(L, 1);
    size_t capacity = check_optional_size(L, 2, 65536, 1024u * 1024u, "process read size");
    size_t read_count = 0u;
    uint8_t *buffer;
    int result;
    if (capacity == 0u || capacity > 1024u * 1024u) return raise_argument(L, "process.read size");
    buffer = (uint8_t *)malloc(capacity);
    if (buffer == NULL) return raise_status(L, CDIN_BACKEND_NO_MEMORY, "process.read", NULL);
    result = is_stderr ? cdin_process_read_stderr(process->value, buffer, capacity, &read_count)
                       : cdin_process_read_stdout(process->value, buffer, capacity, &read_count);
    if (result != 0) { free(buffer); lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char *)buffer, read_count);
    free(buffer);
    return 1;
}
int lua_process_read_stdout(lua_State *L) { return lua_process_read(L, 0); }
int lua_process_read_stderr(lua_State *L) { return lua_process_read(L, 1); }
int lua_process_gc(lua_State *L)
{
    cdin_lua_process *process = (cdin_lua_process *)luaL_checkudata(L, 1, CDIN_LUA_PROCESS_METATABLE);
    if (process->active && process->value != NULL) cdin_process_destroy(process->value);
    process->active = 0; process->value = NULL;
    return 0;
}


#endif
