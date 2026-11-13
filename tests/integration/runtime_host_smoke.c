/*
 * Live Backend -> PUC Lua 5.4 -> cdin-runtime startup/shutdown smoke test.
 *
 * Requirement references:
 * - LIFE-001 / LIFE-002: https://github.com/cdin-editor/spec/blob/c4de9bf3d506b4e1f07b8986531cd056341708b0/docs/architecture/lifecycle.md#L37-L40
 * - BE-007: https://github.com/cdin-editor/spec/blob/c4de9bf3d506b4e1f07b8986531cd056341708b0/docs/components/backend.md#L70-L71
 * - Spec ADR-0019 (PUC Lua 5.4 C API bridge): https://github.com/cdin-editor/spec/blob/main/docs/adr/ADR-0019-runtime-bridge.md
 *
 * This test intentionally uses the production Runtime tree, not tests/support/fake_host.lua.
 */
#define CDIN_WITH_LUA 1
#include "backend_internal.h"
#include "runtime/lua_host_internal.h"
#include "cdin/backend.h"
#include "cdin/backend_protocol.h"
#include <cdin/protocol.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static size_t write_cbor_uint(uint8_t *out, uint64_t value)
{
    if (value < 24u) { out[0] = (uint8_t)value; return 1u; }
    if (value <= UINT8_MAX) { out[0] = 0x18u; out[1] = (uint8_t)value; return 2u; }
    if (value <= UINT16_MAX) { out[0] = 0x19u; out[1] = (uint8_t)(value >> 8u); out[2] = (uint8_t)value; return 3u; }
    out[0] = 0x1bu;
    out[1] = (uint8_t)(value >> 56u); out[2] = (uint8_t)(value >> 48u);
    out[3] = (uint8_t)(value >> 40u); out[4] = (uint8_t)(value >> 32u);
    out[5] = (uint8_t)(value >> 24u); out[6] = (uint8_t)(value >> 16u);
    out[7] = (uint8_t)(value >> 8u); out[8] = (uint8_t)value;
    return 9u;
}
static size_t write_cbor_text(uint8_t *out, const char *text)
{
    size_t n = strlen(text), h = n < 24u ? 1u : 2u;
    if (n < 24u) out[0] = (uint8_t)(0x60u + n);
    else { out[0] = 0x78u; out[1] = (uint8_t)n; }
    memcpy(out + h, text, n);
    return h + n;
}
static size_t make_splitter_payload(uint8_t *out, size_t cap, uint64_t request_id,
                                    uint64_t revision, const char *split_id)
{
    size_t p = 0u;
    if (cap < 128u) return 0u;
    out[p++] = 0xa4u;
    p += write_cbor_text(out + p, "ratio_bp"); p += write_cbor_uint(out + p, 5000u);
    p += write_cbor_text(out + p, "split_id"); p += write_cbor_text(out + p, split_id);
    p += write_cbor_text(out + p, "request_id"); p += write_cbor_uint(out + p, request_id);
    p += write_cbor_text(out + p, "layout_revision"); p += write_cbor_uint(out + p, revision);
    return p;
}
static int get_frontend_revision(cdin_backend_frontend *frontend, uint64_t *revision)
{
    size_t i, count = cdin_backend_frontend_pending(frontend);
    cdin_backend_message message;
    int found = 0;
    *revision = 0u;
    for (i = 0u; i < count; ++i) {
        uint64_t candidate = 0u;
        if (cdin_backend_frontend_message_at(frontend, i, &message) != CDIN_BACKEND_OK ||
            message.type != CDIN_BACKEND_MESSAGE_UI_LAYOUT) continue;
        if (cdin_protocol_payload_get_uint(message.payload, message.payload_length,
                                           "revision", &candidate) == CDIN_PROTOCOL_OK &&
            (!found || candidate > *revision)) {
            *revision = candidate;
            found = 1;
        }
    }
    return found;
}
static int send_splitter_expect_status(cdin_backend *backend, cdin_backend_frontend *frontend,
                                       uint64_t revision, uint64_t request_id,
                                       uint64_t frame_sequence,
                                       const char *split_id, const char *expected)
{
    uint8_t payload[256], frame[512];
    size_t payload_length, frame_length = 0u, i;
    size_t processed = 0u, status_length = 0u;
    uint64_t observed_id = 0u;
    char observed_status[32];
    cdin_protocol_message_view input;
    cdin_backend_message output;
    payload_length = make_splitter_payload(payload, sizeof(payload), request_id, revision, split_id);
    if (payload_length == 0u) return 0;
    memset(&input, 0, sizeof(input));
    input.major = CDIN_PROTOCOL_VERSION_MAJOR;
    input.minor = CDIN_PROTOCOL_VERSION_MINOR;
    input.sequence = frame_sequence;
    input.type = CDIN_PROTOCOL_PANE_SPLITTER_RESIZE;
    input.name_space = "pane";
    input.payload = payload;
    input.payload_length = payload_length;
    if (cdin_protocol_encode_frame(&input, frame, sizeof(frame), &frame_length,
                                   CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES) != CDIN_PROTOCOL_OK) return 0;
    if (cdin_backend_frontend_receive(frontend, frame, frame_length) != CDIN_BACKEND_OK) return 0;
    if (cdin_backend_pump(backend, 64u, &processed) != CDIN_BACKEND_OK) return 0;
    for (i = 0u; i < cdin_backend_frontend_pending(frontend); ++i) {
        if (cdin_backend_frontend_message_at(frontend, i, &output) != CDIN_BACKEND_OK ||
            output.type != CDIN_BACKEND_MESSAGE_PANE_SPLITTER_RESULT) continue;
        if (cdin_protocol_payload_get_uint(output.payload, output.payload_length,
                                           "request_id", &observed_id) != CDIN_PROTOCOL_OK) return 0;
        if (observed_id != request_id) continue;
        if (cdin_protocol_payload_get_text(output.payload, output.payload_length, "status",
                                           observed_status, sizeof(observed_status),
                                           &status_length) != CDIN_PROTOCOL_OK) return 0;
        observed_status[status_length < sizeof(observed_status) ? status_length : sizeof(observed_status)-1u] = '\0';
        return strcmp(observed_status, expected) == 0;
    }
    return 0;
}

