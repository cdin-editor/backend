/*
 * BE-005 keeps path behavior independent from Frontend/toolkit choices. The
 * Backend OS-services contract defines normalize/absolute/join/relative
 * operations and avoids hidden current-directory changes; lexical operations
 * here therefore do not mutate process-global working-directory state.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 */
#include "path.h"
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
#include <unistd.h>
#endif

static int is_separator(char value)
{
    return value == '/' || value == '\\';
}

static size_t root_length(const char *path)
{
#ifdef _WIN32
    if (isalpha((unsigned char)path[0]) && path[1] == ':' && is_separator(path[2])) return 3u;
    if (is_separator(path[0]) && is_separator(path[1]) && path[2] != '\0') {
        size_t i = 2u;
        while (path[i] != '\0' && !is_separator(path[i])) i++;
        if (path[i] == '\0') return i;
        i++;
        while (path[i] != '\0' && !is_separator(path[i])) i++;
        return i;
    }
#endif
    return is_separator(path[0]) ? 1u : 0u;
}

int cdin_path_is_absolute(const char *path)
{
    if (path == NULL || path[0] == '\0') return 0;
#ifdef _WIN32
    if (isalpha((unsigned char)path[0]) && path[1] == ':' && is_separator(path[2])) return 1;
    if (is_separator(path[0]) && is_separator(path[1])) return 1;
#endif
    return path[0] == '/';
}

int cdin_path_normalize(const char *path, char **out_path)
{
    size_t len, root, i, out_len, segment_count = 0u;
    size_t *starts = NULL;
    char *result = NULL;
    int absolute;
    if (path == NULL || out_path == NULL) return -1;
    *out_path = NULL;
    len = strlen(path);
    if (len > SIZE_MAX - 2u || len > SIZE_MAX / sizeof(size_t) - 1u) return -1;
    root = root_length(path);
    absolute = cdin_path_is_absolute(path);
    starts = (size_t *)calloc(len + 1u, sizeof(size_t));
    result = (char *)malloc(len + 4u);
    if (starts == NULL || result == NULL) goto fail;
#ifdef _WIN32
    if (root == 3u) {
        result[0] = (char)toupper((unsigned char)path[0]);
        result[1] = ':';
        result[2] = '/';
        out_len = 3u;
        i = 3u;
    } else if (root >= 2u && is_separator(path[0]) && is_separator(path[1])) {
        result[0] = '/'; result[1] = '/'; out_len = 2u; i = 2u;
    } else
#endif
    {
        out_len = absolute ? 1u : 0u;
        if (absolute) result[0] = '/';
        i = root;
    }
    while (i < len) {
        size_t start;
        size_t segment_len;
        while (i < len && is_separator(path[i])) i++;
        if (i >= len) break;
        start = i;
        while (i < len && !is_separator(path[i])) i++;
        segment_len = i - start;
        if (segment_len == 1u && path[start] == '.') continue;
        if (segment_len == 2u && path[start] == '.' && path[start + 1u] == '.') {
            if (segment_count != 0u) {
                out_len = starts[--segment_count];
                if (out_len > 0u && result[out_len - 1u] == '/' &&
                    !(out_len == 3u && result[1] == ':')) out_len--;
            } else if (!absolute) {
                if (out_len != 0u && result[out_len - 1u] != '/') result[out_len++] = '/';
                result[out_len++] = '.'; result[out_len++] = '.';
                starts[segment_count++] = out_len;
            }
            continue;
        }
        if (out_len != 0u && result[out_len - 1u] != '/') result[out_len++] = '/';
        starts[segment_count++] = out_len;
        memcpy(result + out_len, path + start, segment_len);
        out_len += segment_len;
    }
    if (out_len == 0u) result[out_len++] = '.';
    if (out_len > 1u && result[out_len - 1u] == '/' && !(out_len == 3u && result[1] == ':')) out_len--;
    result[out_len] = '\0';
    free(starts);
    *out_path = result;
    return 0;
fail:
    free(starts);
    free(result);
    return -1;
}

int cdin_path_absolute(const char *path, char **out_path)
{
    char *joined = NULL;
    char *cwd = NULL;
    int result;
    if (path == NULL || out_path == NULL) return -1;
    *out_path = NULL;
#ifdef _WIN32
    if (isalpha((unsigned char)path[0]) && path[1] == ':' &&
        !is_separator(path[2])) return -1; /* Drive-relative paths depend on hidden per-drive cwd state. */
#endif
    if (cdin_path_is_absolute(path)) return cdin_path_normalize(path, out_path);
#ifdef _WIN32
    {
        DWORD required = GetCurrentDirectoryW(0u, NULL);
        wchar_t *wide_cwd;
        if (required == 0u) return -1;
#if SIZE_MAX <= UINT32_MAX
        /* DWORD can approach UINT32_MAX on 32-bit Windows. Keep the size_t
         * allocation check only on targets where that range can overflow. */
        if ((uint64_t)required + UINT64_C(1) >
            (uint64_t)(SIZE_MAX / sizeof(wchar_t))) return -1;
#endif
        wide_cwd = (wchar_t *)calloc((size_t)required + 1u, sizeof(wchar_t));
        if (wide_cwd == NULL) return -1;
        if (GetCurrentDirectoryW(required + 1u, wide_cwd) == 0u) {
            free(wide_cwd);
            return -1;
        }
        cwd = cdin_wide_to_utf8(wide_cwd);
        free(wide_cwd);
        if (cwd == NULL) return -1;
    }
#else
    {
        size_t cap = 256u;
        for (;;) {
            char *next = (char *)realloc(cwd, cap);
            if (next == NULL) { free(cwd); return -1; }
            cwd = next;
            if (getcwd(cwd, cap) != NULL) break;
            if (errno != ERANGE || cap > SIZE_MAX / 2u) { free(cwd); return -1; }
            cap *= 2u;
        }
    }
#endif
    result = cdin_path_join(cwd, path, &joined);
    free(cwd);
    if (result != 0) return -1;
    *out_path = joined;
    return 0;
}

