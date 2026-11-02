/* lua-host-documents: isolated implementation of the optional Lua 5.4 bridge. */
#ifndef CDIN_WITH_LUA
typedef int lua_host_documents_disabled_translation_unit;
#else
#include "lua_host_internal.h"

int lua_doc_create(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    size_t length;
    const char *bytes = luaL_checklstring(L, 2, &length);
    cdin_host_handle handle;
    cdin_host_doc_info info;
    uint64_t session_doc_id;
    cdin_backend_status status = cdin_kernel_host_document_create(host->backend->kernel,
        (const uint8_t *)bytes, length, &handle, &info);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.create", NULL);
    session_doc_id = ((uint64_t)handle.generation << 32) | (uint64_t)handle.slot;
    status = cdin_session_document_open(host->backend->session, session_doc_id, info.version, info.length);
    if (status != CDIN_BACKEND_OK) {
        (void)cdin_kernel_host_document_close(host->backend->kernel, handle);
        return raise_status(L, status, "session.document_open", NULL);
    }
    lua_createtable(L, 0, 3);
    (void)push_handle(L, handle, CDIN_LUA_DOCUMENT); lua_setfield(L, -2, "handle");
    lua_pushinteger(L, (lua_Integer)info.version); lua_setfield(L, -2, "version");
    lua_pushinteger(L, (lua_Integer)info.length); lua_setfield(L, -2, "length");
    return 1;
}

int lua_doc_close(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *handle = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    uint64_t session_id = ((uint64_t)handle->value.generation << 32) | (uint64_t)handle->value.slot;
    cdin_host_doc_info info;
    cdin_backend_status status;
    status = cdin_kernel_host_document_info(host->backend->kernel, handle->value, &info);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.close", NULL);
    status = cdin_session_document_close(host->backend->session, session_id);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.close", NULL);
    status = cdin_kernel_host_document_close(host->backend->kernel, handle->value);
    if (status != CDIN_BACKEND_OK) {
        (void)cdin_session_document_open(host->backend->session, session_id, info.version, info.length);
        return raise_status(L, status, "documents.close", NULL);
    }
    handle->active = 0;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_doc_read(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *handle = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_read_result result;
    uint64_t start, length, expected = 0u;
    int has_expected;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    start = field_u64(L, 3, "start", 0u);
    length = field_u64(L, 3, "length", 0u);
    lua_getfield(L, 3, "expected_version");
    has_expected = !lua_isnil(L, -1);
    if (has_expected) expected = check_u64(L, -1, "expected_version");
    lua_pop(L, 1);
    status = cdin_kernel_host_document_read(host->backend->kernel, handle->value,
        start, length, has_expected, expected, &result);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        memset(&details, 0, sizeof(details));
        if (status == CDIN_BACKEND_STALE_VERSION) {
            cdin_host_doc_info current;
            details.has_expected = has_expected; details.expected = expected;
            if (cdin_kernel_host_document_info(host->backend->kernel, handle->value, &current) == CDIN_BACKEND_OK) {
                details.has_current = 1; details.current = current.version;
            }
        }
        return raise_status(L, status, "documents.read", &details);
    }
    push_read_result(L, &result, 1);
    cdin_kernel_host_read_result_free(&result);
    return 1;
}

