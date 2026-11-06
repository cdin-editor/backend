#ifndef _WIN32
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#endif

#include "backend_internal.h"
#include "runtime/lua_host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define TEST_SEPARATOR "\\"
static int make_dir(const char *path) { return CreateDirectoryA(path, NULL) ? 0 : -1; }
static int remove_dir(const char *path) { return RemoveDirectoryA(path) ? 0 : -1; }
static int remove_file(const char *path) { return DeleteFileA(path) ? 0 : -1; }
static int make_temp_root(char *output, size_t capacity)
{
    char temp[MAX_PATH];
    char file[MAX_PATH];
    DWORD length = GetTempPathA((DWORD)sizeof(temp), temp);
    if (length == 0u || length >= sizeof(temp)) return -1;
    if (GetTempFileNameA(temp, "cdx", 0u, file) == 0u) return -1;
    if (!DeleteFileA(file) || !CreateDirectoryA(file, NULL)) return -1;
    if (strlen(file) + 1u > capacity) {
        RemoveDirectoryA(file);
        return -1;
    }
    strcpy(output, file);
    return 0;
}
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define TEST_SEPARATOR "/"
static int make_dir(const char *path) { return mkdir(path, 0700); }
static int remove_dir(const char *path) { return rmdir(path); }
static int remove_file(const char *path) { return unlink(path); }
static int make_temp_root(char *output, size_t capacity)
{
    char pattern[] = "/tmp/cdin-extension-resolver-XXXXXX";
    char *created = mkdtemp(pattern);
    if (created == NULL || strlen(created) + 1u > capacity) return -1;
    strcpy(output, created);
    return 0;
}
#endif

typedef enum resolver_call_status {
    RESOLVER_CALL_OK = 0,
    RESOLVER_CALL_MISSING,
    RESOLVER_CALL_ERROR
} resolver_call_status;

static int join_path(char *output, size_t capacity, const char *left, const char *right)
{
    int written;
    size_t left_length;
    if (output == NULL || left == NULL || right == NULL) return -1;
    left_length = strlen(left);
    written = snprintf(output, capacity, "%s%s%s", left,
                       left_length != 0u && left[left_length - 1u] == TEST_SEPARATOR[0] ? "" : TEST_SEPARATOR,
                       right);
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int write_bytes(const char *path, const void *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    int result = 0;
    if (file == NULL) return -1;
    if (length != 0u && fwrite(bytes, 1u, length, file) != length) result = -1;
    if (fclose(file) != 0) result = -1;
    return result;
}

static resolver_call_status resolve_module(lua_State *L, int resolver_ref,
                                           const char *owner, const char *module,
                                           const uint8_t *expected, size_t expected_length,
                                           const char *expected_chunk, const char *expected_error)
{
    int base = lua_gettop(L);
    resolver_call_status result;
    lua_rawgeti(L, LUA_REGISTRYINDEX, resolver_ref);
    lua_pushstring(L, owner);
    lua_pushstring(L, module);
    if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
        const char *code = NULL;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "code");
            code = lua_tostring(L, -1);
        }
        if (expected_error != NULL && code != NULL && strcmp(code, expected_error) == 0) {
            result = RESOLVER_CALL_ERROR;
        } else {
            (void)fprintf(stderr, "resolver %s/%s returned unexpected error code=%s\n",
                          owner, module, code != NULL ? code : "(not structured)");
            result = (resolver_call_status)-1;
        }
    } else if (lua_isnil(L, -2)) {
        result = expected_error == NULL ? RESOLVER_CALL_MISSING : (resolver_call_status)-1;
    } else {
        size_t source_length = 0u;
        size_t chunk_length = 0u;
        const char *source = lua_tolstring(L, -2, &source_length);
        const char *chunk = lua_tolstring(L, -1, &chunk_length);
        if (expected_error != NULL || source == NULL || chunk == NULL ||
            source_length != expected_length ||
            (source_length != 0u && memcmp(source, expected, source_length) != 0) ||
            expected_chunk == NULL || chunk_length != strlen(expected_chunk) ||
            memcmp(chunk, expected_chunk, chunk_length) != 0) {
            (void)fprintf(stderr, "resolver %s/%s returned unexpected bytes/chunk\n", owner, module);
            result = (resolver_call_status)-1;
        } else {
            result = RESOLVER_CALL_OK;
        }
    }
    lua_settop(L, base);
    return result;
}

