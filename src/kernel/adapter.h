#ifndef CDIN_BACKEND_KERNEL_ADAPTER_H
#define CDIN_BACKEND_KERNEL_ADAPTER_H
#include "../core/status.h"
#include <stddef.h>
#include <stdint.h>

typedef struct cdin_kernel_host cdin_kernel_host;

typedef struct cdin_host_handle { uint32_t slot; uint32_t generation; } cdin_host_handle;
typedef struct cdin_host_doc_info { uint64_t version; uint64_t length; uint64_t accepted_sequence; } cdin_host_doc_info;
typedef struct cdin_host_error_details {
    uint32_t limit_id;
    uint64_t expected, current, requested, length, required;
} cdin_host_error_details;
typedef struct cdin_host_read_result { uint8_t *bytes; size_t length; uint64_t version; uint64_t total_length; uint32_t flags; cdin_host_error_details details; } cdin_host_read_result;
typedef struct cdin_host_edit_result {
    uint64_t version, accepted_sequence, start, removed, inserted, new_length;
    uint32_t edit_count;
    cdin_host_error_details details;
    uint8_t *selection_blob;
    size_t selection_blob_length;
} cdin_host_edit_result;
typedef struct cdin_host_edit {
    uint64_t start, finish;
    const uint8_t *bytes;
    size_t length;
} cdin_host_edit;
typedef struct cdin_host_notification {
    uint32_t kind;
    uint64_t from_version, to_version, current_version;
    uint32_t edit_count, edits_stored;
    int edits_truncated;
    uint64_t evicted;
    int is_clean;
    struct { uint64_t start, removed, inserted; } edits[8];
} cdin_host_notification;
typedef struct cdin_host_search_result {
    uint64_t version, count, next_from, next_bound, steps;
    int truncated, partial, skipped_raw, exhausted;
    cdin_host_error_details details;
    uint64_t *ranges;
} cdin_host_search_result;

cdin_backend_status cdin_kernel_host_create(uint64_t max_documents, uint64_t max_document_bytes,
                                             uint64_t max_undo_bytes, int log_level, int log_output,
                                             uint64_t max_log_message_bytes,
                                             cdin_backend_log_callback log_callback, void *log_context,
                                             cdin_kernel_host **out_host);
void cdin_kernel_host_destroy(cdin_kernel_host *host);
cdin_backend_status cdin_kernel_host_log(cdin_kernel_host *host, int level,
                                          const char *component, const char *message);
cdin_backend_status cdin_kernel_host_document_create(cdin_kernel_host *host,
                                                      const uint8_t *bytes, size_t length,
                                                      cdin_host_handle *out_handle,
                                                      cdin_host_doc_info *out_info);
cdin_backend_status cdin_kernel_host_document_close(cdin_kernel_host *host, cdin_host_handle handle);
cdin_backend_status cdin_kernel_host_document_info(cdin_kernel_host *host, cdin_host_handle handle,
                                                    cdin_host_doc_info *out_info);
cdin_backend_status cdin_kernel_host_document_read(cdin_kernel_host *host, cdin_host_handle handle,
                                                    uint64_t start, uint64_t length,
                                                    int has_expected, uint64_t expected,
                                                    cdin_host_read_result *out_result);
void cdin_kernel_host_read_result_free(cdin_host_read_result *result);
cdin_backend_status cdin_kernel_host_document_replace(cdin_kernel_host *host, cdin_host_handle handle,
                                                       uint64_t expected_version,
                                                       uint64_t start, uint64_t finish,
                                                       const uint8_t *bytes, size_t length,
                                                       uint64_t coalesce_key,
                                                       const uint8_t *blob_before, size_t blob_before_length,
                                                       const uint8_t *blob_after, size_t blob_after_length,
                                                       cdin_host_edit_result *out_result);
cdin_backend_status cdin_kernel_host_document_replace_batch(cdin_kernel_host *host, cdin_host_handle handle,
                                                             uint64_t expected_version,
                                                             const cdin_host_edit *edits, size_t count,
                                                             const uint8_t *blob_before, size_t blob_before_length,
                                                             const uint8_t *blob_after, size_t blob_after_length,
                                                             cdin_host_edit_result *out_result);