int lua_doc_read_lines(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *handle = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_lines_result result;
    uint64_t first_line;
    uint64_t requested_count;
    uint64_t expected;
    uint64_t max_bytes_value;
    uint64_t frame_limit;
    uint64_t max_line_count;
    uint64_t framing_reserve;
    size_t index;
    size_t max_bytes;
    static const char empty_line = '\0';
    cdin_backend_status status;

    luaL_checktype(L, 3, LUA_TTABLE);
    first_line = field_u64(L, 3, "first_line", 0u);
    requested_count = field_u64(L, 3, "line_count", 0u);
    expected = field_u64(L, 3, "expected_version", UINT64_MAX);
    frame_limit = host->backend->config.max_message_bytes;

    /* Reserve the fixed view.lines envelope separately from every line item.
     * A line map has a text key and byte-string header even when bytes are empty.
     * The 12-byte per-line bound covers the largest CBOR length header for the
     * configured 1 MiB frame limit; 256 bytes covers map keys and a 128-byte id.
     * Limiting both count and content prevents many empty lines from overflowing
     * the final CBOR frame after the Kernel has already produced the response. */
    if (frame_limit <= 256u) {
        return raise_status(L, CDIN_BACKEND_LIMIT_EXCEEDED, "documents.read_lines", NULL);
    }
    max_line_count = (frame_limit - 256u) / 24u;
    if (max_line_count == 0u) {
        return raise_status(L, CDIN_BACKEND_LIMIT_EXCEEDED, "documents.read_lines", NULL);
    }
    if (requested_count == 0u || requested_count > 4096u) {
        return raise_status(L, CDIN_BACKEND_LIMIT_EXCEEDED, "documents.read_lines", NULL);
    }
    if (requested_count > max_line_count) requested_count = max_line_count;
    framing_reserve = 256u + requested_count * 12u;
    max_bytes_value = field_u64(L, 3, "max_bytes", frame_limit - framing_reserve);
    if (max_bytes_value == 0u || max_bytes_value > frame_limit - framing_reserve ||
        max_bytes_value > SIZE_MAX)
        return raise_status(L, CDIN_BACKEND_LIMIT_EXCEEDED, "documents.read_lines", NULL);
    max_bytes = (size_t)max_bytes_value;

    status = cdin_kernel_host_document_read_lines(host->backend->kernel, handle->value,
        first_line, (size_t)requested_count, expected, max_bytes, &result);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        memset(&details, 0, sizeof(details));
        if (status == CDIN_BACKEND_STALE_VERSION) {
            details.has_expected = 1; details.expected = result.details.expected;
            details.has_current = 1; details.current = result.details.current;
        } else if (status == CDIN_BACKEND_OUT_OF_RANGE) {
            details.has_requested = 1; details.requested = result.details.requested;
            details.has_length = 1; details.length = result.details.length;
        } else if (status == CDIN_BACKEND_LIMIT_EXCEEDED && result.details.required != 0u) {
            details.has_required = 1; details.required = result.details.required;
        }
        cdin_kernel_host_lines_result_free(&result);
        return raise_status(L, status, "documents.read_lines", &details);
    }

    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)result.version); lua_setfield(L, -2, "version");
    lua_pushinteger(L, (lua_Integer)first_line); lua_setfield(L, -2, "first_line");
    lua_pushinteger(L, (lua_Integer)result.total_lines); lua_setfield(L, -2, "total_lines");
    lua_createtable(L, (int)result.count, 0);
    for (index = 0u; index < result.count; index++) {
        const char *bytes = result.lines[index].length == 0u
            ? &empty_line : (const char *)result.lines[index].bytes;
        lua_pushlstring(L, bytes, result.lines[index].length);
        lua_rawseti(L, -2, (lua_Integer)(index + 1u));
    }
    lua_setfield(L, -2, "lines");
    cdin_kernel_host_lines_result_free(&result);
    return 1;
}

int lua_doc_replace(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *handle = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_edit_result result;
    const char *bytes;
    size_t length, before_length = 0u, after_length = 0u;
    const uint8_t *before = NULL, *after = NULL;
    uint64_t expected, start, finish, coalesce;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    expected = field_u64(L, 3, "expected_version", UINT64_MAX);
    start = field_u64(L, 3, "start", 0u);
    finish = field_u64(L, 3, "finish", 0u);
    coalesce = field_u64(L, 3, "coalesce_key", 0u);
    lua_getfield(L, 3, "bytes"); bytes = luaL_checklstring(L, -1, &length);
    (void)get_optional_blob(L, 3, "undo_blob_before", &before, &before_length); if (before != NULL || before_length != 0u) lua_pop(L, 1);
    (void)get_optional_blob(L, 3, "undo_blob_after", &after, &after_length); if (after != NULL || after_length != 0u) lua_pop(L, 1);
    status = cdin_kernel_host_document_replace(host->backend->kernel, handle->value,
        expected, start, finish, (const uint8_t *)bytes, length, coalesce,
        before, before_length, after, after_length, &result);
    lua_pop(L, 1); /* bytes */
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        lua_error_details_from_host(&details, &result.details, status);
        if (status == CDIN_BACKEND_STALE_VERSION && !details.has_expected) {
            cdin_host_doc_info current;
            details.has_expected = expected != UINT64_MAX; details.expected = expected;
            if (cdin_kernel_host_document_info(host->backend->kernel, handle->value, &current) == CDIN_BACKEND_OK) {
                details.has_current = 1; details.current = current.version;
            }
        }
        return raise_status(L, status, "documents.replace", &details);
    }
    push_edit_result(L, &result);
    cdin_kernel_host_edit_result_free(&result);
    return 1;
}

