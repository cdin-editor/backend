#include "kernel/adapter.h"
#include "cdin/kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        goto fail; \
    } \
} while (0)

static void discard_log(void *context, int level, const char *message, size_t length)
{
    (void)context;
    (void)level;
    (void)message;
    (void)length;
}

static int read_equals(cdin_kernel_host *host, cdin_host_handle document,
                       const char *expected, uint64_t expected_version)
{
    cdin_host_read_result read_result;
    const size_t expected_length = strlen(expected);
    cdin_backend_status status = cdin_kernel_host_document_read(
        host, document, 0u, UINT64_MAX, 1, expected_version, &read_result);

    if (status != CDIN_BACKEND_OK) return 0;
    if (read_result.version != expected_version || read_result.length != expected_length ||
        memcmp(read_result.bytes, expected, expected_length) != 0) {
        cdin_kernel_host_read_result_free(&read_result);
        return 0;
    }
    cdin_kernel_host_read_result_free(&read_result);
    return 1;
}

int main(void)
{
    static const uint8_t original[] = "hello world\nsecond line\n";
    static const uint8_t replacement[] = "CDIN";
    static const uint8_t welcome[] = "welcome";
    static const uint8_t draft[] = "draft";
    cdin_kernel_host *host = NULL;
    cdin_host_handle document = {0u, 0u};
    cdin_host_handle snapshot = {0u, 0u};
    cdin_host_handle transaction = {0u, 0u};
    cdin_host_handle subscription = {0u, 0u};
    cdin_host_doc_info info;
    cdin_host_doc_info snapshot_info;
    cdin_host_doc_info lone_cr_info;
    cdin_host_read_result read_result;
    cdin_host_edit_result edit_result;
    cdin_host_error_details error_details;
    cdin_host_notification notifications[8];
    cdin_host_search_result search_result;
    uint64_t base_version = 0u;
    size_t notification_count = 0u;
    int is_clean = 0;
    uint32_t abi_major = 0u, abi_minor = 0u, abi_patch = 0u;
    cdin_backend_status status;

    CHECK(cdin_kernel_host_create(4u, 1024u * 1024u, 1024u * 1024u,
                                  CDIN_BACKEND_LOG_WARN, CDIN_BACKEND_LOG_OUTPUT_CALLBACK,
                                  16u * 1024u, discard_log, NULL, &host) == CDIN_BACKEND_OK);
    cdin_kernel_host_abi_version(host, &abi_major, &abi_minor, &abi_patch);
    CHECK(abi_major == 1u && abi_minor >= 1u);

    CHECK(cdin_kernel_host_document_create(host, original, sizeof(original) - 1u,
                                           &document, &info) == CDIN_BACKEND_OK);
    CHECK(info.version == 0u && info.length == sizeof(original) - 1u);
    {
        cdin_host_line_position position;
        CHECK(cdin_kernel_host_document_line_from_offset(
            host, document, 12u, 0u, &position) == CDIN_BACKEND_OK);
        CHECK(position.version == 0u && position.line == 1u && position.column == 0u);
        CHECK(cdin_kernel_host_document_line_from_offset(
            host, document, 11u, 0u, &position) == CDIN_BACKEND_OK);
        CHECK(position.line == 0u && position.column == 11u);
        CHECK(cdin_kernel_host_document_line_from_offset(
            host, document, sizeof(original) - 1u, 0u, &position) == CDIN_BACKEND_OK);
        CHECK(position.line == 2u && position.column == 0u);
        CHECK(cdin_kernel_host_document_line_from_offset(
            host, document, sizeof(original), 0u, &position) == CDIN_BACKEND_OUT_OF_RANGE);
        CHECK(position.details.requested == sizeof(original));
        CHECK(position.details.length == sizeof(original) - 1u);
        CHECK(cdin_kernel_host_document_line_from_offset(
            host, document, 0u, 9u, &position) == CDIN_BACKEND_STALE_VERSION);
        CHECK(position.details.expected == 9u && position.details.current == 0u);
    }
    {
        cdin_host_lines_result lines;
        static const uint8_t lone_cr_bytes[] = { 'a', '\r', 'b', '\r', 'c' };
        cdin_host_handle lone_cr_document = {0u, 0u};

        CHECK(cdin_kernel_host_document_read_lines(host, document, 0u, 0, 0u,
            2u, 0u, 12u, &lines) == CDIN_BACKEND_OK);
        CHECK(lines.version == 0u && lines.first_line == 0u && lines.first_byte == 0u);
        CHECK(lines.total_lines == 3u && lines.count == 1u);
        CHECK(lines.lines[0].length == 12u);
        CHECK(memcmp(lines.lines[0].bytes, "hello world\n", 12u) == 0);
        cdin_kernel_host_lines_result_free(&lines);

        CHECK(cdin_kernel_host_document_read_lines(host, document, 0u, 1, 12u,
            2u, 0u, 12u, &lines) == CDIN_BACKEND_OK);
        CHECK(lines.first_line == 1u && lines.first_byte == 12u);
        CHECK(lines.count == 2u && lines.lines[0].length == 12u);
        CHECK(memcmp(lines.lines[0].bytes, "second line\n", 12u) == 0);
        CHECK(lines.lines[1].length == 0u); /* final empty line at EOF */
        cdin_kernel_host_lines_result_free(&lines);
        CHECK(cdin_kernel_host_document_read_lines(host, document, 0u, 1, 24u,
            1u, 0u, 64u, &lines) == CDIN_BACKEND_OK);
        CHECK(lines.first_line == 2u && lines.first_byte == 24u);
        CHECK(lines.total_lines == 3u && lines.count == 1u);
        CHECK(lines.lines[0].length == 0u);
        cdin_kernel_host_lines_result_free(&lines);

        CHECK(cdin_kernel_host_document_read_lines(host, document, 0u, 1, 25u,
            1u, 0u, 64u, &lines) == CDIN_BACKEND_OUT_OF_RANGE);
        CHECK(lines.details.requested == 25u && lines.details.length == sizeof(original) - 1u);
        cdin_kernel_host_lines_result_free(&lines);

        CHECK(cdin_kernel_host_document_read_lines(host, document, 0u, 1, 12u,
            1u, 9u, 64u, &lines) == CDIN_BACKEND_STALE_VERSION);
        CHECK(lines.details.expected == 9u && lines.details.current == 0u);
        cdin_kernel_host_lines_result_free(&lines);

        CHECK(cdin_kernel_host_document_create(host, lone_cr_bytes, sizeof(lone_cr_bytes),
                                               &lone_cr_document, &lone_cr_info) == CDIN_BACKEND_OK);
        CHECK(cdin_kernel_host_document_read_lines(host, lone_cr_document, 0u, 1, 2u,
            1u, 0u, 64u, &lines) == CDIN_BACKEND_OK);
        CHECK(lines.first_line == 1u && lines.first_byte == 2u);
        CHECK(lines.count == 1u && lines.lines[0].length == 2u);
        CHECK(memcmp(lines.lines[0].bytes, "b\r", 2u) == 0);
        cdin_kernel_host_lines_result_free(&lines);
        CHECK(cdin_kernel_host_document_close(host, lone_cr_document) == CDIN_BACKEND_OK);
    }
    CHECK(cdin_kernel_host_document_is_clean(host, document, &is_clean) == CDIN_BACKEND_OK);
    CHECK(is_clean == 1);

    /* A save operation holds a stable snapshot while the live document changes. */
    CHECK(cdin_kernel_host_snapshot_acquire(host, document, &snapshot,
                                            &snapshot_info) == CDIN_BACKEND_OK);
    CHECK(snapshot_info.version == info.version && snapshot_info.length == info.length);

    CHECK(cdin_kernel_host_subscribe(host, document, &subscription,
                                     &base_version) == CDIN_BACKEND_OK);
    CHECK(base_version == 0u);

    /* Replace the selected word, using the version read by the UI. */
    CHECK(cdin_kernel_host_document_replace(host, document, info.version,
                                            6u, 11u, replacement, sizeof(replacement) - 1u,
                                            0u, NULL, 0u, NULL, 0u,
                                            &edit_result) == CDIN_BACKEND_OK);
    CHECK(edit_result.version == 1u && edit_result.new_length == 23u);
    CHECK(read_equals(host, document, "hello CDIN\nsecond line\n", 1u));

    /* A stale UI frame must fail with useful version details, not overwrite. */
    status = cdin_kernel_host_document_replace(host, document, 0u, 0u, 5u,
                                               welcome, sizeof(welcome) - 1u,
                                               0u, NULL, 0u, NULL, 0u, &edit_result);
    CHECK(status == CDIN_BACKEND_STALE_VERSION);
    CHECK(edit_result.details.expected == 0u && edit_result.details.current == 1u);

    CHECK(cdin_kernel_host_snapshot_read(host, snapshot, 0u, UINT64_MAX,
                                         &read_result) == CDIN_BACKEND_OK);
    CHECK(read_result.version == 0u && read_result.length == sizeof(original) - 1u);
    CHECK(memcmp(read_result.bytes, original, read_result.length) == 0);
    cdin_kernel_host_read_result_free(&read_result);

    CHECK(cdin_kernel_host_drain(host, subscription, 8u, notifications,
                                 &notification_count) == CDIN_BACKEND_OK);
    CHECK(notification_count >= 1u);

    /* Transactions expose a private working copy until commit. */
    CHECK(cdin_kernel_host_transaction_begin(host, document, 1u,
                                             &transaction, &base_version) == CDIN_BACKEND_OK);
    CHECK(base_version == 1u);
    CHECK(cdin_kernel_host_transaction_replace(host, transaction, 0u, 5u,
                                               welcome, sizeof(welcome) - 1u,
                                               &error_details) == CDIN_BACKEND_OK);
    CHECK(cdin_kernel_host_transaction_read(host, transaction, 0u, UINT64_MAX,
                                            &read_result) == CDIN_BACKEND_OK);
    CHECK(read_result.length == strlen("welcome CDIN\nsecond line\n"));
    CHECK(memcmp(read_result.bytes, "welcome CDIN\nsecond line\n", read_result.length) == 0);
    cdin_kernel_host_read_result_free(&read_result);
    CHECK(read_equals(host, document, "hello CDIN\nsecond line\n", 1u));
    CHECK(cdin_kernel_host_transaction_commit(host, transaction, NULL, 0u,
                                              NULL, 0u, &edit_result) == CDIN_BACKEND_OK);
    transaction.slot = 0u;
    CHECK(edit_result.version == 2u);
    CHECK(read_equals(host, document, "welcome CDIN\nsecond line\n", 2u));

    /* Undo and redo are version-checked; stale commands do not change content. */
    CHECK(cdin_kernel_host_document_history(host, document, 1u, 128u, 0,
                                            &edit_result) == CDIN_BACKEND_STALE_VERSION);
    CHECK(cdin_kernel_host_document_history(host, document, 2u, 128u, 0,
                                            &edit_result) == CDIN_BACKEND_OK);
    CHECK(read_equals(host, document, "hello CDIN\nsecond line\n", edit_result.version));
    CHECK(cdin_kernel_host_document_history(host, document, edit_result.version, 128u, 1,
                                            &edit_result) == CDIN_BACKEND_OK);
    CHECK(read_equals(host, document, "welcome CDIN\nsecond line\n", edit_result.version));

    /* Literal search returns byte ranges tied to the current document version. */
    CHECK(cdin_kernel_host_document_info(host, document, &info) == CDIN_BACKEND_OK);
    CHECK(cdin_kernel_host_search(host, document, (const uint8_t *)"CDIN", 4u,
                                  CDIN_SEARCH_LITERAL, CDIN_SEARCH_FORWARD,
                                  0u, info.length, 8u, 1, info.version,
                                  0u, 100000u, &search_result) == CDIN_BACKEND_OK);
    CHECK(search_result.version == info.version && search_result.count == 1u);
    CHECK(search_result.ranges[0] == 8u && search_result.ranges[1] == 12u);
    cdin_kernel_host_search_result_free(&search_result);

    /* Regex was not selected by the Kernel ABI; the adapter must say so. */
    CHECK(cdin_kernel_host_search(host, document, (const uint8_t *)".*", 2u,
                                  CDIN_SEARCH_REGEX, CDIN_SEARCH_FORWARD,
                                  0u, info.length, 8u, 0, 0u,
                                  0u, 100000u, &search_result) == CDIN_BACKEND_UNSUPPORTED);

    /* An aborted transaction must not leak draft bytes into the live document. */
    CHECK(cdin_kernel_host_transaction_begin(host, document, info.version,
                                             &transaction, &base_version) == CDIN_BACKEND_OK);
    CHECK(cdin_kernel_host_transaction_replace(host, transaction, 0u, 7u,
                                               draft, sizeof(draft) - 1u,
                                               &error_details) == CDIN_BACKEND_OK);
    CHECK(cdin_kernel_host_transaction_abort(host, transaction) == CDIN_BACKEND_OK);
    transaction.slot = 0u;
    CHECK(read_equals(host, document, "welcome CDIN\nsecond line\n", info.version));

    CHECK(cdin_kernel_host_snapshot_release(host, snapshot) == CDIN_BACKEND_OK);
    snapshot.slot = 0u;
    CHECK(cdin_kernel_host_unsubscribe(host, subscription) == CDIN_BACKEND_OK);
    subscription.slot = 0u;
    CHECK(cdin_kernel_host_document_mark_clean_if_version(host, document,
                                                          info.version - 1u, &is_clean) == CDIN_BACKEND_OK);
    CHECK(is_clean == 0);
    CHECK(cdin_kernel_host_document_mark_clean_if_version(host, document,
                                                          info.version, &is_clean) == CDIN_BACKEND_OK);
    CHECK(is_clean == 1);
    CHECK(cdin_kernel_host_document_close(host, document) == CDIN_BACKEND_OK);
    CHECK(cdin_kernel_host_document_info(host, document, &info) == CDIN_BACKEND_NOT_FOUND);

    cdin_kernel_host_destroy(host);
    (void)puts("PASS kernel_document_workflow: create/read/replace, stale-version rejection, snapshots, notifications, transactions, undo/redo, literal search, abort and stale handles");
    return 0;

fail:
    if (host != NULL) {
        if (transaction.slot != 0u) (void)cdin_kernel_host_transaction_abort(host, transaction);
        if (snapshot.slot != 0u) (void)cdin_kernel_host_snapshot_release(host, snapshot);
        if (subscription.slot != 0u) (void)cdin_kernel_host_unsubscribe(host, subscription);
        if (document.slot != 0u) (void)cdin_kernel_host_document_close(host, document);
        cdin_kernel_host_destroy(host);
    }
    return 1;
}
