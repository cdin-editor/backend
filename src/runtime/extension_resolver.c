/*
 * Backend-owned extension module resolver.
 *
 * A configured root is trusted application input. Each direct child directory
 * is one extension namespace. Resolution is fail-closed when no root is set.
 * POSIX traversal is descriptor-relative and refuses symlinks at every child.
 * Windows opens reparse points explicitly, rejects them, and verifies the final
 * path of the opened handle remains inside the canonical extension root.
 */
#include "lua_host_internal.h"
#include "../platform/native_path.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef enum resolver_result {
    RESOLVER_OK = 0,
    RESOLVER_NOT_FOUND,
    RESOLVER_DENIED,
    RESOLVER_LIMIT,
    RESOLVER_NO_MEMORY,
    RESOLVER_IO
} resolver_result;

#ifndef _WIN32
static int ascii_equal_fold(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (tolower(a) != tolower(b)) return 0;
    }
    return *left == '\0' && *right == '\0';
}
#endif

static int valid_plugin_name(const char *value)
{
    size_t i;
    size_t length;
    if (value == NULL) return 0;
    length = strlen(value);
    if (length == 0u || length > 128u ||
        value[0] == '.' || value[length - 1u] == '.' ||
        strstr(value, "..") != NULL) return 0;
    for (i = 0u; i < length; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch >= 0x80u || !(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) return 0;
    }
    return 1;
}

static int valid_module_name(const char *value)
{
    size_t i;
    size_t length;
    int segment_has_character = 0;
    if (value == NULL) return 0;
    length = strlen(value);
    if (length == 0u || length > 256u) return 0;
    for (i = 0u; i < length; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch == '.') {
            if (!segment_has_character) return 0;
            segment_has_character = 0;
            continue;
        }
        if (!(isalnum(ch) || ch == '_' || ch == '-')) return 0;
        segment_has_character = 1;
    }
    return segment_has_character;
}

static const char *resolver_runtime_code(resolver_result result)
{
    switch (result) {
        case RESOLVER_DENIED: return "PERMISSION_DENIED";
        case RESOLVER_LIMIT: return "LIMIT_EXCEEDED";
        case RESOLVER_NO_MEMORY: return "NO_MEMORY";
        case RESOLVER_IO: return "IO_ERROR";
        case RESOLVER_NOT_FOUND: return "NOT_FOUND";
        case RESOLVER_OK: break;
    }
    return "INTERNAL";
}

static int raise_resolver_error(lua_State *L, const char *code,
                                const char *message, const char *owner,
                                const char *module)
{
    lua_createtable(L, 0, 4);
    lua_pushliteral(L, "cdin.runtime");
    lua_setfield(L, -2, "domain");
    lua_pushstring(L, code);
    lua_setfield(L, -2, "code");
    lua_pushstring(L, message);
    lua_setfield(L, -2, "message");
    lua_createtable(L, 0, 2);
    if (owner != NULL) {
        lua_pushstring(L, owner);
        lua_setfield(L, -2, "extension");
    }
    if (module != NULL) {
        lua_pushstring(L, module);
        lua_setfield(L, -2, "module");
    }
    lua_setfield(L, -2, "details");
    return lua_error(L);
}

static int raise_resolver_result(lua_State *L, resolver_result result,
                                 const char *owner, const char *module)
{
    const char *message = "extension module resolution failed";
    if (result == RESOLVER_DENIED) message = "extension module path is not permitted";
    else if (result == RESOLVER_LIMIT) message = "extension source exceeds configured limits";
    else if (result == RESOLVER_NO_MEMORY) message = "could not allocate extension source buffer";
    else if (result == RESOLVER_IO) message = "could not read extension module";
    return raise_resolver_error(L, resolver_runtime_code(result), message, owner, module);
}

#ifdef _WIN32

static wchar_t *append_wide_component(const wchar_t *parent, const char *component)
{
    size_t parent_length;
    size_t component_length;
    wchar_t *result;
    size_t i;
    if (parent == NULL || component == NULL) return NULL;
    parent_length = wcslen(parent);
    component_length = strlen(component);
    if (parent_length > SIZE_MAX - component_length - 2u) return NULL;
    result = (wchar_t *)calloc(parent_length + component_length + 2u, sizeof(wchar_t));
    if (result == NULL) return NULL;
    memcpy(result, parent, parent_length * sizeof(wchar_t));
    if (parent_length != 0u && result[parent_length - 1u] != L'\\') {
        result[parent_length++] = L'\\';
    }
    for (i = 0u; i < component_length; i++) {
        unsigned char ch = (unsigned char)component[i];
        if (ch >= 0x80u) {
            free(result);
            return NULL;
        }
        result[parent_length + i] = (wchar_t)ch;
    }
    return result;
}