cdin_backend_status cdin_kernel_host_document_history(cdin_kernel_host *host, cdin_host_handle handle,
                                                       uint64_t expected_version, size_t blob_capacity,
                                                       int redo, cdin_host_edit_result *out_result);
void cdin_kernel_host_edit_result_free(cdin_host_edit_result *result);
cdin_backend_status cdin_kernel_host_document_mark_clean(cdin_kernel_host *host, cdin_host_handle handle);
cdin_backend_status cdin_kernel_host_document_mark_clean_if_version(cdin_kernel_host *host,
                                                                     cdin_host_handle handle,
                                                                     uint64_t expected, int *out_clean);
cdin_backend_status cdin_kernel_host_document_is_clean(cdin_kernel_host *host, cdin_host_handle handle,
                                                        int *out_clean);
cdin_backend_status cdin_kernel_host_snapshot_acquire(cdin_kernel_host *host, cdin_host_handle document,
                                                       cdin_host_handle *out_snapshot,
                                                       cdin_host_doc_info *out_info);
cdin_backend_status cdin_kernel_host_snapshot_read(cdin_kernel_host *host, cdin_host_handle snapshot,
                                                    uint64_t start, uint64_t length,
                                                    cdin_host_read_result *out_result);
cdin_backend_status cdin_kernel_host_snapshot_release(cdin_kernel_host *host, cdin_host_handle snapshot);
cdin_backend_status cdin_kernel_host_transaction_begin(cdin_kernel_host *host, cdin_host_handle document,
                                                         uint64_t expected_version,
                                                         cdin_host_handle *out_transaction,
                                                         uint64_t *out_base_version);
cdin_backend_status cdin_kernel_host_transaction_replace(cdin_kernel_host *host, cdin_host_handle transaction,
                                                           uint64_t start, uint64_t finish,
                                                           const uint8_t *bytes, size_t length,
                                                           cdin_host_error_details *out_details);
cdin_backend_status cdin_kernel_host_transaction_read(cdin_kernel_host *host, cdin_host_handle transaction,
                                                        uint64_t start, uint64_t length,
                                                        cdin_host_read_result *out_result);
cdin_backend_status cdin_kernel_host_transaction_commit(cdin_kernel_host *host,
                                                         cdin_host_handle transaction,
                                                         const uint8_t *blob_before, size_t before_length,
                                                         const uint8_t *blob_after, size_t after_length,
                                                         cdin_host_edit_result *out_result);
cdin_backend_status cdin_kernel_host_transaction_abort(cdin_kernel_host *host,
                                                        cdin_host_handle transaction);
cdin_backend_status cdin_kernel_host_subscribe(cdin_kernel_host *host, cdin_host_handle document,
                                               cdin_host_handle *out_subscription,
                                               uint64_t *out_version);
cdin_backend_status cdin_kernel_host_unsubscribe(cdin_kernel_host *host,
                                                  cdin_host_handle subscription);
cdin_backend_status cdin_kernel_host_drain(cdin_kernel_host *host, cdin_host_handle subscription,
                                            size_t max_events, cdin_host_notification *out_events,
                                            size_t *out_count);
cdin_backend_status cdin_kernel_host_search(cdin_kernel_host *host, cdin_host_handle document,
                                             const uint8_t *pattern, size_t pattern_length,
                                             uint32_t mode, uint32_t direction,
                                             uint64_t from, uint64_t bound, uint64_t limit,
                                             int has_expected, uint64_t expected,
                                             uint32_t search_flags, uint64_t step_limit,
                                             cdin_host_search_result *out_result);
void cdin_kernel_host_search_result_free(cdin_host_search_result *result);
void cdin_kernel_host_features(cdin_kernel_host *host, uint64_t *out_mask);
void cdin_kernel_host_abi_version(cdin_kernel_host *host, uint32_t *major, uint32_t *minor, uint32_t *patch);

#endif
