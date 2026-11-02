#ifndef CDIN_BACKEND_FILESYSTEM_H
#define CDIN_BACKEND_FILESYSTEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdin_atomic_writer cdin_atomic_writer;

typedef enum cdin_fs_entry_type {
    CDIN_FS_FILE = 0,
    CDIN_FS_DIRECTORY,
    CDIN_FS_OTHER
} cdin_fs_entry_type;

typedef struct cdin_fs_entry {
    const char *name;
    cdin_fs_entry_type type;
    uint64_t size;
    int64_t modified_seconds;
} cdin_fs_entry;

typedef int (*cdin_fs_chunk_fn)(void *context, const uint8_t *bytes, size_t length);
typedef int (*cdin_fs_list_fn)(void *context, const cdin_fs_entry *entry);

/* Reads a file in bounded chunks. The callback must not retain the byte view. */
int cdin_fs_read_chunks(const char *path, size_t chunk_size, uint64_t max_bytes,
                        cdin_fs_chunk_fn callback, void *context, uint64_t *out_total);

/*
 * Writes to a sibling temporary file and publishes it only on commit.
 * A failed/aborted write leaves an existing destination unchanged.
 */
int cdin_fs_begin_atomic_write(const char *path, uint64_t max_bytes, int overwrite,
                               cdin_atomic_writer **out_writer);
int cdin_fs_write_chunk(cdin_atomic_writer *writer, const void *bytes, size_t length);
int cdin_fs_commit_atomic_write(cdin_atomic_writer *writer);
void cdin_fs_abort_atomic_write(cdin_atomic_writer *writer);

/* `out_entry->name` is NULL for stat; directory callback names are borrowed. */
int cdin_fs_stat(const char *path, cdin_fs_entry *out_entry);
int cdin_fs_list_directory(const char *path, cdin_fs_list_fn callback, void *context);

#ifdef __cplusplus
}
#endif

#endif