static int wide_path_within_root(const cdin_lua_host *host, const wchar_t *path)
{
    size_t root_length;
    size_t path_length;
    if (host == NULL || host->extension_root_wide == NULL || path == NULL) return 0;
    root_length = wcslen(host->extension_root_wide);
    path_length = wcslen(path);
    while (root_length > 0u && host->extension_root_wide[root_length - 1u] == L'\\' &&
           !(root_length == 3u && host->extension_root_wide[1] == L':')) {
        root_length--;
    }
    if (path_length <= root_length ||
        CompareStringOrdinal(host->extension_root_wide, (int)root_length,
                             path, (int)root_length, TRUE) != CSTR_EQUAL) return 0;
    return path[root_length] == L'\\';
}

static resolver_result verify_handle_path(const cdin_lua_host *host, HANDLE handle)
{
    DWORD required;
    DWORD written;
    wchar_t *final_path;
    resolver_result result = RESOLVER_IO;
    required = GetFinalPathNameByHandleW(handle, NULL, 0u,
                                         FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0u) return RESOLVER_IO;
    if ((size_t)required > SIZE_MAX / sizeof(wchar_t) - 1u) return RESOLVER_LIMIT;
    final_path = (wchar_t *)calloc((size_t)required + 1u, sizeof(wchar_t));
    if (final_path == NULL) return RESOLVER_NO_MEMORY;
    written = GetFinalPathNameByHandleW(handle, final_path, required + 1u,
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0u || written > required) {
        result = RESOLVER_IO;
    } else if (!wide_path_within_root(host, final_path)) {
        result = RESOLVER_DENIED;
    } else {
        result = RESOLVER_OK;
    }
    free(final_path);
    return result;
}

static resolver_result windows_check_exact_name(const wchar_t *parent,
                                                const char *component)
{
    wchar_t *candidate = NULL;
    wchar_t *pattern = NULL;
    WIN32_FIND_DATAW data;
    HANDLE find = INVALID_HANDLE_VALUE;
    int folded_matches = 0;
    int exact_matches = 0;
    resolver_result result = RESOLVER_NOT_FOUND;
    size_t i;
    size_t parent_length;
    if (parent == NULL || component == NULL) return RESOLVER_DENIED;
    candidate = (wchar_t *)calloc(strlen(component) + 1u, sizeof(wchar_t));
    if (candidate == NULL) return RESOLVER_NO_MEMORY;
    for (i = 0u; component[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)component[i];
        if (ch >= 0x80u) {
            free(candidate);
            return RESOLVER_DENIED;
        }
        candidate[i] = (wchar_t)ch;
    }
    parent_length = wcslen(parent);
    pattern = (wchar_t *)calloc(parent_length + 3u, sizeof(wchar_t));
    if (pattern == NULL) {
        free(candidate);
        return RESOLVER_NO_MEMORY;
    }
    memcpy(pattern, parent, parent_length * sizeof(wchar_t));
    if (parent_length != 0u && pattern[parent_length - 1u] != L'\\') {
        pattern[parent_length++] = L'\\';
    }
    pattern[parent_length++] = L'*';
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        result = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            ? RESOLVER_NOT_FOUND : RESOLVER_IO;
        goto done;
    }
    result = RESOLVER_NOT_FOUND;
    do {
        if (CompareStringOrdinal(data.cFileName, -1, candidate, -1, TRUE) == CSTR_EQUAL) {
            folded_matches++;
            if (CompareStringOrdinal(data.cFileName, -1, candidate, -1, FALSE) == CSTR_EQUAL) {
                exact_matches++;
            }
        }
    } while (FindNextFileW(find, &data));
    if (folded_matches == 1 && exact_matches == 1) result = RESOLVER_OK;
    else if (folded_matches != 0) result = RESOLVER_DENIED;
done:
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    free(pattern);
    free(candidate);
    return result;
}