static int lua_doc_history(lua_State *L, int redo)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *handle = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_edit_result result;
    uint64_t expected;
    size_t blob_capacity;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    expected = field_u64(L, 3, "expected_version", UINT64_MAX);
    blob_capacity = (size_t)field_u64(L, 3, "blob_capacity",
        (uint64_t)cdin_kernel_host_default_blob_capacity());
    status = cdin_kernel_host_document_history(host->backend->kernel, handle->value,
        expected, blob_capacity, redo, &result);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        lua_error_details_from_host(&details, &result.details, status);
        return raise_status(L, status, redo ? "documents.redo" : "documents.undo", &details);
    }
    push_edit_result(L, &result);
    cdin_kernel_host_edit_result_free(&result);
    return 1;
}
int lua_doc_undo(lua_State *L) { return lua_doc_history(L, 0); }
int lua_doc_redo(lua_State *L) { return lua_doc_history(L, 1); }

int lua_doc_replace_batch(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *handle = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_edit_result result;
    cdin_host_edit *edits = NULL;
    uint8_t **owned_bytes = NULL;
    size_t count, i, before_length = 0u, after_length = 0u;
    const uint8_t *before = NULL, *after = NULL;
    uint64_t expected;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    expected = field_u64(L, 3, "expected_version", UINT64_MAX);
    lua_getfield(L, 3, "edits");
    luaL_checktype(L, -1, LUA_TTABLE);
    count = lua_rawlen(L, -1);
    if (count == 0u || count > cdin_kernel_host_max_batch_edits() ||
        count > SIZE_MAX / sizeof(*edits)) {
        lua_pop(L, 1);
        return raise_status(L, CDIN_BACKEND_LIMIT_EXCEEDED, "documents.replace_batch", NULL);
    }
    edits = (cdin_host_edit *)calloc(count, sizeof(*edits));
    owned_bytes = (uint8_t **)calloc(count, sizeof(*owned_bytes));
    if (edits == NULL || owned_bytes == NULL) {
        free(edits); free(owned_bytes); lua_pop(L, 1);
        return raise_status(L, CDIN_BACKEND_NO_MEMORY, "documents.replace_batch", NULL);
    }
    for (i = 0u; i < count; i++) {
        size_t length;
        const char *bytes;
        lua_rawgeti(L, -1, (lua_Integer)(i + 1u));
        luaL_checktype(L, -1, LUA_TTABLE);
        edits[i].start = field_u64(L, -1, "start", 0u);
        edits[i].finish = field_u64(L, -1, "finish", 0u);
        lua_getfield(L, -1, "bytes");
        bytes = luaL_checklstring(L, -1, &length);
        if (length != 0u) {
            owned_bytes[i] = (uint8_t *)malloc(length);
            if (owned_bytes[i] == NULL) {
                lua_pop(L, 3);
                for (size_t j = 0u; j < count; j++) free(owned_bytes[j]);
                free(owned_bytes); free(edits);
                return raise_status(L, CDIN_BACKEND_NO_MEMORY, "documents.replace_batch", NULL);
            }
            memcpy(owned_bytes[i], bytes, length);
        }
        edits[i].bytes = owned_bytes[i];
        edits[i].length = length;
        lua_pop(L, 2); /* bytes and edit */
    }
    lua_pop(L, 1); /* edits */
    (void)get_optional_blob(L, 3, "undo_blob_before", &before, &before_length);
    if (before != NULL) lua_pop(L, 1);
    (void)get_optional_blob(L, 3, "undo_blob_after", &after, &after_length);
    if (after != NULL) lua_pop(L, 1);
    status = cdin_kernel_host_document_replace_batch(host->backend->kernel, handle->value,
        expected, edits, count, before, before_length, after, after_length, &result);
    for (i = 0u; i < count; i++) free(owned_bytes[i]);
    free(owned_bytes);
    free(edits);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        lua_error_details_from_host(&details, &result.details, status);
        return raise_status(L, status, "documents.replace_batch", &details);
    }
    push_edit_result(L, &result);
    cdin_kernel_host_edit_result_free(&result);
    return 1;
}

