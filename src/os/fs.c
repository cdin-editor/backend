/*
 * BE-005 requires file services to stay independent of display toolkits;
 * BE-008 and ARCH-021 require bounded work and buffers. Chunked reads cap
 * temporary memory, while same-directory atomic replacement prevents readers
 * from observing a partially written destination.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md
 */
#include "fs.h"
#include "path.h"
#include "../platform/native_path.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdatomic.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <process.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

struct cdin_atomic_writer {
    FILE *file;
    char *destination;
    char *temporary;
    uint64_t max_bytes;
    uint64_t written;
    int overwrite;
    int closed;
};

static atomic_ullong writer_sequence = 0u;

static void cdin_fs_remove_native(const char *path);

static FILE *cdin_fopen_utf8(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wide_path = cdin_utf8_to_wide(path);
    wchar_t wide_mode[8];
    size_t i;
    FILE *file = NULL;
    if (wide_path == NULL) return NULL;
    for (i = 0u; mode[i] != '\0' && i + 1u < sizeof(wide_mode) / sizeof(wide_mode[0]); i++) wide_mode[i] = (wchar_t)(unsigned char)mode[i];
    wide_mode[i] = L'\0';
    file = _wfopen(wide_path, wide_mode);
    free(wide_path);
    return file;
#else
    return fopen(path, mode);
#endif
}

int cdin_fs_read_chunks(const char *path, size_t chunk_size, uint64_t max_bytes,
                        cdin_fs_chunk_fn callback, void *context, uint64_t *out_total)
{
    FILE *file = NULL;
    uint8_t *buffer = NULL;
    uint64_t total = 0u;
    int result = -1;
    if (path == NULL || callback == NULL || chunk_size == 0u || max_bytes == 0u) return -1;
    if (out_total != NULL) *out_total = 0u;
    file = cdin_fopen_utf8(path, "rb");
    if (file == NULL) goto done;
    buffer = (uint8_t *)malloc(chunk_size);
    if (buffer == NULL) goto done;
    for (;;) {
        size_t count = fread(buffer, 1u, chunk_size, file);
        if (count != 0u) {
            if ((uint64_t)count > max_bytes - total) { errno = EFBIG; goto done; }
            total += (uint64_t)count;
            if (callback(context, buffer, count) != 0) { errno = ECANCELED; goto done; }
        }
        if (count < chunk_size) {
            if (ferror(file)) goto done;
            break;
        }
    }
    if (out_total != NULL) *out_total = total;
    result = 0;
done:
    free(buffer);
    if (file != NULL && fclose(file) != 0 && result == 0) result = -1;
    return result;
}

int cdin_fs_begin_atomic_write(const char *path, uint64_t max_bytes, int overwrite, cdin_atomic_writer **out_writer)
{
    cdin_atomic_writer *writer = NULL;
    size_t path_length;
    unsigned int attempt;
    if (path == NULL || path[0] == '\0' || out_writer == NULL || max_bytes == 0u) return -1;
    *out_writer = NULL;
    writer = (cdin_atomic_writer *)calloc(1u, sizeof(*writer));
    if (writer == NULL) return -1;
    path_length = strlen(path);
    if (path_length > SIZE_MAX - 96u) goto fail;
    writer->destination = (char *)malloc(path_length + 1u);
    writer->temporary = (char *)malloc(path_length + 96u);
    if (writer->destination == NULL || writer->temporary == NULL) goto fail;
    memcpy(writer->destination, path, path_length + 1u);
    writer->max_bytes = max_bytes;
    writer->overwrite = overwrite != 0;
    for (attempt = 0u; attempt < 32u; attempt++) {
        unsigned long long sequence = atomic_fetch_add_explicit(&writer_sequence, 1u, memory_order_relaxed) + 1u;
#ifdef _WIN32
        unsigned long process_id = (unsigned long)_getpid();
#else
        unsigned long process_id = (unsigned long)getpid();
#endif
        int n = snprintf(writer->temporary, path_length + 96u, "%s.cdin-tmp-%lu-%llu",
                         path, process_id, sequence);
        if (n < 0 || (size_t)n >= path_length + 96u) goto fail;
        writer->file = cdin_fopen_utf8(writer->temporary, "wbx");
        if (writer->file != NULL) break;
        if (errno != EEXIST) goto fail;
    }
    if (writer->file == NULL) goto fail;
    *out_writer = writer;
    return 0;
fail:
    if (writer != NULL) {
        if (writer->file != NULL) (void)fclose(writer->file);
        if (writer->temporary != NULL) cdin_fs_remove_native(writer->temporary);
        free(writer->temporary); free(writer->destination); free(writer);
    }
    return -1;
}