static resolver_result windows_open_child_directory(cdin_lua_host *host,
                                                     const wchar_t *parent,
                                                     const char *component,
                                                     wchar_t **out_path)
{
    wchar_t *path;
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;
    resolver_result result;
    if (out_path == NULL) return RESOLVER_IO;
    *out_path = NULL;
    result = windows_check_exact_name(parent, component);
    if (result != RESOLVER_OK) return result;
    path = append_wide_component(parent, component);
    if (path == NULL) return RESOLVER_NO_MEMORY;
    handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        free(path);
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return RESOLVER_NOT_FOUND;
        if (error == ERROR_CANT_ACCESS_FILE || error == ERROR_INVALID_REPARSE_DATA) return RESOLVER_DENIED;
        return RESOLVER_IO;
    }
    if (!GetFileInformationByHandle(handle, &info)) {
        CloseHandle(handle);
        free(path);
        return RESOLVER_IO;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        CloseHandle(handle);
        free(path);
        return RESOLVER_DENIED;
    }
    result = verify_handle_path(host, handle);
    CloseHandle(handle);
    if (result != RESOLVER_OK) {
        free(path);
        return result;
    }
    *out_path = path;
    return RESOLVER_OK;
}

static resolver_result windows_read_module(cdin_lua_host *host,
                                           const char *owner,
                                           const char *module,
                                           char **out_source,
                                           size_t *out_length)
{
    wchar_t *current = NULL;
    wchar_t *next = NULL;
    wchar_t *file_path = NULL;
    size_t module_length = strlen(module);
    size_t start = 0u;
    size_t offset = 0u;
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info;
    DWORD file_size_high;
    DWORD file_size_low;
    uint64_t size;
    char *source = NULL;
    size_t total = 0u;
    resolver_result result = RESOLVER_IO;
    current = (wchar_t *)calloc(wcslen(host->extension_root_wide) + 1u, sizeof(wchar_t));
    if (current == NULL) return RESOLVER_NO_MEMORY;
    wcscpy(current, host->extension_root_wide);
    result = windows_open_child_directory(host, current, owner, &next);
    free(current);
    current = next;
    next = NULL;
    if (result != RESOLVER_OK) goto done;

    while (offset < module_length) {
        size_t segment_length;
        char segment[257];
        while (offset < module_length && module[offset] != '.') offset++;
        segment_length = offset - start;
        if (segment_length == 0u || segment_length >= sizeof(segment)) {
            result = RESOLVER_DENIED;
            goto done;
        }
        memcpy(segment, module + start, segment_length);
        segment[segment_length] = '\0';
        start = ++offset;
        if (offset < module_length) {
            result = windows_open_child_directory(host, current, segment, &next);
            if (result != RESOLVER_OK) goto done;
            free(current);
            current = next;
            next = NULL;
        } else {
            size_t file_component_length = segment_length + sizeof(".lua");
            char *file_component = (char *)malloc(file_component_length);
            if (file_component == NULL) {
                result = RESOLVER_NO_MEMORY;
                goto done;
            }
            memcpy(file_component, segment, segment_length);
            memcpy(file_component + segment_length, ".lua", sizeof(".lua"));
            result = windows_check_exact_name(current, file_component);
            if (result != RESOLVER_OK) {
                free(file_component);
                goto done;
            }
            file_path = append_wide_component(current, file_component);
            free(file_component);
            if (file_path == NULL) {
                result = RESOLVER_NO_MEMORY;
                goto done;
            }
            handle = CreateFileW(file_path, GENERIC_READ | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            if (handle == INVALID_HANDLE_VALUE) {
                DWORD error = GetLastError();
                result = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    ? RESOLVER_NOT_FOUND : RESOLVER_IO;
                goto done;
            }
            if (!GetFileInformationByHandle(handle, &info)) {
                result = RESOLVER_IO;
                goto done;
            }
            if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0u ||
                info.nNumberOfLinks != 1u) {
                result = RESOLVER_DENIED;
                goto done;
            }
            result = verify_handle_path(host, handle);
            if (result != RESOLVER_OK) goto done;
            file_size_high = info.nFileSizeHigh;
            file_size_low = info.nFileSizeLow;
            size = ((uint64_t)file_size_high << 32u) | (uint64_t)file_size_low;
            if (size > host->extension_source_limit ||
                size > (uint64_t)(SIZE_MAX - 1u)) {
                result = RESOLVER_LIMIT;
                goto done;
            }
            source = (char *)malloc((size_t)size + 1u);
            if (source == NULL) {
                result = RESOLVER_NO_MEMORY;
                goto done;
            }
            while (total < (size_t)size) {
                DWORD amount = 0u;
                DWORD request = (DWORD)(((size_t)size - total) > 65536u
                    ? 65536u : ((size_t)size - total));
                if (!ReadFile(handle, source + total, request, &amount, NULL) || amount == 0u) {
                    result = RESOLVER_IO;
                    goto done;
                }
                total += (size_t)amount;
            }
            {
                char extra;
                DWORD amount = 0u;
                if (!ReadFile(handle, &extra, 1u, &amount, NULL)) {
                    result = RESOLVER_IO;
                    goto done;
                }
                if (amount != 0u) {
                    result = RESOLVER_LIMIT;
                    goto done;
                }
            }
            source[total] = '\0';
            result = RESOLVER_OK;
        }
    }
done:
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    free(current);
    free(next);
    free(file_path);
    if (result != RESOLVER_OK) {
        free(source);
        return result;
    }
    *out_source = source;
    *out_length = total;
    return RESOLVER_OK;
}

