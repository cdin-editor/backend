/*
 * Private declarations shared by the small Lua bridge modules.
 * This header is not installed: it contains private details of the accepted
 * PUC Lua 5.4 bridge (Spec ADR-0019), not part of the public Backend API.
 */
#ifndef CDIN_BACKEND_RUNTIME_LUA_HOST_INTERNAL_H
#define CDIN_BACKEND_RUNTIME_LUA_HOST_INTERNAL_H

#ifndef CDIN_WITH_LUA
#error "lua_host_internal.h is only available when CDIN_WITH_LUA is enabled"
#endif

#include "../backend_internal.h"
#include "../diagnostics/log.h"
#include "../os/fs.h"
#include "../os/process.h"
#include "../platform/clock.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if LUA_VERSION_NUM < 504
#error "CDIN Backend requires Lua 5.4"
#endif

#define CDIN_LUA_HANDLE_METATABLE "cdin.backend.handle"
#define CDIN_LUA_WRITER_METATABLE "cdin.backend.atomic_writer"
#define CDIN_LUA_PROCESS_METATABLE "cdin.backend.process"
#define CDIN_LUA_MAX_LOG_FIELDS 16u
#define CDIN_LUA_MAX_FIELD_BYTES 256u
#define CDIN_LUA_MAX_LOG_BUFFER (1024u * 1024u + 1u)

typedef enum cdin_lua_handle_kind {
    CDIN_LUA_DOCUMENT = 1,
    CDIN_LUA_SNAPSHOT,
    CDIN_LUA_TRANSACTION,
    CDIN_LUA_SUBSCRIPTION
} cdin_lua_handle_kind;

typedef struct cdin_lua_handle {
    cdin_host_handle value;
    cdin_lua_handle_kind kind;
    int active;
} cdin_lua_handle;

typedef struct cdin_lua_writer {
    cdin_atomic_writer *value;
    int active;
} cdin_lua_writer;

typedef struct cdin_lua_process {
    cdin_process *value;
    int active;
} cdin_lua_process;

struct cdin_lua_host {
    cdin_backend *backend;
    lua_State *state;
    int host_ref;
    int runtime_ref;
    int latest_ui_ref;
    size_t next_protocol_client;
    int started;
    /* Optional Backend-owned extension resolver state. Disabled unless an
     * explicit extension_source_root was configured by the application. */
    char *extension_root_path;
#ifdef _WIN32
    wchar_t *extension_root_wide;
    void *extension_root_handle;
#else
    int extension_root_fd;
#endif
    uint64_t extension_source_limit;
    uint64_t extension_source_total_limit;
    uint64_t extension_source_total;
    uint32_t extension_module_limit;
    uint32_t extension_module_count;
};

typedef struct cdin_lua_error_details {
    int has_expected;
    uint64_t expected;
    int has_current;
    uint64_t current;
    int has_requested;
    uint64_t requested;
    int has_length;
    uint64_t length;
    int has_limit;
    uint32_t limit;
    int has_required;
    uint64_t required;
} cdin_lua_error_details;

/* Shared validation, error translation and Lua value construction. */
void lua_error_details_from_host(cdin_lua_error_details *target,
                                 const cdin_host_error_details *source,
                                 cdin_backend_status status);
int raise_status(lua_State *L, cdin_backend_status status, const char *operation,
                 const cdin_lua_error_details *details);
int raise_argument(lua_State *L, const char *operation);
int raise_io(lua_State *L, const char *operation);
uint64_t check_u64(lua_State *L, int index, const char *field);
uint64_t field_u64(lua_State *L, int table, const char *field, uint64_t fallback);
size_t check_optional_size(lua_State *L, int index, lua_Integer fallback,
                           uint64_t maximum, const char *field);
int field_boolean(lua_State *L, int table, const char *field);
cdin_lua_handle *check_handle(lua_State *L, int index, cdin_lua_handle_kind kind);
cdin_lua_handle *push_handle(lua_State *L, cdin_host_handle value, cdin_lua_handle_kind kind);
void push_version(lua_State *L, uint32_t major, uint32_t minor, uint32_t patch);
void push_read_result(lua_State *L, cdin_host_read_result *result, int include_total);
void push_edit_result(lua_State *L, cdin_host_edit_result *result);
int get_optional_blob(lua_State *L, int table, const char *name,
                      const uint8_t **out_bytes, size_t *out_length);

/* Host-level callbacks. Registration lives in lua_host_register.c. */
int lua_host_info(lua_State *L);
int lua_host_log(lua_State *L);
int lua_ui_publish(lua_State *L);
cdin_backend_status cdin_lua_host_dispatch_frontend(cdin_lua_host *host, size_t max_events, size_t *out_processed);
cdin_backend_status cdin_lua_host_publish_latest_ui(cdin_lua_host *host, cdin_protocol_client *client);

/* Kernel document and history callbacks. */
int lua_doc_create(lua_State *L);
int lua_doc_close(lua_State *L);
int lua_doc_read(lua_State *L);
int lua_doc_read_lines(lua_State *L);
int lua_doc_replace(lua_State *L);
int lua_doc_undo(lua_State *L);
int lua_doc_redo(lua_State *L);
int lua_doc_replace_batch(lua_State *L);
int lua_doc_acquire_snapshot(lua_State *L);
int lua_doc_read_snapshot(lua_State *L);
int lua_doc_release_snapshot(lua_State *L);
int lua_doc_begin_transaction(lua_State *L);
int lua_doc_transaction_replace(lua_State *L);
int lua_doc_transaction_read(lua_State *L);
int lua_doc_commit_transaction(lua_State *L);
int lua_doc_abort_transaction(lua_State *L);
int lua_doc_mark_clean(lua_State *L);
int lua_doc_mark_clean_if_version(lua_State *L);
int lua_doc_is_clean(lua_State *L);
int lua_doc_subscribe(lua_State *L);
int lua_doc_unsubscribe(lua_State *L);
int lua_doc_drain(lua_State *L);
int lua_doc_search(lua_State *L);

/* Reusable OS-service callbacks and userdata finalizers. */
int lua_fs_read_file(lua_State *L);
int lua_fs_begin_atomic_write(lua_State *L);
int lua_fs_write_chunk(lua_State *L);
int lua_fs_commit_atomic_write(lua_State *L);
int lua_fs_abort_atomic_write(lua_State *L);
int lua_time_monotonic_ns(lua_State *L);
int lua_time_wall_seconds(lua_State *L);
int lua_process_spawn(lua_State *L);
int lua_process_poll(lua_State *L);
int lua_process_wait(lua_State *L);
int lua_process_terminate(lua_State *L);
int lua_process_read_stdout(lua_State *L);
int lua_process_read_stderr(lua_State *L);
int lua_writer_gc(lua_State *L);
int lua_process_gc(lua_State *L);

void cdin_lua_register_host(cdin_lua_host *host);
cdin_backend_status cdin_lua_host_extension_resolver_init(cdin_lua_host *host);
void cdin_lua_host_extension_resolver_destroy(cdin_lua_host *host);
int lua_extension_module_resolver(lua_State *L);

#endif