static int create_runtime_split(cdin_backend *backend, char *split_id,
                               size_t split_id_capacity, uint64_t *revision)
{
    lua_State *L;
    int base, runtime_index, document_index, ui_index, tree_index, snapshot_index;
    char document_id[129], first_view_id[129];
    size_t document_id_length = 0u, first_view_id_length = 0u, split_id_length = 0u;
    int ok = 0;

    if (backend == NULL || backend->runtime == NULL || split_id == NULL ||
        split_id_capacity == 0u || revision == NULL) return 0;
    L = backend->runtime->state;
    if (L == NULL) return 0;
    base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, backend->runtime->runtime_ref);
    if (!lua_istable(L, -1)) goto done;
    runtime_index = lua_absindex(L, -1);

    lua_getfield(L, runtime_index, "open_document");
    if (!lua_isfunction(L, -1)) goto done;
    lua_pushvalue(L, runtime_index);
    lua_pushliteral(L, "backend splitter live integration");
    lua_newtable(L);
    if (lua_pcall(L, 3, 1, 0) != LUA_OK || !lua_istable(L, -1)) goto done;
    document_index = lua_absindex(L, -1);
    lua_getfield(L, document_index, "id");
    if (lua_type(L, -1) != LUA_TSTRING) goto done;
    {
        const char *value = lua_tolstring(L, -1, &document_id_length);
        if (value == NULL || document_id_length == 0u || document_id_length >= sizeof(document_id)) goto done;
        memcpy(document_id, value, document_id_length);
        document_id[document_id_length] = '\0';
    }
    lua_pop(L, 1);

    lua_getfield(L, runtime_index, "create_view");
    if (!lua_isfunction(L, -1)) goto done;
    lua_pushvalue(L, runtime_index);
    lua_pushstring(L, document_id);
    lua_newtable(L);
    if (lua_pcall(L, 3, 1, 0) != LUA_OK || !lua_istable(L, -1)) goto done;
    lua_getfield(L, -1, "id");
    if (lua_type(L, -1) != LUA_TSTRING) goto done;
    {
        const char *value = lua_tolstring(L, -1, &first_view_id_length);
        if (value == NULL || first_view_id_length == 0u ||
            first_view_id_length >= sizeof(first_view_id)) goto done;
        memcpy(first_view_id, value, first_view_id_length);
        first_view_id[first_view_id_length] = '\0';
    }
    lua_pop(L, 2); /* First View and its ID. */

    lua_getfield(L, runtime_index, "create_view");
    if (!lua_isfunction(L, -1)) goto done;
    lua_pushvalue(L, runtime_index);
    lua_pushstring(L, document_id);
    lua_createtable(L, 0, 3);
    lua_pushstring(L, first_view_id); lua_setfield(L, -2, "split_with");
    lua_pushliteral(L, "vertical"); lua_setfield(L, -2, "axis");
    lua_pushnumber(L, 0.5); lua_setfield(L, -2, "ratio");
    if (lua_pcall(L, 3, 1, 0) != LUA_OK || !lua_istable(L, -1)) goto done;
    lua_pop(L, 1); /* Second View. */

    lua_getfield(L, runtime_index, "ui");
    if (!lua_istable(L, -1)) goto done;
    ui_index = lua_absindex(L, -1);
    lua_getfield(L, ui_index, "revision");
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) goto done;
    *revision = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, ui_index, "pane_tree");
    if (!lua_istable(L, -1)) goto done;
    tree_index = lua_absindex(L, -1);
    lua_getfield(L, tree_index, "snapshot");
    if (!lua_isfunction(L, -1)) goto done;
    lua_pushvalue(L, tree_index);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK || !lua_istable(L, -1)) goto done;
    snapshot_index = lua_absindex(L, -1);
    lua_getfield(L, snapshot_index, "root");
    if (!lua_istable(L, -1)) goto done;
    lua_getfield(L, -1, "kind");
    if (lua_type(L, -1) != LUA_TSTRING || strcmp(lua_tostring(L, -1), "split") != 0) goto done;
    lua_pop(L, 1);
    lua_getfield(L, -1, "id");
    if (lua_type(L, -1) != LUA_TSTRING) goto done;
    {
        const char *value = lua_tolstring(L, -1, &split_id_length);
        if (value == NULL || split_id_length == 0u || split_id_length >= split_id_capacity) goto done;
        memcpy(split_id, value, split_id_length);
        split_id[split_id_length] = '\0';
    }
    ok = 1;