#else

static int ascii_component_match(const char *entry, const char *candidate)
{
    return strcmp(entry, candidate) == 0;
}

static resolver_result posix_check_exact_name(int directory_fd, const char *component)
{
    int duplicate = dup(directory_fd);
    DIR *directory;
    struct dirent *entry;
    size_t folded_matches = 0u;
    size_t exact_matches = 0u;
    if (duplicate < 0) return RESOLVER_IO;
    directory = fdopendir(duplicate);
    if (directory == NULL) {
        close(duplicate);
        return RESOLVER_IO;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (ascii_equal_fold(entry->d_name, component)) {
            folded_matches++;
            if (ascii_component_match(entry->d_name, component)) exact_matches++;
        }
    }
    if (errno != 0) {
        closedir(directory);
        return RESOLVER_IO;
    }
    closedir(directory);
    if (folded_matches == 1u && exact_matches == 1u) return RESOLVER_OK;
    if (folded_matches != 0u) return RESOLVER_DENIED;
    return RESOLVER_NOT_FOUND;
}

static resolver_result posix_open_child_directory(int parent_fd,
                                                  const char *component,
                                                  int *out_fd)
{
    int descriptor;
    resolver_result result;
    if (out_fd == NULL) return RESOLVER_IO;
    *out_fd = -1;
    result = posix_check_exact_name(parent_fd, component);
    if (result != RESOLVER_OK) return result;
    descriptor = openat(parent_fd, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno == ENOENT) return RESOLVER_NOT_FOUND;
        if (errno == ELOOP || errno == ENOTDIR) return RESOLVER_DENIED;
        return RESOLVER_IO;
    }
    *out_fd = descriptor;
    return RESOLVER_OK;
}