int lua_doc_acquire_snapshot(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *document = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_handle snapshot;
    cdin_host_doc_info info;
    cdin_backend_status status = cdin_kernel_host_snapshot_acquire(host->backend->kernel,
        document->value, &snapshot, &info);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.acquire_snapshot", NULL);
    lua_createtable(L, 0, 3);
    (void)push_handle(L, snapshot, CDIN_LUA_SNAPSHOT); lua_setfield(L, -2, "handle");
    lua_pushinteger(L, (lua_Integer)info.version); lua_setfield(L, -2, "version");
    lua_pushinteger(L, (lua_Integer)info.length); lua_setfield(L, -2, "length");
    return 1;
}

int lua_doc_read_snapshot(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *snapshot = check_handle(L, 2, CDIN_LUA_SNAPSHOT);
    cdin_host_read_result result;
    uint64_t start, length;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    start = field_u64(L, 3, "start", 0u);
    length = field_u64(L, 3, "length", 0u);
    status = cdin_kernel_host_snapshot_read(host->backend->kernel, snapshot->value, start, length, &result);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        lua_error_details_from_host(&details, &result.details, status);
        return raise_status(L, status, "documents.read_snapshot", &details);
    }
    push_read_result(L, &result, 0);
    cdin_kernel_host_read_result_free(&result);
    return 1;
}

int lua_doc_release_snapshot(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *snapshot = check_handle(L, 2, CDIN_LUA_SNAPSHOT);
    cdin_backend_status status = cdin_kernel_host_snapshot_release(host->backend->kernel, snapshot->value);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.release_snapshot", NULL);
    snapshot->active = 0;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_doc_begin_transaction(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *document = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_handle transaction;
    uint64_t base_version;
    uint64_t expected;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    expected = field_u64(L, 3, "expected_version", UINT64_MAX);
    status = cdin_kernel_host_transaction_begin(host->backend->kernel, document->value,
        expected, &transaction, &base_version);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        memset(&details, 0, sizeof(details));
        if (status == CDIN_BACKEND_STALE_VERSION) {
            details.has_expected = 1; details.expected = expected;
            details.has_current = 1; details.current = base_version;
        }
        return raise_status(L, status, "documents.begin_transaction", &details);
    }
    lua_createtable(L, 0, 2);
    (void)push_handle(L, transaction, CDIN_LUA_TRANSACTION); lua_setfield(L, -2, "handle");
    lua_pushinteger(L, (lua_Integer)base_version); lua_setfield(L, -2, "base_version");
    return 1;
}

int lua_doc_transaction_replace(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *transaction = check_handle(L, 2, CDIN_LUA_TRANSACTION);
    size_t length;
    const char *bytes;
    uint64_t start, finish;
    cdin_backend_status status;
    cdin_host_error_details host_details;
    luaL_checktype(L, 3, LUA_TTABLE);
    start = field_u64(L, 3, "start", 0u);
    finish = field_u64(L, 3, "finish", 0u);
    lua_getfield(L, 3, "bytes"); bytes = luaL_checklstring(L, -1, &length);
    status = cdin_kernel_host_transaction_replace(host->backend->kernel, transaction->value,
        start, finish, (const uint8_t *)bytes, length, &host_details);
    lua_pop(L, 1);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        lua_error_details_from_host(&details, &host_details, status);
        return raise_status(L, status, "documents.transaction_replace", &details);
    }
    lua_createtable(L, 0, 2);
    lua_pushboolean(L, 1); lua_setfield(L, -2, "accepted");
    return 1;
}