int cdin_path_join(const char *left, const char *right, char **out_path)
{
    size_t a, b, need;
    char *combined;
    if (left == NULL || right == NULL || out_path == NULL) return -1;
    *out_path = NULL;
#ifdef _WIN32
    if (isalpha((unsigned char)right[0]) && right[1] == ':' &&
        !is_separator(right[2])) return -1;
#endif
    if (cdin_path_is_absolute(right)) return cdin_path_normalize(right, out_path);
    a = strlen(left); b = strlen(right);
    if (b > SIZE_MAX - 2u || a > SIZE_MAX - b - 2u) return -1;
    need = a + b + 2u;
    combined = (char *)malloc(need);
    if (combined == NULL) return -1;
    memcpy(combined, left, a);
    if (a != 0u && !is_separator(left[a - 1u])) combined[a++] = '/';
    memcpy(combined + a, right, b + 1u);
    if (cdin_path_normalize(combined, out_path) != 0) { free(combined); return -1; }
    free(combined);
    return 0;
}

static int same_root(const char *a, const char *b)
{
#ifdef _WIN32
    {
        int a_drive = isalpha((unsigned char)a[0]) && a[1] == ':';
        int b_drive = isalpha((unsigned char)b[0]) && b[1] == ':';
        int a_unc = a[0] == '/' && a[1] == '/';
        int b_unc = b[0] == '/' && b[1] == '/';
        if (a_drive || b_drive) {
            return a_drive && b_drive &&
                   tolower((unsigned char)a[0]) == tolower((unsigned char)b[0]);
        }
        if (a_unc || b_unc) {
            size_t a_root = root_length(a);
            size_t b_root = root_length(b);
            size_t i;
            if (!a_unc || !b_unc || a_root != b_root) return 0;
            for (i = 0u; i < a_root; i++) {
                if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
            }
            return 1;
        }
    }
#endif
    return (a[0] == '/') == (b[0] == '/');
}

int cdin_path_relative(const char *from_directory, const char *to_path, char **out_path)
{
    char *from = NULL, *to = NULL, *result = NULL;
    size_t i = 0u, common = 0u, out = 0u, from_len, to_len;
    size_t up = 0u, needed;
    if (from_directory == NULL || to_path == NULL || out_path == NULL) return -1;
    *out_path = NULL;
    if (cdin_path_absolute(from_directory, &from) != 0 || cdin_path_absolute(to_path, &to) != 0) goto fail;
    if (!same_root(from, to)) goto fail;
    from_len = strlen(from);
    to_len = strlen(to);
#ifdef _WIN32
    while (i < from_len && i < to_len && tolower((unsigned char)from[i]) == tolower((unsigned char)to[i])) i++;
#else
    while (i < from_len && i < to_len && from[i] == to[i]) i++;
#endif
    common = i;
    if (common != 0u && common < from_len && common < to_len &&
        from[common - 1u] != '/' && from[common] != '/' && to[common] != '/') {
        while (common > 0u && from[common - 1u] != '/') common--;
    } else if (common != 0u && common < from_len && from[common] != '/') {
        while (common > 0u && from[common - 1u] != '/') common--;
    }
    i = common;
    while (i < from_len) {
        while (i < from_len && from[i] == '/') i++;
        if (i >= from_len) break;
        up++;
        while (i < from_len && from[i] != '/') i++;
    }
    i = common;
    while (i < to_len && to[i] == '/') i++;
    if (to_len - i > SIZE_MAX - 2u ||
        up > (SIZE_MAX - (to_len - i) - 2u) / 3u) goto fail;
    needed = up * 3u + (to_len - i) + 2u;
    result = (char *)malloc(needed);
    if (result == NULL) goto fail;
    while (up > 0u) {
        if (out != 0u) result[out++] = '/';
        result[out++] = '.';
        result[out++] = '.';
        up--;
    }
    if (i < to_len) {
        if (out != 0u) result[out++] = '/';
        memcpy(result + out, to + i, to_len - i);
        out += to_len - i;
    }
    if (out == 0u) result[out++] = '.';
    result[out] = '\0';
    free(from);
    free(to);
    *out_path = result;
    return 0;
fail:
    free(from);
    free(to);
    free(result);
    return -1;
}

const char *cdin_path_basename(const char *path)
{
    const char *base;
    const char *cursor;
    if (path == NULL) return "";
    base = path;
    for (cursor = path; *cursor != '\0'; cursor++) if (is_separator(*cursor)) base = cursor + 1;
    return base;
}