static resolver_result posix_read_module(cdin_lua_host *host,
                                         const char *owner,
                                         const char *module,
                                         char **out_source,
                                         size_t *out_length)
{
    char *module_copy = NULL;
    char *segment;
    char *save = NULL;
    int current_fd = -1;
    int next_fd = -1;
    int file_fd = -1;
    struct stat info;
    char *source = NULL;
    size_t total = 0u;
    resolver_result result;
    module_copy = strdup(module);
    if (module_copy == NULL) return RESOLVER_NO_MEMORY;
    current_fd = dup(host->extension_root_fd);
    if (current_fd < 0) {
        free(module_copy);
        return RESOLVER_IO;
    }
    result = posix_open_child_directory(current_fd, owner, &next_fd);
    close(current_fd);
    current_fd = next_fd;
    next_fd = -1;
    if (result != RESOLVER_OK) goto done;
    segment = strtok_r(module_copy, ".", &save);
    while (segment != NULL) {
        char *following = strtok_r(NULL, ".", &save);
        if (following != NULL) {
            result = posix_open_child_directory(current_fd, segment, &next_fd);
            if (result != RESOLVER_OK) goto done;
            close(current_fd);
            current_fd = next_fd;
            next_fd = -1;
        } else {
            size_t component_length = strlen(segment);
            char *filename;
            if (component_length > SIZE_MAX - 5u) {
                result = RESOLVER_LIMIT;
                goto done;
            }
            filename = (char *)malloc(component_length + 5u);
            if (filename == NULL) {
                result = RESOLVER_NO_MEMORY;
                goto done;
            }
            memcpy(filename, segment, component_length);
            memcpy(filename + component_length, ".lua", 5u);
            result = posix_check_exact_name(current_fd, filename);
            if (result != RESOLVER_OK) {
                free(filename);
                goto done;
            }
            file_fd = openat(current_fd, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            free(filename);
            if (file_fd < 0) {
                result = (errno == ENOENT) ? RESOLVER_NOT_FOUND :
                    ((errno == ELOOP || errno == EISDIR || errno == ENOTDIR)
                        ? RESOLVER_DENIED : RESOLVER_IO);
                goto done;
            }
            if (fstat(file_fd, &info) != 0) {
                result = RESOLVER_IO;
                goto done;
            }
            if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
                result = RESOLVER_DENIED;
                goto done;
            }
            if (info.st_size < 0 || (uint64_t)info.st_size > host->extension_source_limit ||
                (uint64_t)info.st_size > (uint64_t)(SIZE_MAX - 1u)) {
                result = RESOLVER_LIMIT;
                goto done;
            }
            source = (char *)malloc((size_t)info.st_size + 1u);
            if (source == NULL) {
                result = RESOLVER_NO_MEMORY;
                goto done;
            }
            while (total <= (size_t)info.st_size) {
                ssize_t amount = read(file_fd, source + total,
                                      (size_t)info.st_size + 1u - total);
                if (amount > 0) {
                    total += (size_t)amount;
                    if (total > host->extension_source_limit) {
                        result = RESOLVER_LIMIT;
                        goto done;
                    }
                    if (total > (size_t)info.st_size) break;
                    continue;
                }
                if (amount == 0) break;
                if (errno == EINTR) continue;
                result = RESOLVER_IO;
                goto done;
            }
            if (total == 0u && info.st_size != 0) {
                result = RESOLVER_IO;
                goto done;
            }
            if (total != (size_t)info.st_size) {
                /* File changed while it was being read; do not compile a
                 * truncated or extended version of the module. */
                result = RESOLVER_IO;
                goto done;
            }
            source[total] = '\0';
            result = RESOLVER_OK;
        }
        segment = following;
    }
done:
    if (file_fd >= 0) close(file_fd);
    if (current_fd >= 0) close(current_fd);
    if (next_fd >= 0) close(next_fd);
    free(module_copy);
    if (result != RESOLVER_OK) {
        free(source);
        return result;
    }
    *out_source = source;
    *out_length = total;
    return RESOLVER_OK;
}

#endif