int lua_doc_transaction_read(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *transaction = check_handle(L, 2, CDIN_LUA_TRANSACTION);
    cdin_host_read_result result;
    uint64_t start, length;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    start = field_u64(L, 3, "start", 0u);
    length = field_u64(L, 3, "length", 0u);
    status = cdin_kernel_host_transaction_read(host->backend->kernel, transaction->value,
        start, length, &result);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        lua_error_details_from_host(&details, &result.details, status);
        return raise_status(L, status, "documents.transaction_read", &details);
    }
    lua_createtable(L, 0, 2);
    lua_pushlstring(L, (const char *)result.bytes, result.length); lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, (lua_Integer)result.total_length); lua_setfield(L, -2, "total_length");
    cdin_kernel_host_read_result_free(&result);
    return 1;
}

int lua_doc_commit_transaction(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *transaction = check_handle(L, 2, CDIN_LUA_TRANSACTION);
    cdin_host_edit_result result;
    const uint8_t *before = NULL, *after = NULL;
    size_t before_length = 0u, after_length = 0u;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    (void)get_optional_blob(L, 3, "undo_blob_before", &before, &before_length); if (before != NULL || before_length != 0u) lua_pop(L, 1);
    (void)get_optional_blob(L, 3, "undo_blob_after", &after, &after_length); if (after != NULL || after_length != 0u) lua_pop(L, 1);
    status = cdin_kernel_host_transaction_commit(host->backend->kernel, transaction->value,
        before, before_length, after, after_length, &result);
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        lua_error_details_from_host(&details, &result.details, status);
        return raise_status(L, status, "documents.commit_transaction", &details);
    }
    transaction->active = 0;
    push_edit_result(L, &result);
    cdin_kernel_host_edit_result_free(&result);
    return 1;
}

