#include "cdin/backend/filesystem.h"
#include "cdin/backend/path.h"
#include "scenario.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct read_capture {
    uint8_t bytes[128];
    size_t length;
    size_t chunks;
} read_capture;

static int capture_chunk(void *context, const uint8_t *bytes, size_t length)
{
    read_capture *capture = (read_capture *)context;
    if (length > sizeof(capture->bytes) - capture->length) return -1;
    memcpy(capture->bytes + capture->length, bytes, length);
    capture->length += length;
    capture->chunks++;
    return 0;
}

static int capture_entry(void *context, const cdin_fs_entry *entry)
{
    size_t *matches = (size_t *)context;
    if (entry->type == CDIN_FS_FILE && entry->name != NULL &&
        strcmp(entry->name, "workspace.cdin") == 0 && entry->size == 27u) {
        (*matches)++;
    }
    return 0;
}

int main(void)
{
    char directory[512];
    char file[768];
    char *normalized = NULL;
    char *joined = NULL;
    char *relative = NULL;
    const char source[] = "project: hello\nrevision: 1\n";
    read_capture capture;
    cdin_atomic_writer *writer = NULL;
    cdin_fs_entry info;
    uint64_t total = 0u;
    size_t found = 0u;

    SCENARIO_CHECK(scenario_temp_directory(directory, sizeof(directory)) == 0);
    SCENARIO_CHECK(cdin_path_normalize("./workspace/notes/../draft.cdin", &normalized) == 0);
    SCENARIO_CHECK(strcmp(normalized, "workspace/draft.cdin") == 0);
    free(normalized);
    SCENARIO_CHECK(cdin_path_join(directory, "workspace.cdin", &joined) == 0);
    SCENARIO_CHECK(scenario_path_join(directory, "workspace.cdin", file, sizeof(file)) == 0);
    SCENARIO_CHECK(strcmp(joined, file) == 0);
    free(joined);

    /* Save a realistic workspace file in chunks and publish it atomically. */
    SCENARIO_CHECK(cdin_fs_begin_atomic_write(file, 128u, 0, &writer) == 0);
    SCENARIO_CHECK(cdin_fs_write_chunk(writer, source, 9u) == 0);
    SCENARIO_CHECK(cdin_fs_write_chunk(writer, source + 9u, sizeof(source) - 1u - 9u) == 0);
    SCENARIO_CHECK(cdin_fs_commit_atomic_write(writer) == 0);
    writer = NULL;

    /* A second writer must not replace a file when overwrite was not requested. */
    SCENARIO_CHECK(cdin_fs_begin_atomic_write(file, 128u, 0, &writer) == 0);
    SCENARIO_CHECK(cdin_fs_write_chunk(writer, "corrupt", 7u) == 0);
    SCENARIO_CHECK(cdin_fs_commit_atomic_write(writer) != 0);
    writer = NULL;

    /* A size-limit failure and explicit abort must preserve the published file. */
    SCENARIO_CHECK(cdin_fs_begin_atomic_write(file, 4u, 1, &writer) == 0);
    SCENARIO_CHECK(cdin_fs_write_chunk(writer, "larger than four bytes", 22u) != 0);
    cdin_fs_abort_atomic_write(writer);
    writer = NULL;

    memset(&capture, 0, sizeof(capture));
    SCENARIO_CHECK(cdin_fs_read_chunks(file, 5u, 128u, capture_chunk, &capture, &total) == 0);
    SCENARIO_CHECK(total == sizeof(source) - 1u);
    SCENARIO_CHECK(capture.length == sizeof(source) - 1u);
    SCENARIO_CHECK(capture.chunks > 1u);
    SCENARIO_CHECK(memcmp(capture.bytes, source, capture.length) == 0);
    SCENARIO_CHECK(cdin_fs_stat(file, &info) == 0);
    SCENARIO_CHECK(info.type == CDIN_FS_FILE && info.size == total);
    SCENARIO_CHECK(cdin_fs_list_directory(directory, capture_entry, &found) == 0);
    SCENARIO_CHECK(found == 1u);

    SCENARIO_CHECK(cdin_path_relative(directory, file, &relative) == 0);
    SCENARIO_CHECK(strcmp(relative, "workspace.cdin") == 0);
    free(relative);
    SCENARIO_CHECK(scenario_remove_file(file) == 0);
    SCENARIO_CHECK(scenario_remove_directory(directory) == 0);
    (void)puts("PASS platform_file_workflow: atomic save, bounded streaming, path and filesystem edge cases");
    return 0;
}
