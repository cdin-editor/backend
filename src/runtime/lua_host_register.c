/* lua-host-registration: isolated implementation of the optional Lua 5.4 bridge. */
#ifndef CDIN_WITH_LUA
typedef int lua_host_registration_disabled_translation_unit;
#else
#include "lua_host_internal.h"

static void set_closure(lua_State *L, cdin_lua_host *host, const char *name, lua_CFunction function)
{
    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, function, 1);
    lua_setfield(L, -2, name);
}

static void create_metatables(lua_State *L)
{
    if (luaL_newmetatable(L, CDIN_LUA_HANDLE_METATABLE)) {
        lua_pushliteral(L, "Backend-owned opaque handle"); lua_setfield(L, -2, "__name");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, CDIN_LUA_WRITER_METATABLE)) {
        lua_pushcfunction(L, lua_writer_gc); lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, CDIN_LUA_PROCESS_METATABLE)) {
        lua_createtable(L, 0, 5);
        lua_pushcfunction(L, lua_process_poll); lua_setfield(L, -2, "poll");
        lua_pushcfunction(L, lua_process_wait); lua_setfield(L, -2, "wait");
        lua_pushcfunction(L, lua_process_terminate); lua_setfield(L, -2, "terminate");
        lua_pushcfunction(L, lua_process_read_stdout); lua_setfield(L, -2, "read_stdout");
        lua_pushcfunction(L, lua_process_read_stderr); lua_setfield(L, -2, "read_stderr");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_process_gc); lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

static int lua_handle_gc(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *handle = (cdin_lua_handle *)luaL_checkudata(L, 1, CDIN_LUA_HANDLE_METATABLE);
    if (handle->active && host != NULL && host->backend != NULL && host->backend->kernel != NULL) {
        switch (handle->kind) {
            case CDIN_LUA_DOCUMENT: {
                uint64_t id = ((uint64_t)handle->value.generation << 32) | (uint64_t)handle->value.slot;
                cdin_backend_status session_status = host->backend->session != NULL
                    ? cdin_session_document_close(host->backend->session, id)
                    : CDIN_BACKEND_OK;
                if (session_status == CDIN_BACKEND_OK) {
                    (void)cdin_kernel_host_document_close(host->backend->kernel, handle->value);
                } else {
                    CDIN_BACKEND_WARN(host->backend->kernel, "RUNTIME",
                                      "document handle finalizer deferred document_id=%llu reason=views still reference the document",
                                      (unsigned long long)id);
                }
                break;
            }
            case CDIN_LUA_SNAPSHOT: (void)cdin_kernel_host_snapshot_release(host->backend->kernel, handle->value); break;
            case CDIN_LUA_TRANSACTION: (void)cdin_kernel_host_transaction_abort(host->backend->kernel, handle->value); break;
            case CDIN_LUA_SUBSCRIPTION: (void)cdin_kernel_host_unsubscribe(host->backend->kernel, handle->value); break;
            default: break;
        }
    }
    handle->active = 0;
    return 0;
}

void cdin_lua_register_host(cdin_lua_host *host)
{
    lua_State *L = host->state;
    create_metatables(L);
    luaL_newmetatable(L, CDIN_LUA_HANDLE_METATABLE);
    lua_pushlightuserdata(L, host); lua_pushcclosure(L, lua_handle_gc, 1);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    lua_createtable(L, 0, 5);
    set_closure(L, host, "info", lua_host_info);
    set_closure(L, host, "log", lua_host_log);
    lua_createtable(L, 0, 22);
    set_closure(L, host, "create", lua_doc_create);
    set_closure(L, host, "close", lua_doc_close);
    set_closure(L, host, "read", lua_doc_read);
    set_closure(L, host, "replace", lua_doc_replace);
    set_closure(L, host, "replace_batch", lua_doc_replace_batch);
    set_closure(L, host, "undo", lua_doc_undo);
    set_closure(L, host, "redo", lua_doc_redo);
    set_closure(L, host, "acquire_snapshot", lua_doc_acquire_snapshot);
    set_closure(L, host, "read_snapshot", lua_doc_read_snapshot);
    set_closure(L, host, "release_snapshot", lua_doc_release_snapshot);
    set_closure(L, host, "begin_transaction", lua_doc_begin_transaction);
    set_closure(L, host, "transaction_replace", lua_doc_transaction_replace);
    set_closure(L, host, "transaction_read", lua_doc_transaction_read);
    set_closure(L, host, "commit_transaction", lua_doc_commit_transaction);
    set_closure(L, host, "abort_transaction", lua_doc_abort_transaction);
    set_closure(L, host, "mark_clean", lua_doc_mark_clean);
    set_closure(L, host, "mark_clean_if_version", lua_doc_mark_clean_if_version);
    set_closure(L, host, "is_clean", lua_doc_is_clean);
    set_closure(L, host, "subscribe", lua_doc_subscribe);
    set_closure(L, host, "unsubscribe", lua_doc_unsubscribe);
    set_closure(L, host, "drain", lua_doc_drain);
    set_closure(L, host, "search", lua_doc_search);
    lua_setfield(L, -2, "documents");
    lua_createtable(L, 0, 1);
    set_closure(L, host, "publish", lua_ui_publish);
    lua_setfield(L, -2, "ui");
    lua_createtable(L, 0, 4);
    set_closure(L, host, "begin_atomic_write", lua_fs_begin_atomic_write);
    set_closure(L, host, "write_chunk", lua_fs_write_chunk);
    set_closure(L, host, "commit_atomic_write", lua_fs_commit_atomic_write);
    set_closure(L, host, "abort_atomic_write", lua_fs_abort_atomic_write);
    lua_setfield(L, -2, "fs");
    lua_createtable(L, 0, 2);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, lua_time_monotonic_ns); lua_setfield(L, -2, "monotonic_ns");
    lua_pushcfunction(L, lua_time_wall_seconds); lua_setfield(L, -2, "wall_seconds");
    lua_setfield(L, -2, "time");
    lua_createtable(L, 0, 1);
    set_closure(L, host, "spawn", lua_process_spawn);
    lua_setfield(L, -2, "process");
    lua_setfield(L, -2, "system");
    host->host_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}


#endif
