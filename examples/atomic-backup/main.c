#include "cdin/backend/services.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKUP_LIMIT_BYTES (32u * 1024u * 1024u)
#define READ_CHUNK_BYTES (64u * 1024u)

typedef struct backup_context {
    cdin_atomic_writer *writer;
} backup_context;

static int copy_chunk(void *opaque, const uint8_t *bytes, size_t length)
{
    backup_context *context = (backup_context *)opaque;
    return cdin_fs_write_chunk(context->writer, bytes, length);
}

static void print_usage(const char *program)
{
    (void)fprintf(stderr, "Usage: %s INPUT_FILE BACKUP_FILE\n", program);
}

int main(int argc, char **argv)
{
    char *input_absolute = NULL;
    char *backup_absolute = NULL;
    cdin_fs_entry source_info;
    cdin_atomic_writer *writer = NULL;
    backup_context context;
    uint64_t copied = 0u;
    int result = EXIT_FAILURE;

    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (cdin_path_absolute(argv[1], &input_absolute) != 0 ||
        cdin_path_absolute(argv[2], &backup_absolute) != 0) {
        (void)fprintf(stderr, "Could not resolve input or backup path.\n");
        goto cleanup;
    }
    if (strcmp(input_absolute, backup_absolute) == 0) {
        (void)fprintf(stderr, "Input and backup paths refer to the same file.\n");
        goto cleanup;
    }
    if (cdin_fs_stat(input_absolute, &source_info) != 0 || source_info.type != CDIN_FS_FILE) {
        (void)fprintf(stderr, "Input must be an existing regular file.\n");
        goto cleanup;
    }
    if (source_info.size > BACKUP_LIMIT_BYTES) {
        (void)fprintf(stderr, "Input is larger than the configured backup limit.\n");
        goto cleanup;
    }

    /*
     * A temporary file beside the destination prevents a partial backup from
     * replacing the last known-good copy if reading or writing later fails.
     */
    if (cdin_fs_begin_atomic_write(backup_absolute, BACKUP_LIMIT_BYTES, 1, &writer) != 0) {
        (void)fprintf(stderr, "Could not create a temporary backup file.\n");
        goto cleanup;
    }
    context.writer = writer;
    if (cdin_fs_read_chunks(input_absolute, READ_CHUNK_BYTES, BACKUP_LIMIT_BYTES,
                            copy_chunk, &context, &copied) != 0) {
        (void)fprintf(stderr, "Reading input or writing backup failed.\n");
        goto cleanup;
    }
    if (copied != source_info.size) {
        (void)fprintf(stderr,
                      "Input changed while it was copied (expected=%" PRIu64
                      " actual=%" PRIu64 ").\n",
                      source_info.size, copied);
        goto cleanup;
    }
    if (cdin_fs_commit_atomic_write(writer) != 0) {
        writer = NULL; /* Commit consumes the writer on both success and failure. */
        (void)fprintf(stderr, "Could not publish the completed backup.\n");
        goto cleanup;
    }
    writer = NULL;
    (void)printf("Backup created: %s (%" PRIu64 " bytes)\n", backup_absolute, copied);
    result = EXIT_SUCCESS;

cleanup:
    if (writer != NULL) cdin_fs_abort_atomic_write(writer);
    free(backup_absolute);
    free(input_absolute);
    return result;
}