int cdin_fs_write_chunk(cdin_atomic_writer *writer, const void *bytes, size_t length)
{
    if (writer == NULL || writer->closed || writer->file == NULL || (bytes == NULL && length != 0u)) return -1;
    if ((uint64_t)length > writer->max_bytes - writer->written) { errno = EFBIG; return -1; }
    if (length != 0u && fwrite(bytes, 1u, length, writer->file) != length) return -1;
    writer->written += (uint64_t)length;
    return 0;
}

static void cdin_fs_remove_native(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = cdin_utf8_to_wide(path);
    if (wide != NULL) { (void)DeleteFileW(wide); free(wide); }
#else
    (void)unlink(path);
#endif
}

static int cdin_replace_file(const char *temporary, const char *destination, int overwrite)
{
#ifdef _WIN32
    wchar_t *wide_tmp = cdin_utf8_to_wide(temporary);
    wchar_t *wide_dst = cdin_utf8_to_wide(destination);
    DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0u);
    int ok = 0;
    if (wide_tmp != NULL && wide_dst != NULL) ok = MoveFileExW(wide_tmp, wide_dst, flags) != 0;
    free(wide_tmp); free(wide_dst);
    return ok ? 0 : -1;
#else
    if (overwrite) return rename(temporary, destination);
    /* A hard link is an atomic, no-replace publish on the same filesystem. */
    if (link(temporary, destination) != 0) return -1;
    if (unlink(temporary) != 0) {
        int saved = errno;
        (void)unlink(destination);
        errno = saved;
        return -1;
    }
    return 0;
#endif
}

int cdin_fs_commit_atomic_write(cdin_atomic_writer *writer)
{
    int result = -1;
    if (writer == NULL || writer->closed || writer->file == NULL) return -1;
    if (fflush(writer->file) != 0) goto done;
#ifdef _WIN32
    if (_commit(_fileno(writer->file)) != 0) goto done;
#else
    if (fsync(fileno(writer->file)) != 0) goto done;
#endif
    if (fclose(writer->file) != 0) { writer->file = NULL; goto done; }
    writer->file = NULL;
    if (cdin_replace_file(writer->temporary, writer->destination, writer->overwrite) != 0) goto done;
    result = 0;
done:
    if (writer->file != NULL) { (void)fclose(writer->file); writer->file = NULL; }
    writer->closed = 1;
    if (result != 0) cdin_fs_remove_native(writer->temporary);
    free(writer->temporary); free(writer->destination); free(writer);
    return result;
}

void cdin_fs_abort_atomic_write(cdin_atomic_writer *writer)
{
    if (writer == NULL) return;
    if (writer->file != NULL) (void)fclose(writer->file);
    if (writer->temporary != NULL) cdin_fs_remove_native(writer->temporary);
    free(writer->temporary); free(writer->destination); free(writer);
}

int cdin_fs_stat(const char *path, cdin_fs_entry *out_entry)
{
    if (path == NULL || out_entry == NULL) return -1;
#ifdef _WIN32
    {
        wchar_t *wide = cdin_utf8_to_wide(path);
        WIN32_FILE_ATTRIBUTE_DATA data;
        ULARGE_INTEGER size;
        ULARGE_INTEGER ticks;
        if (wide == NULL) return -1;
        if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) { free(wide); return -1; }
        free(wide);
        size.HighPart = data.nFileSizeHigh; size.LowPart = data.nFileSizeLow;
        ticks.HighPart = data.ftLastWriteTime.dwHighDateTime; ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
        out_entry->name = cdin_path_basename(path);
        out_entry->type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? CDIN_FS_DIRECTORY : CDIN_FS_FILE;
        out_entry->size = size.QuadPart;
        out_entry->modified_seconds = (int64_t)(ticks.QuadPart / UINT64_C(10000000)) - INT64_C(11644473600);
        return 0;
    }