done:
    if (!ok) {
        const char *error = lua_tostring(L, -1);
        if (error != NULL) (void)fprintf(stderr, "Runtime split fixture failed: %s\n", error);
    }
    lua_settop(L, base);
    return ok;
}

static int verify_success_batch(cdin_backend_frontend *frontend, uint64_t request_id,
                                uint64_t expected_revision)
{
    size_t i, count = cdin_backend_frontend_pending(frontend);
    size_t result_index = SIZE_MAX, layout_index = SIZE_MAX, flush_index = SIZE_MAX;
    uint64_t group = 0u, value = 0u;
    char status[32], scope[16];
    size_t status_length = 0u, scope_length = 0u;
    cdin_backend_message message;

    for (i = 0u; i < count; ++i) {
        if (cdin_backend_frontend_message_at(frontend, i, &message) != CDIN_BACKEND_OK ||
            message.type != CDIN_BACKEND_MESSAGE_PANE_SPLITTER_RESULT) continue;
        if (cdin_protocol_payload_get_uint(message.payload, message.payload_length,
                                           "request_id", &value) != CDIN_PROTOCOL_OK ||
            value != request_id) continue;
        if (cdin_protocol_payload_get_text(message.payload, message.payload_length, "status",
                                           status, sizeof(status), &status_length) != CDIN_PROTOCOL_OK ||
            cdin_protocol_payload_get_uint(message.payload, message.payload_length,
                                           "layout_revision", &value) != CDIN_PROTOCOL_OK) return 0;
        status[status_length < sizeof(status) ? status_length : sizeof(status)-1u] = '\0';
        if (strcmp(status, "applied") != 0 || value != expected_revision) return 0;
        result_index = i;
        group = message.flush_sequence;
        break;
    }
    if (result_index == SIZE_MAX || group == 0u) return 0;

    for (i = result_index + 1u; i < count; ++i) {
        if (cdin_backend_frontend_message_at(frontend, i, &message) != CDIN_BACKEND_OK ||
            message.flush_sequence != group) continue;
        if (layout_index == SIZE_MAX && message.type == CDIN_BACKEND_MESSAGE_UI_LAYOUT) {
            if (cdin_protocol_payload_get_uint(message.payload, message.payload_length,
                                               "revision", &value) != CDIN_PROTOCOL_OK ||
                value != expected_revision) return 0;
            layout_index = i;
            continue;
        }
        if (layout_index != SIZE_MAX && message.type == CDIN_PROTOCOL_UI_FLUSH) {
            if (cdin_protocol_payload_get_text(message.payload, message.payload_length, "scope",
                                               scope, sizeof(scope), &scope_length) != CDIN_PROTOCOL_OK ||
                cdin_protocol_payload_get_uint(message.payload, message.payload_length,
                                               "revision", &value) != CDIN_PROTOCOL_OK) return 0;
            scope[scope_length < sizeof(scope) ? scope_length : sizeof(scope)-1u] = '\0';
            if (strcmp(scope, "snapshot") != 0 || value != expected_revision) return 0;
            flush_index = i;
            break;
        }
    }
    return layout_index != SIZE_MAX && flush_index != SIZE_MAX &&
        result_index < layout_index && layout_index < flush_index;
}