static int setup_host(cdin_backend *backend, cdin_lua_host *host, const char *root,
                      uint64_t source_limit, uint32_t module_limit)
{
    memset(backend, 0, sizeof(*backend));
    memset(host, 0, sizeof(*host));
#ifdef _WIN32
    backend->owner_thread = GetCurrentThreadId();
    host->extension_root_handle = NULL;
#else
    backend->owner_thread = pthread_self();
    host->extension_root_fd = -1;
#endif
    backend->owner_thread_valid = 1;
    backend->config.extension_source_root = root;
    backend->config.max_extension_source_bytes = source_limit;
    backend->config.max_extension_modules = module_limit;
    host->backend = backend;
    return cdin_lua_host_extension_resolver_init(host) == CDIN_BACKEND_OK ? 0 : -1;
}

static void cleanup_host(cdin_lua_host *host)
{
    cdin_lua_host_extension_resolver_destroy(host);
}

static int mock_runtime_load_extension(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    if (strcmp(name, "missing") == 0) {
        lua_createtable(L, 0, 1);
        lua_pushliteral(L, "NOT_FOUND");
        lua_setfield(L, -2, "code");
        return lua_error(L);
    }
    lua_createtable(L, 0, 3);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushliteral(L, "active");
    lua_setfield(L, -2, "state");
    lua_pushliteral(L, "resolver");
    lua_setfield(L, -2, "source");
    return 1;
}

static int mock_runtime_unload_extension(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    lua_pushboolean(L, strcmp(name, "demo") == 0);
    return 1;
}

static int test_trusted_runtime_extension_api(
    cdin_backend *backend, cdin_lua_host *host, lua_State *L)
{
    int runtime_ref;
    int result = -1;
    if (backend == NULL || host == NULL || L == NULL) return -1;

    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, mock_runtime_load_extension);
    lua_setfield(L, -2, "load_extension");
    lua_pushcfunction(L, mock_runtime_unload_extension);
    lua_setfield(L, -2, "unload_extension");
    runtime_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    host->state = L;
    host->runtime_ref = runtime_ref;
    host->started = 1;
    backend->runtime = host;
    backend->state = CDIN_BACKEND_STATE_RUNNING;

    if (cdin_backend_extension_load(backend, "demo") != CDIN_BACKEND_OK ||
        cdin_backend_extension_load(backend, "missing") != CDIN_BACKEND_NOT_FOUND ||
        cdin_backend_extension_unload(backend, "demo") != CDIN_BACKEND_OK ||
        cdin_backend_extension_unload(backend, "missing") != CDIN_BACKEND_NOT_FOUND ||
        cdin_backend_extension_load(backend, "") != CDIN_BACKEND_INVALID_ARGUMENT) {
        goto cleanup;
    }
    result = 0;

cleanup:
    backend->runtime = NULL;
    backend->state = CDIN_BACKEND_STATE_CREATED;
    host->started = 0;
    host->state = NULL;
    host->runtime_ref = LUA_NOREF;
    luaL_unref(L, LUA_REGISTRYINDEX, runtime_ref);
    return result;
}