#else
    {
        struct stat value;
        if (stat(path, &value) != 0) return -1;
        out_entry->name = cdin_path_basename(path);
        out_entry->type = S_ISDIR(value.st_mode) ? CDIN_FS_DIRECTORY : (S_ISREG(value.st_mode) ? CDIN_FS_FILE : CDIN_FS_OTHER);
        out_entry->size = value.st_size < 0 ? 0u : (uint64_t)value.st_size;
        out_entry->modified_seconds = (int64_t)value.st_mtime;
        return 0;
    }
#endif
}

int cdin_fs_list_directory(const char *path, cdin_fs_list_fn callback, void *context)
{
    if (path == NULL || callback == NULL) return -1;
#ifdef _WIN32
    {
        size_t n = strlen(path);
        char *pattern = (char *)malloc(n + 4u);
        wchar_t *wide_pattern = NULL;
        WIN32_FIND_DATAW data;
        HANDLE search;
        int result = 0;
        if (pattern == NULL) return -1;
        memcpy(pattern, path, n);
        if (n != 0u && path[n - 1u] != '/' && path[n - 1u] != '\\') pattern[n++] = '/';
        memcpy(pattern + n, "*", 2u);
        wide_pattern = cdin_utf8_to_wide(pattern);
        free(pattern);
        if (wide_pattern == NULL) return -1;
        search = FindFirstFileW(wide_pattern, &data);
        free(wide_pattern);
        if (search == INVALID_HANDLE_VALUE) return -1;
        do {
            char *name;
            cdin_fs_entry entry;
            ULARGE_INTEGER size, ticks;
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            name = cdin_wide_to_utf8(data.cFileName);
            if (name == NULL) { result = -1; break; }
            size.HighPart = data.nFileSizeHigh; size.LowPart = data.nFileSizeLow;
            ticks.HighPart = data.ftLastWriteTime.dwHighDateTime; ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
            entry.name = name;
            entry.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? CDIN_FS_DIRECTORY : CDIN_FS_FILE;
            entry.size = size.QuadPart;
            entry.modified_seconds = (int64_t)(ticks.QuadPart / UINT64_C(10000000)) - INT64_C(11644473600);
            if (callback(context, &entry) != 0) result = -1;
            free(name);
            if (result != 0) break;
        } while (FindNextFileW(search, &data));
        FindClose(search);
        return result;
    }
#else
    {
        DIR *directory = opendir(path);
        struct dirent *item;
        int result = 0;
        if (directory == NULL) return -1;
        while ((item = readdir(directory)) != NULL) {
            char *full;
            cdin_fs_entry entry;
            struct stat st;
            size_t a = strlen(path), b = strlen(item->d_name);
            if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
            if (a > SIZE_MAX - b - 2u) { result = -1; break; }
            full = (char *)malloc(a + b + 2u);
            if (full == NULL) { result = -1; break; }
            memcpy(full, path, a);
            if (a != 0u && path[a - 1u] != '/') full[a++] = '/';
            memcpy(full + a, item->d_name, b + 1u);
            if (stat(full, &st) != 0) { free(full); result = -1; break; }
            free(full);
            entry.name = item->d_name;
            entry.type = S_ISDIR(st.st_mode) ? CDIN_FS_DIRECTORY : (S_ISREG(st.st_mode) ? CDIN_FS_FILE : CDIN_FS_OTHER);
            entry.size = st.st_size < 0 ? 0u : (uint64_t)st.st_size;
            entry.modified_seconds = (int64_t)st.st_mtime;
            if (callback(context, &entry) != 0) { result = -1; break; }
        }
        if (closedir(directory) != 0 && result == 0) result = -1;
        return result;
    }
#endif
}