static int runtime_handle_error_and_gc_conformance(cdin_backend *backend)
{
    lua_State *L;
    int base, docs_index, result_index, handle_index, error_index;
    cdin_lua_handle *document_userdata;
    cdin_host_handle document_handle, snapshot_handle, transaction_handle, subscription_handle;
    cdin_host_doc_info info;
    cdin_backend_status status;
    int passed = 0;

#define REQUIRE_RUNTIME_TEST(condition, message) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "Runtime Host conformance failed: %s\n", message); \
        goto done; \
    } \
} while (0)

    if (backend == NULL || backend->runtime == NULL ||
        backend->runtime->state == NULL || backend->runtime->host_ref == LUA_NOREF)
        return 0;

    L = backend->runtime->state;
    base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, backend->runtime->host_ref);
    REQUIRE_RUNTIME_TEST(lua_istable(L, -1), "Backend Host table is unavailable");
    lua_getfield(L, -1, "documents");
    REQUIRE_RUNTIME_TEST(lua_istable(L, -1), "Host documents service is unavailable");
    lua_remove(L, -2);
    docs_index = lua_absindex(L, -1);

    /* Exercise the production adapter's structured status mapping. */
    lua_getfield(L, docs_index, "create");
    lua_pushvalue(L, docs_index);
    lua_pushliteral(L, "abc");
    REQUIRE_RUNTIME_TEST(lua_pcall(L, 2, 1, 0) == LUA_OK,
                         "documents.create failed");
    result_index = lua_absindex(L, -1);
    lua_getfield(L, result_index, "handle");
    handle_index = lua_absindex(L, -1);
    document_userdata = (cdin_lua_handle *)luaL_testudata(
        L, handle_index, CDIN_LUA_HANDLE_METATABLE);
    REQUIRE_RUNTIME_TEST(document_userdata != NULL &&
                         document_userdata->kind == CDIN_LUA_DOCUMENT &&
                         document_userdata->active,
                         "documents.create did not return an active document handle");
    document_handle = document_userdata->value;
    lua_remove(L, result_index);
    handle_index = lua_absindex(L, -1);

    lua_getfield(L, docs_index, "replace");
    lua_pushvalue(L, docs_index);
    lua_pushvalue(L, handle_index);
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, 0); lua_setfield(L, -2, "expected_version");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "start");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "finish");
    lua_pushliteral(L, "x"); lua_setfield(L, -2, "bytes");
    REQUIRE_RUNTIME_TEST(lua_pcall(L, 3, 1, 0) == LUA_OK,
                         "documents.replace failed");
    lua_pop(L, 1);

    lua_getfield(L, docs_index, "read");
    lua_pushvalue(L, docs_index);
    lua_pushvalue(L, handle_index);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, 0); lua_setfield(L, -2, "start");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "length");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "expected_version");
    REQUIRE_RUNTIME_TEST(lua_pcall(L, 3, 1, 0) != LUA_OK,
                         "stale documents.read unexpectedly succeeded");
    error_index = lua_absindex(L, -1);
    REQUIRE_RUNTIME_TEST(lua_istable(L, error_index),
                         "stale read did not return a structured error table");
    lua_getfield(L, error_index, "domain");
    REQUIRE_RUNTIME_TEST(lua_isstring(L, -1) &&
                         strcmp(lua_tostring(L, -1), "cdin.runtime") == 0,
                         "stale read error domain is incorrect");
    lua_pop(L, 1);
    lua_getfield(L, error_index, "code");
    REQUIRE_RUNTIME_TEST(lua_isstring(L, -1) &&
                         strcmp(lua_tostring(L, -1), "STALE_VERSION") == 0,
                         "stale Kernel status was not preserved as STALE_VERSION");
    lua_pop(L, 1);
    lua_getfield(L, error_index, "details");
    REQUIRE_RUNTIME_TEST(lua_istable(L, -1),
                         "stale read error details are missing");
    lua_getfield(L, -1, "expected");
    REQUIRE_RUNTIME_TEST(lua_isinteger(L, -1) && lua_tointeger(L, -1) == 0,
                         "stale read expected-version detail is incorrect");
    lua_pop(L, 1);
    lua_getfield(L, -1, "current");
    REQUIRE_RUNTIME_TEST(lua_isinteger(L, -1) && lua_tointeger(L, -1) == 1,
                         "stale read current-version detail is incorrect");
    lua_pop(L, 2); /* details and structured error */

    /*
     * Drop each Backend-owned Lua userdata without an explicit close. A forced
     * full collection must release the corresponding Kernel handle exactly
     * once; querying/releasing the copied opaque identity must then report the
     * adapter's invalid-handle result.
     */
    lua_getfield(L, docs_index, "acquire_snapshot");
    lua_pushvalue(L, docs_index);
    lua_pushvalue(L, handle_index);
    REQUIRE_RUNTIME_TEST(lua_pcall(L, 2, 1, 0) == LUA_OK,
                         "documents.acquire_snapshot failed");
    result_index = lua_absindex(L, -1);
    lua_getfield(L, result_index, "handle");
    {
        cdin_lua_handle *value = (cdin_lua_handle *)luaL_testudata(
            L, -1, CDIN_LUA_HANDLE_METATABLE);
        REQUIRE_RUNTIME_TEST(value != NULL && value->kind == CDIN_LUA_SNAPSHOT &&
                             value->active,
                             "snapshot did not return an active Backend handle");
        snapshot_handle = value->value;
    }
    lua_remove(L, result_index);
    lua_pop(L, 1);
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    status = cdin_kernel_host_snapshot_release(backend->kernel, snapshot_handle);
    REQUIRE_RUNTIME_TEST(status == CDIN_BACKEND_NOT_FOUND,
                         "snapshot userdata finalizer did not release its Kernel handle");

    lua_getfield(L, docs_index, "begin_transaction");
    lua_pushvalue(L, docs_index);
    lua_pushvalue(L, handle_index);
    lua_createtable(L, 0, 1);
    lua_pushinteger(L, 1); lua_setfield(L, -2, "expected_version");
    REQUIRE_RUNTIME_TEST(lua_pcall(L, 3, 1, 0) == LUA_OK,
                         "documents.begin_transaction failed");
    result_index = lua_absindex(L, -1);
    lua_getfield(L, result_index, "handle");
    {
        cdin_lua_handle *value = (cdin_lua_handle *)luaL_testudata(
            L, -1, CDIN_LUA_HANDLE_METATABLE);
        REQUIRE_RUNTIME_TEST(value != NULL && value->kind == CDIN_LUA_TRANSACTION &&
                             value->active,
                             "transaction did not return an active Backend handle");
        transaction_handle = value->value;
    }
    lua_remove(L, result_index);
    lua_pop(L, 1);
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    status = cdin_kernel_host_transaction_abort(backend->kernel, transaction_handle);
    REQUIRE_RUNTIME_TEST(status == CDIN_BACKEND_NOT_FOUND,
                         "transaction userdata finalizer did not abort its Kernel handle");

    lua_getfield(L, docs_index, "subscribe");
    lua_pushvalue(L, docs_index);
    lua_pushvalue(L, handle_index);
    REQUIRE_RUNTIME_TEST(lua_pcall(L, 2, 1, 0) == LUA_OK,
                         "documents.subscribe failed");
    result_index = lua_absindex(L, -1);
    lua_getfield(L, result_index, "handle");
    {
        cdin_lua_handle *value = (cdin_lua_handle *)luaL_testudata(
            L, -1, CDIN_LUA_HANDLE_METATABLE);
        REQUIRE_RUNTIME_TEST(value != NULL && value->kind == CDIN_LUA_SUBSCRIPTION &&
                             value->active,
                             "subscribe did not return an active Backend handle");
        subscription_handle = value->value;
    }
    lua_remove(L, result_index);
    lua_pop(L, 1);
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    status = cdin_kernel_host_unsubscribe(backend->kernel, subscription_handle);
    REQUIRE_RUNTIME_TEST(status == CDIN_BACKEND_NOT_FOUND,
                         "subscription userdata finalizer did not unsubscribe its Kernel handle");

    /* Explicit close disables the finalizer before the Lua object is collected. */
    lua_getfield(L, docs_index, "close");
    lua_pushvalue(L, docs_index);
    lua_pushvalue(L, handle_index);
    REQUIRE_RUNTIME_TEST(lua_pcall(L, 2, 1, 0) == LUA_OK && lua_toboolean(L, -1),
                         "documents.close failed");
    lua_pop(L, 1);
    REQUIRE_RUNTIME_TEST(document_userdata->active == 0,
                         "explicit document close left its userdata finalizer active");
    lua_pop(L, 1); /* Document userdata. */
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    status = cdin_kernel_host_document_info(backend->kernel, document_handle, &info);
    REQUIRE_RUNTIME_TEST(status == CDIN_BACKEND_NOT_FOUND,
                         "explicit close did not invalidate the Kernel document handle");

    /* The fallback finalizer must also close an unclosed document handle. */
    lua_getfield(L, docs_index, "create");
    lua_pushvalue(L, docs_index);
    lua_pushliteral(L, "gc-document");
    REQUIRE_RUNTIME_TEST(lua_pcall(L, 2, 1, 0) == LUA_OK,
                         "second documents.create failed");
    result_index = lua_absindex(L, -1);
    lua_getfield(L, result_index, "handle");
    {
        cdin_lua_handle *value = (cdin_lua_handle *)luaL_testudata(
            L, -1, CDIN_LUA_HANDLE_METATABLE);
        REQUIRE_RUNTIME_TEST(value != NULL && value->kind == CDIN_LUA_DOCUMENT &&
                             value->active,
                             "second document did not return an active handle");
        document_handle = value->value;
    }
    lua_remove(L, result_index);
    lua_pop(L, 1);
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    status = cdin_kernel_host_document_info(backend->kernel, document_handle, &info);
    REQUIRE_RUNTIME_TEST(status == CDIN_BACKEND_NOT_FOUND,
                         "document userdata finalizer did not close its Kernel handle");

    passed = 1;
    (void)puts("PASS runtime_host_conformance: typed stale-version errors and document/snapshot/transaction/subscription finalizers");