static int test_resolver(void)
{
    char temp[512] = { 0 };
    char root[640], demo[640], helpers[640], large[640], outside[640];
    char path[768], link_path[768], secret_file[768];
    static const char init_source[] = "return { init = function() end }";
    static const char greeting_source[] = "return { value = 'hello' }\n";
    static const char small_source[] = "return true\n";
    static const char payload_source[] = "return 'this source is longer than the configured per-module read limit'\n";
    static const uint8_t binary_source[] = { 'r','e','t','u','r','n',' ', '1', 0u, 0xffu };
    cdin_backend backend;
    cdin_lua_host host;
    lua_State *L = NULL;
    int resolver_ref = LUA_NOREF;
    int symlink_created = 0;
    int collision_created = 0;
    int result = 1;

    if (make_temp_root(temp, sizeof(temp)) != 0 ||
        join_path(root, sizeof(root), temp, "extensions") != 0 ||
        join_path(demo, sizeof(demo), root, "demo") != 0 ||
        join_path(helpers, sizeof(helpers), demo, "helpers") != 0 ||
        join_path(large, sizeof(large), demo, "large") != 0 ||
        join_path(outside, sizeof(outside), temp, "outside") != 0 ||
        join_path(secret_file, sizeof(secret_file), outside, "secret.lua") != 0 ||
        make_dir(root) != 0 || make_dir(demo) != 0 ||
        make_dir(helpers) != 0 || make_dir(large) != 0 || make_dir(outside) != 0) {
        (void)fprintf(stderr, "could not create extension resolver test tree\n");
        goto cleanup_temp;
    }

    if (join_path(path, sizeof(path), demo, "init.lua") != 0 ||
        write_bytes(path, init_source, sizeof(init_source) - 1u) != 0 ||
        join_path(path, sizeof(path), helpers, "greeting.lua") != 0 ||
        write_bytes(path, greeting_source, sizeof(greeting_source) - 1u) != 0 ||
        join_path(path, sizeof(path), large, "payload.lua") != 0 ||
        write_bytes(path, payload_source, sizeof(payload_source) - 1u) != 0 ||
        join_path(path, sizeof(path), large, "small.lua") != 0 ||
        write_bytes(path, small_source, sizeof(small_source) - 1u) != 0 ||
        join_path(path, sizeof(path), demo, "binary.lua") != 0 ||
        write_bytes(path, binary_source, sizeof(binary_source)) != 0 ||
        write_bytes(secret_file, small_source, sizeof(small_source) - 1u) != 0 ||
        join_path(path, sizeof(path), large, "another.lua") != 0 ||
        write_bytes(path, small_source, sizeof(small_source) - 1u) != 0) {
        (void)fprintf(stderr, "could not create extension resolver test modules\n");
        goto cleanup_tree;
    }

    if (setup_host(&backend, &host, root, 48u, 4u) != 0) {
        (void)fprintf(stderr, "could not initialize extension resolver\n");
        goto cleanup_tree;
    }
    L = luaL_newstate();
    if (L == NULL) goto cleanup_host_and_tree;
    lua_pushlightuserdata(L, &host);
    lua_pushcclosure(L, lua_extension_module_resolver, 1);
    resolver_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (resolve_module(L, resolver_ref, "demo", "init",
                       (const uint8_t *)init_source, sizeof(init_source) - 1u,
                       "extension://demo/init", NULL) != RESOLVER_CALL_OK ||
        resolve_module(L, resolver_ref, "demo", "helpers.greeting",
                       (const uint8_t *)greeting_source, sizeof(greeting_source) - 1u,
                       "extension://demo/helpers.greeting", NULL) != RESOLVER_CALL_OK ||
        resolve_module(L, resolver_ref, "demo", "large.payload",
                       NULL, 0u, NULL, "LIMIT_EXCEEDED") != RESOLVER_CALL_ERROR ||
        resolve_module(L, resolver_ref, "demo", "binary",
                       binary_source, sizeof(binary_source),
                       "extension://demo/binary", NULL) != RESOLVER_CALL_OK ||
        resolve_module(L, resolver_ref, "demo", "missing",
                       NULL, 0u, NULL, NULL) != RESOLVER_CALL_MISSING ||
        resolve_module(L, resolver_ref, "../outside", "init",
                       NULL, 0u, NULL, "PERMISSION_DENIED") != RESOLVER_CALL_ERROR ||
        resolve_module(L, resolver_ref, "demo", "../outside",
                       NULL, 0u, NULL, "PERMISSION_DENIED") != RESOLVER_CALL_ERROR) {
        goto cleanup_host_and_tree;
    }

    if (test_trusted_runtime_extension_api(&backend, &host, L) != 0) {
        (void)fprintf(stderr, "trusted Runtime extension lifecycle API failed\n");
        goto cleanup_host_and_tree;
    }

    if (join_path(path, sizeof(path), demo, "linked.lua") != 0) goto cleanup_host_and_tree;
#ifdef _WIN32
    if (CreateSymbolicLinkA(path, secret_file, 0x2u)) {
        symlink_created = 1;
    }
#else
    if (symlink(secret_file, path) == 0) symlink_created = 1;
#endif
    if (symlink_created &&
        resolve_module(L, resolver_ref, "demo", "linked",
                       NULL, 0u, NULL, "PERMISSION_DENIED") != RESOLVER_CALL_ERROR) {
        goto cleanup_host_and_tree;
    }

#ifndef _WIN32
    /* POSIX can represent case-folding collisions. Reject both spellings
     * rather than choosing one based on directory enumeration order. */
    {
        char upper_path[768], lower_path[768];
        if (join_path(upper_path, sizeof(upper_path), demo, "Case.lua") == 0 &&
            join_path(lower_path, sizeof(lower_path), demo, "case.lua") == 0 &&
            write_bytes(upper_path, small_source, sizeof(small_source) - 1u) == 0 &&
            write_bytes(lower_path, small_source, sizeof(small_source) - 1u) == 0) {
            collision_created = 1;
            if (resolve_module(L, resolver_ref, "demo", "Case",
                               NULL, 0u, NULL, "PERMISSION_DENIED") != RESOLVER_CALL_ERROR) {
                goto cleanup_host_and_tree;
            }
        }
    }
#endif

    /* Four successful reads exhaust the configured per-host module-count
     * cap. Failed and missing reads do not consume a module slot. */
    if (resolve_module(L, resolver_ref, "demo", "large.small",
                       (const uint8_t *)small_source, sizeof(small_source) - 1u,
                       "extension://demo/large.small", NULL) != RESOLVER_CALL_OK ||
        resolve_module(L, resolver_ref, "demo", "large.another",
                       (const uint8_t *)small_source, sizeof(small_source) - 1u,
                       "extension://demo/large.another", "LIMIT_EXCEEDED") != RESOLVER_CALL_ERROR) {
        goto cleanup_host_and_tree;
    }

    /* With no configured source root, Runtime receives no resolver and
     * a defensive direct call still fails closed. */
    cleanup_host(&host);
    if (setup_host(&backend, &host, NULL, 48u, 4u) != 0 ||
        resolve_module(L, resolver_ref, "demo", "init",
                       NULL, 0u, NULL, "UNSUPPORTED") != RESOLVER_CALL_ERROR) {
        goto cleanup_host_and_tree;
    }
    result = 0;

    cleanup_host_and_tree:
    if (resolver_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, resolver_ref);
    if (L != NULL) lua_close(L);
    cleanup_host(&host);
    cleanup_tree:
    (void)join_path(path, sizeof(path), demo, "init.lua"); (void)remove_file(path);
    (void)join_path(path, sizeof(path), helpers, "greeting.lua"); (void)remove_file(path);
    (void)join_path(path, sizeof(path), large, "payload.lua"); (void)remove_file(path);
    (void)join_path(path, sizeof(path), large, "small.lua"); (void)remove_file(path);
    (void)join_path(path, sizeof(path), large, "another.lua"); (void)remove_file(path);
    (void)join_path(path, sizeof(path), demo, "binary.lua"); (void)remove_file(path);
    (void)remove_file(secret_file);
    (void)join_path(link_path, sizeof(link_path), demo, "linked.lua");
    (void)join_path(path, sizeof(path), demo, "Case.lua");
    (void)remove_file(path);
    (void)join_path(path, sizeof(path), demo, "case.lua");
    (void)remove_file(path);
    if (symlink_created) (void)remove_file(link_path);
    if (collision_created) {
        (void)join_path(path, sizeof(path), demo, "Case.lua"); (void)remove_file(path);
        (void)join_path(path, sizeof(path), demo, "case.lua"); (void)remove_file(path);
    }
    (void)remove_dir(helpers);
    (void)remove_dir(large);
    (void)remove_dir(demo);
    (void)remove_dir(outside);
    (void)remove_dir(root);
cleanup_temp:
    (void)remove_dir(temp);
    return result;
}

int main(void)
{
    if (test_resolver() != 0) return 1;
    puts("PASS extension_resolver: bounded owner-scoped loading, virtual chunk names, exact bytes, traversal denial and module-count limits");
    return 0;
}