int lua_doc_abort_transaction(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *transaction = check_handle(L, 2, CDIN_LUA_TRANSACTION);
    cdin_backend_status status = cdin_kernel_host_transaction_abort(host->backend->kernel, transaction->value);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.abort_transaction", NULL);
    transaction->active = 0;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_doc_mark_clean(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *document = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_backend_status status = cdin_kernel_host_document_mark_clean(host->backend->kernel, document->value);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.mark_clean", NULL);
    lua_pushboolean(L, 1);
    return 1;
}
int lua_doc_mark_clean_if_version(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *document = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    uint64_t expected = check_u64(L, 3, "saved_version");
    int clean = 0;
    cdin_backend_status status = cdin_kernel_host_document_mark_clean_if_version(host->backend->kernel,
        document->value, expected, &clean);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.mark_clean_if_version", NULL);
    lua_pushboolean(L, clean);
    return 1;
}
int lua_doc_is_clean(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *document = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    int clean = 0;
    cdin_backend_status status = cdin_kernel_host_document_is_clean(host->backend->kernel, document->value, &clean);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.is_clean", NULL);
    lua_pushboolean(L, clean);
    return 1;
}

int lua_doc_subscribe(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *document = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_handle subscription;
    uint64_t version;
    cdin_backend_status status = cdin_kernel_host_subscribe(host->backend->kernel, document->value,
        &subscription, &version);
    if (status != CDIN_BACKEND_OK) return raise_status(L, status, "documents.subscribe", NULL);
    lua_createtable(L, 0, 2);
    (void)push_handle(L, subscription, CDIN_LUA_SUBSCRIPTION); lua_setfield(L, -2, "handle");
    lua_pushinteger(L, (lua_Integer)version); lua_setfield(L, -2, "version");
    return 1;
}
int lua_doc_unsubscribe(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *subscription = (cdin_lua_handle *)luaL_checkudata(L, 2, CDIN_LUA_HANDLE_METATABLE);
    cdin_backend_status status;
    if (subscription->kind != CDIN_LUA_SUBSCRIPTION) return raise_argument(L, "documents.unsubscribe handle");
    if (!subscription->active) { lua_pushboolean(L, 1); return 1; }
    status = cdin_kernel_host_unsubscribe(host->backend->kernel, subscription->value);
    if (status != CDIN_BACKEND_OK && status != CDIN_BACKEND_NOT_FOUND) return raise_status(L, status, "documents.unsubscribe", NULL);
    subscription->active = 0;
    lua_pushboolean(L, 1);
    return 1;
}

static const char *notification_name(uint32_t kind)
{
    static const char *const names[] = {
        "EDIT", "BATCH", "TRANSACTION", "UNDO", "REDO", "RELOAD", "CLOSED",
        "HISTORY_EVICTED", "RESYNC", "SAVED_STATE_CHANGED"
    };
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "RESYNC";
}
int lua_doc_drain(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *subscription = check_handle(L, 2, CDIN_LUA_SUBSCRIPTION);
    size_t max_events = check_optional_size(L, 3, 64, 4096u, "max_events");
    cdin_host_notification *events = NULL;
    size_t count = 0u, i;
    cdin_backend_status status;
    if (max_events > 4096u) return raise_status(L, CDIN_BACKEND_LIMIT_EXCEEDED, "documents.drain", NULL);
    if (max_events == 0u) { lua_newtable(L); return 1; }
    events = (cdin_host_notification *)calloc(max_events, sizeof(*events));
    if (events == NULL) return raise_status(L, CDIN_BACKEND_NO_MEMORY, "documents.drain", NULL);
    status = cdin_kernel_host_drain(host->backend->kernel, subscription->value,
        max_events, events, &count);
    if (status != CDIN_BACKEND_OK) { free(events); return raise_status(L, status, "documents.drain", NULL); }
    lua_createtable(L, (int)count, 0);
    for (i = 0u; i < count; i++) {
        size_t j;
        lua_createtable(L, 0, 10);
        lua_pushstring(L, notification_name(events[i].kind)); lua_setfield(L, -2, "kind");
        lua_pushinteger(L, (lua_Integer)events[i].from_version); lua_setfield(L, -2, "from_version");
        lua_pushinteger(L, (lua_Integer)events[i].to_version); lua_setfield(L, -2, "to_version");
        lua_pushinteger(L, (lua_Integer)events[i].current_version); lua_setfield(L, -2, "current_version");
        lua_pushinteger(L, (lua_Integer)events[i].edit_count); lua_setfield(L, -2, "edit_count");
        lua_pushinteger(L, (lua_Integer)events[i].edits_stored); lua_setfield(L, -2, "edits_stored");
        lua_pushboolean(L, events[i].edits_truncated); lua_setfield(L, -2, "edits_truncated");
        lua_pushinteger(L, (lua_Integer)events[i].evicted); lua_setfield(L, -2, "evicted");
        lua_pushboolean(L, events[i].is_clean); lua_setfield(L, -2, "is_clean");
        lua_createtable(L, (int)events[i].edits_stored, 0);
        for (j = 0u; j < events[i].edits_stored && j < 8u; j++) {
            lua_createtable(L, 0, 3);
            lua_pushinteger(L, (lua_Integer)events[i].edits[j].start); lua_setfield(L, -2, "start");
            lua_pushinteger(L, (lua_Integer)events[i].edits[j].removed); lua_setfield(L, -2, "removed");
            lua_pushinteger(L, (lua_Integer)events[i].edits[j].inserted); lua_setfield(L, -2, "inserted");
            lua_rawseti(L, -2, (lua_Integer)(j + 1u));
        }
        lua_setfield(L, -2, "edits");
        lua_rawseti(L, -2, (lua_Integer)(i + 1u));
    }
    free(events);
    return 1;
}

int lua_doc_search(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    cdin_lua_handle *document = check_handle(L, 2, CDIN_LUA_DOCUMENT);
    cdin_host_search_result result;
    const char *pattern, *mode, *direction;
    size_t pattern_length = 0u;
    uint32_t search_mode, search_direction, flags = 0u;
    uint64_t from, bound, limit, expected, steps;
    int has_expected;
    cdin_backend_status status;
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_getfield(L, 3, "pattern"); pattern = luaL_checklstring(L, -1, &pattern_length);
    lua_getfield(L, 3, "mode"); mode = luaL_optstring(L, -1, "literal");
    if (strcmp(mode, "literal") == 0) search_mode = CDIN_HOST_SEARCH_LITERAL;
    else if (strcmp(mode, "bytes") == 0) search_mode = CDIN_HOST_SEARCH_BYTES;
    else if (strcmp(mode, "regex") == 0) { lua_pop(L, 2); return raise_status(L, CDIN_BACKEND_UNSUPPORTED, "documents.search", NULL); }
    else { lua_pop(L, 2); return raise_argument(L, "documents.search mode"); }
    lua_pop(L, 1);
    lua_getfield(L, 3, "direction"); direction = luaL_optstring(L, -1, "forward");
    if (strcmp(direction, "forward") == 0) search_direction = CDIN_HOST_SEARCH_FORWARD;
    else if (strcmp(direction, "backward") == 0) search_direction = CDIN_HOST_SEARCH_BACKWARD;
    else { lua_pop(L, 2); return raise_argument(L, "documents.search direction"); }
    lua_pop(L, 1);
    from = field_u64(L, 3, "from", 0u);
    bound = field_u64(L, 3, "bound", UINT64_MAX);
    limit = field_u64(L, 3, "limit", 100u);
    steps = field_u64(L, 3, "step_limit", 0u);
    lua_getfield(L, 3, "expected_version"); has_expected = !lua_isnil(L, -1);
    expected = has_expected ? check_u64(L, -1, "expected_version") : 0u; lua_pop(L, 1);
    if (field_boolean(L, 3, "case_insensitive")) flags |= CDIN_HOST_SEARCH_CASE_INSENSITIVE;
    if (field_boolean(L, 3, "any_newline")) flags |= CDIN_HOST_SEARCH_ANY_NEWLINE;
    if (limit == 0u || limit > 100000u || limit > (uint64_t)INT_MAX || limit > SIZE_MAX / 2u) { lua_pop(L, 1); return raise_argument(L, "documents.search limit"); }
    status = cdin_kernel_host_search(host->backend->kernel, document->value,
        (const uint8_t *)pattern, pattern_length, search_mode, search_direction,
        from, bound, limit, has_expected, expected, flags, steps, &result);
    lua_pop(L, 1); /* pattern */
    if (status != CDIN_BACKEND_OK) {
        cdin_lua_error_details details;
        lua_error_details_from_host(&details, &result.details, status);
        return raise_status(L, status, "documents.search", &details);
    }
    lua_createtable(L, 0, 10);
    lua_pushinteger(L, (lua_Integer)result.version); lua_setfield(L, -2, "version");
    lua_pushinteger(L, (lua_Integer)result.count); lua_setfield(L, -2, "count");
    lua_pushinteger(L, (lua_Integer)result.next_from); lua_setfield(L, -2, "next_from");
    lua_pushinteger(L, (lua_Integer)result.next_bound); lua_setfield(L, -2, "next_bound");
    lua_pushinteger(L, (lua_Integer)result.steps); lua_setfield(L, -2, "steps");
    lua_pushboolean(L, result.truncated); lua_setfield(L, -2, "truncated");
    lua_pushboolean(L, result.partial); lua_setfield(L, -2, "partial");
    lua_pushboolean(L, result.skipped_raw); lua_setfield(L, -2, "skipped_raw");
    lua_pushboolean(L, result.exhausted); lua_setfield(L, -2, "exhausted");
    lua_createtable(L, (int)result.count, 0);
    {
        uint64_t i;
        for (i = 0u; i < result.count; i++) {
            lua_createtable(L, 2, 0);
            lua_pushinteger(L, (lua_Integer)result.ranges[i * 2u]); lua_rawseti(L, -2, 1);
            lua_pushinteger(L, (lua_Integer)result.ranges[i * 2u + 1u]); lua_rawseti(L, -2, 2);
            lua_rawseti(L, -2, (lua_Integer)(i + 1u));
        }
    }
    lua_setfield(L, -2, "ranges");
    cdin_kernel_host_search_result_free(&result);
    return 1;
}


#endif