done:
    lua_settop(L, base);
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
#undef REQUIRE_RUNTIME_TEST
    return passed;
}

int main(void)
{
    const char *runtime_root = getenv("CDIN_RUNTIME_ROOT");
    cdin_backend_config config;
    cdin_backend *backend = NULL;
    cdin_backend_frontend *driver = NULL, *observer = NULL;
    cdin_backend_hello hello;
    cdin_backend_hello_ack ack;
    uint64_t revision = 0u;
    uint64_t split_revision = 0u;
    uint64_t next_revision = 0u;
    char split_id[129];
    cdin_backend_status status;
    size_t processed = 0u;

    if (runtime_root == NULL || runtime_root[0] == '\0') {
        (void)fprintf(stderr, "CDIN_RUNTIME_ROOT must point to the checked-out runtime/lua directory.\n");
        return 2;
    }

    cdin_backend_config_init(&config);
    config.enable_lua = 1;
    config.enable_headless_protocol = 1;
    config.runtime_root = runtime_root;
    config.runtime_entry = "cdin.runtime";
    config.log_level = CDIN_BACKEND_LOG_WARN;

    status = cdin_backend_create(&config, &backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend create failed: %s\n", cdin_backend_status_name(status));
        return 1;
    }
    status = cdin_backend_start(backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend/Runtime startup failed: %s\n", cdin_backend_status_name(status));
        cdin_backend_destroy(backend);
        return 1;
    }
    if (!runtime_handle_error_and_gc_conformance(backend)) {
        (void)fprintf(stderr, "Runtime Host error/GC conformance failed.\n");
        goto fail;
    }
    if (!create_runtime_split(backend, split_id, sizeof(split_id), &split_revision)) {
        (void)fprintf(stderr, "Could not create the live Runtime split fixture.\n");
        goto fail;
    }
    if (cdin_backend_frontend_open(backend, &driver) != CDIN_BACKEND_OK) goto fail;
    memset(&hello, 0, sizeof(hello));
    hello.major = CDIN_BACKEND_PROTOCOL_MAJOR;
    hello.minor = CDIN_BACKEND_PROTOCOL_MINOR;
    hello.frontend_name = "splitter-driver";
    hello.capabilities = CDIN_BACKEND_CAP_PANE_SPLITTER_RESIZE |
                         CDIN_BACKEND_CAP_DRIVER_ROLE_EVENTS;
    if (cdin_backend_frontend_hello(driver, &hello, &ack) != CDIN_BACKEND_OK ||
        !ack.driver || (ack.accepted_capabilities & CDIN_BACKEND_CAP_PANE_SPLITTER_RESIZE) == 0u ||
        cdin_backend_frontend_attach(driver) != CDIN_BACKEND_OK ||
        cdin_backend_pump(backend, 64u, &processed) != CDIN_BACKEND_OK ||
        !get_frontend_revision(driver, &revision) ||
        !send_splitter_expect_status(backend, driver, revision, 1u, 1u, "missing-split", "not_found")) {
        (void)fprintf(stderr, "Live Runtime splitter request did not return not_found.\n");
        goto fail;
    }
    if (cdin_backend_frontend_open(backend, &observer) != CDIN_BACKEND_OK) goto fail;
    hello.frontend_name = "splitter-observer";
    if (cdin_backend_frontend_hello(observer, &hello, &ack) != CDIN_BACKEND_OK ||
        ack.driver || (ack.accepted_capabilities & CDIN_BACKEND_CAP_PANE_SPLITTER_RESIZE) == 0u ||
        cdin_backend_frontend_attach(observer) != CDIN_BACKEND_OK ||
        cdin_backend_pump(backend, 64u, &processed) != CDIN_BACKEND_OK ||
        !get_frontend_revision(observer, &revision) ||
        !send_splitter_expect_status(backend, observer, revision, 1u, 1u, "missing-split", "not_driver")) {
        (void)fprintf(stderr, "Live Runtime did not reject Observer splitter mutation.\n");
        goto fail;
    }
    if (!get_frontend_revision(driver, &revision) || revision != split_revision ||
        !send_splitter_expect_status(backend, driver, revision, 2u, 2u, split_id, "applied")) {
        (void)fprintf(stderr, "Live Runtime did not apply the semantic splitter resize.\n");
        goto fail;
    }
    next_revision = split_revision + 1u;
    if (!verify_success_batch(driver, 2u, next_revision)) {
        (void)fprintf(stderr, "Splitter result/layout/flush were not one ordered snapshot batch.\n");
        goto fail;
    }
    if (!get_frontend_revision(driver, &revision) || revision != next_revision) {
        (void)fprintf(stderr, "Splitter mutation did not advance the Runtime layout revision exactly once.\n");
        goto fail;
    }
    {
        const uint64_t wide_request_id = (UINT64_C(1) << 63u);
        if (!send_splitter_expect_status(backend, driver, next_revision,
                                         wide_request_id, 3u, split_id, "applied")) {
            (void)fprintf(stderr, "A full-width uint64 splitter request ID was rejected.\n");
            goto fail;
        }
        next_revision++;
        if (!verify_success_batch(driver, wide_request_id, next_revision)) {
            (void)fprintf(stderr, "Full-width request ID did not produce an atomic success snapshot.\n");
            goto fail;
        }
    }

    /* Simulate a client whose atomic snapshot could not be queued, then detach
     * it. The next dispatch must release the client-scoped pending gate so the
     * newly promoted Driver can resize normally. */
    backend->runtime->pane_result.active = 1;
    backend->runtime->pane_result.pending_publish = 1;
    backend->runtime->pane_result.client_id =
        cdin_protocol_client_id((cdin_protocol_client *)driver);
    backend->runtime->pane_result.request_id = 99u;
    backend->runtime->pane_result.expected_revision = next_revision;
    backend->runtime->pane_result.ratio_bp = 5000u;
    backend->runtime->pane_result.result_revision = next_revision;
    backend->runtime->pane_result.status = "applied";
    (void)snprintf(backend->runtime->pane_result.split_id,
                   sizeof(backend->runtime->pane_result.split_id), "%s", split_id);
    if (!send_splitter_expect_status(backend, driver, next_revision,
                                     4u, 4u, split_id, "busy")) {
        (void)fprintf(stderr, "A concurrent splitter request did not return the semantic busy result.\n");
        goto fail;
    }
    if (cdin_backend_frontend_detach(driver) != CDIN_BACKEND_OK ||
        cdin_backend_pump(backend, 64u, &processed) != CDIN_BACKEND_OK ||
        backend->runtime->pane_result.pending_publish) {
        (void)fprintf(stderr, "Detaching the pending publisher did not release the splitter gate.\n");
        goto fail;
    }
    if (!send_splitter_expect_status(backend, observer, next_revision,
                                     3u, 2u, split_id, "applied")) {
        (void)fprintf(stderr, "The promoted Driver remained blocked by a detached splitter publisher.\n");
        goto fail;
    }
    next_revision++;
    if (!verify_success_batch(observer, 3u, next_revision)) {
        (void)fprintf(stderr, "Promoted Driver resize did not publish an atomic result snapshot.\n");
        goto fail;
    }

    status = cdin_backend_pump(backend, 64u, &processed);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend pump failed: %s\n", cdin_backend_status_name(status));
        (void)cdin_backend_shutdown(backend, 1);
        cdin_backend_destroy(backend);
        return 1;
    }
    /* Close attached clients before shutdown destroys the Protocol server. */
    if (observer != NULL) {
        (void)cdin_backend_frontend_detach(observer);
        (void)cdin_backend_frontend_close(observer);
        observer = NULL;
    }
    if (driver != NULL) {
        (void)cdin_backend_frontend_detach(driver);
        (void)cdin_backend_frontend_close(driver);
        driver = NULL;
    }
    status = cdin_backend_shutdown(backend, 0);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend/Runtime graceful shutdown failed: %s\n", cdin_backend_status_name(status));
        cdin_backend_destroy(backend);
        return 1;
    }
    cdin_backend_destroy(backend);
    (void)puts("PASS runtime_host_smoke: real Lua 5.4 state, production cdin-runtime load/start/pump/shutdown");
    return 0;
fail:
    if (observer != NULL) (void)cdin_backend_frontend_close(observer);
    if (driver != NULL) (void)cdin_backend_frontend_close(driver);
    if (backend != NULL) {
        (void)cdin_backend_shutdown(backend, 1);
        cdin_backend_destroy(backend);
    }
    return 1;
}