cdin_backend_status cdin_lua_host_extension_resolver_init(cdin_lua_host *host)
{
    const char *configured_root;
    if (host == NULL || host->backend == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    configured_root = host->backend->config.extension_source_root;
#ifdef _WIN32
    host->extension_root_handle = NULL;
    host->extension_root_wide = NULL;
#else
    host->extension_root_fd = -1;
#endif
    host->extension_root_path = NULL;
    host->extension_source_total = 0u;
    host->extension_module_count = 0u;
    host->extension_source_limit = host->backend->config.max_extension_source_bytes;
    host->extension_module_limit = host->backend->config.max_extension_modules;
    host->extension_source_total_limit =
        host->extension_source_limit * (uint64_t)host->extension_module_limit;
    if (configured_root == NULL) return CDIN_BACKEND_OK;
#ifdef _WIN32
    {
        wchar_t *input_path = cdin_utf8_to_wide(configured_root);
        HANDLE handle;
        BY_HANDLE_FILE_INFORMATION info;
        DWORD required;
        DWORD written;
        wchar_t *canonical;
        if (input_path == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
        handle = CreateFileW(input_path, FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        free(input_path);
        if (handle == INVALID_HANDLE_VALUE) return CDIN_BACKEND_IO_ERROR;
        if (!GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            CloseHandle(handle);
            return CDIN_BACKEND_INVALID_ARGUMENT;
        }
        required = GetFinalPathNameByHandleW(handle, NULL, 0u,
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0u || (size_t)required > SIZE_MAX / sizeof(wchar_t) - 1u) {
            CloseHandle(handle);
            return CDIN_BACKEND_IO_ERROR;
        }
        canonical = (wchar_t *)calloc((size_t)required + 1u, sizeof(wchar_t));
        if (canonical == NULL) {
            CloseHandle(handle);
            return CDIN_BACKEND_NO_MEMORY;
        }
        written = GetFinalPathNameByHandleW(handle, canonical, required + 1u,
                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0u || written > required) {
            free(canonical);
            CloseHandle(handle);
            return CDIN_BACKEND_IO_ERROR;
        }
        host->extension_root_path = cdin_wide_to_utf8(canonical);
        if (host->extension_root_path == NULL) {
            free(canonical);
            CloseHandle(handle);
            return CDIN_BACKEND_NO_MEMORY;
        }
        host->extension_root_wide = canonical;
        host->extension_root_handle = handle;
    }
#else
    {
        char *canonical = realpath(configured_root, NULL);
        int descriptor;
        struct stat info;
        if (canonical == NULL) return CDIN_BACKEND_IO_ERROR;
        descriptor = open(canonical, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) {
            free(canonical);
            return CDIN_BACKEND_INVALID_ARGUMENT;
        }
        if (fstat(descriptor, &info) != 0 || !S_ISDIR(info.st_mode)) {
            close(descriptor);
            free(canonical);
            return CDIN_BACKEND_INVALID_ARGUMENT;
        }
        host->extension_root_path = canonical;
        host->extension_root_fd = descriptor;
    }
#endif
    CDIN_BACKEND_INFO(host->backend->kernel, "RUNTIME",
                      "extension resolver enabled source_limit=%llu module_limit=%u",
                      (unsigned long long)host->extension_source_limit,
                      (unsigned)host->extension_module_limit);
    return CDIN_BACKEND_OK;
}

void cdin_lua_host_extension_resolver_destroy(cdin_lua_host *host)
{
    if (host == NULL) return;
#ifdef _WIN32
    if (host->extension_root_handle != NULL &&
        host->extension_root_handle != INVALID_HANDLE_VALUE) {
        CloseHandle((HANDLE)host->extension_root_handle);
        host->extension_root_handle = NULL;
    }
    free(host->extension_root_wide);
#else
    if (host->extension_root_fd >= 0) {
        close(host->extension_root_fd);
        host->extension_root_fd = -1;
    }
#endif
    free(host->extension_root_path);
    host->extension_root_path = NULL;
}

int lua_extension_module_resolver(lua_State *L)
{
    cdin_lua_host *host = (cdin_lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    const char *owner;
    const char *module;
    size_t owner_length = 0u;
    size_t module_length = 0u;
    char *source = NULL;
    size_t source_length = 0u;
    resolver_result result;
    uint64_t next_total;
    if (host == NULL || host->backend == NULL || host->extension_root_path == NULL) {
        return raise_resolver_error(L, "UNSUPPORTED",
                                    "extension loading is not enabled by Backend",
                                    NULL, NULL);
    }
    if (!cdin_backend_is_owner(host->backend)) {
        return raise_resolver_error(L, "INVALID_STATE",
                                    "extension sources must be resolved on the Runtime owner thread",
                                    NULL, NULL);
    }
    owner = luaL_checklstring(L, 1, &owner_length);
    module = luaL_checklstring(L, 2, &module_length);
    if (owner_length == 0u || owner_length > 128u || module_length == 0u ||
        module_length > 256u || strlen(owner) != owner_length ||
        strlen(module) != module_length || !valid_plugin_name(owner) ||
        !valid_module_name(module)) {
        return raise_resolver_error(L, "PERMISSION_DENIED",
                                    "extension name or module identifier is invalid",
                                    owner, module);
    }
    if (host->extension_module_count >= host->extension_module_limit) {
        return raise_resolver_error(L, "LIMIT_EXCEEDED",
                                    "extension module-count budget is exhausted",
                                    owner, module);
    }
#ifdef _WIN32
    result = windows_read_module(host, owner, module, &source, &source_length);
#else
    result = posix_read_module(host, owner, module, &source, &source_length);
#endif
    if (result == RESOLVER_NOT_FOUND) {
        lua_pushnil(L);
        return 1;
    }
    if (result != RESOLVER_OK) return raise_resolver_result(L, result, owner, module);
    if ((uint64_t)source_length > host->extension_source_total_limit - host->extension_source_total) {
        free(source);
        return raise_resolver_error(L, "LIMIT_EXCEEDED",
                                    "extension source aggregate budget is exhausted",
                                    owner, module);
    }
    next_total = host->extension_source_total + (uint64_t)source_length;
    host->extension_module_count++;
    host->extension_source_total = next_total;
    lua_pushlstring(L, source, source_length);
    lua_pushfstring(L, "extension://%s/%s", owner, module);
    free(source);
    return 2;
}
