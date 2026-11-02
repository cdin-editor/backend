/*
 * Snapshot, transaction and notification handle management.
 * Snapshot immutability, transactional commit/abort and bounded notifications
 * are Kernel contracts; this module manages Backend generations and ownership.
 * https://github.com/cdin-editor/spec/blob/main/docs/kernel/transactions.md
 * https://github.com/cdin-editor/spec/blob/main/docs/kernel/change-notifications.md
 * https://github.com/cdin-editor/spec/blob/main/docs/kernel/undo-redo.md
 */
#include "adapter_internal.h"
#include <stdlib.h>
#include <string.h>

cdin_backend_status cdin_kernel_host_snapshot_acquire(cdin_kernel_host *host, cdin_host_handle document,
                                                       cdin_host_handle *out_snapshot,
                                                       cdin_host_doc_info *out_info)
{
    cdin_doc_slot *doc;
    cdin_version version;
    cdin_snapshot snapshot;
    cdin_status status;
    cdin_backend_status mapped = cdin_kernel_document_get(host, document, &doc);
    if (mapped != CDIN_BACKEND_OK || out_snapshot == NULL || out_info == NULL) return mapped == CDIN_BACKEND_OK ? CDIN_BACKEND_INVALID_ARGUMENT : mapped;
    status = cdin_snapshot_acquire(host->kernel, doc->raw, &version, &snapshot);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    mapped = cdin_kernel_resource_new(host, host->snapshots, (uint64_t)snapshot, document, out_snapshot);
    if (mapped != CDIN_BACKEND_OK) { (void)cdin_snapshot_release(host->kernel, snapshot); return mapped; }
    out_info->version = version;
    out_info->accepted_sequence = 0u;
    status = cdin_text_len(host->kernel, cdin_src_snap(snapshot), &out_info->length);
    if (status != CDIN_OK) { (void)cdin_snapshot_release(host->kernel, snapshot); cdin_kernel_resource_remove(&host->snapshots[out_snapshot->slot - 1u]); return cdin_kernel_map_status(status); }
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_snapshot_read(cdin_kernel_host *host, cdin_host_handle snapshot,
                                                    uint64_t start, uint64_t length,
                                                    cdin_host_read_result *out_result)
{
    cdin_resource_slot *slot;
    cdin_backend_status result;
    if (out_result == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    result = cdin_kernel_resource_get(host, host == NULL ? NULL : host->snapshots, snapshot, &slot);
    if (result != CDIN_BACKEND_OK) return result;
    return cdin_kernel_read_source(host, cdin_src_snap((cdin_snapshot)slot->raw), start, length, 0, 0u, out_result);
}

cdin_backend_status cdin_kernel_host_snapshot_release(cdin_kernel_host *host, cdin_host_handle snapshot)
{
    cdin_resource_slot *slot;
    cdin_backend_status result = cdin_kernel_resource_get(host, host == NULL ? NULL : host->snapshots, snapshot, &slot);
    cdin_status status;
    if (result != CDIN_BACKEND_OK) return result;
    status = cdin_snapshot_release(host->kernel, (cdin_snapshot)slot->raw);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    cdin_kernel_resource_remove(slot);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_transaction_begin(cdin_kernel_host *host, cdin_host_handle document,
                                                         uint64_t expected_version,
                                                         cdin_host_handle *out_transaction,
                                                         uint64_t *out_base_version)
{
    cdin_doc_slot *doc;
    cdin_txn_begin_request request;
    cdin_txn transaction;
    cdin_status status;
    uint64_t current_version = 0u;
    uint64_t effective_expected = expected_version;
    cdin_backend_status mapped = cdin_kernel_document_get(host, document, &doc);
    if (mapped != CDIN_BACKEND_OK || out_transaction == NULL || out_base_version == NULL) return mapped == CDIN_BACKEND_OK ? CDIN_BACKEND_INVALID_ARGUMENT : mapped;
    status = cdin_text_version(host->kernel, cdin_src_doc(doc->raw), &current_version);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    if (effective_expected == CDIN_ANY_VERSION) effective_expected = current_version;
    else if (effective_expected != current_version) {
        *out_base_version = current_version;
        return CDIN_BACKEND_STALE_VERSION;
    }
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.expected_version = effective_expected;
    status = cdin_txn_begin(host->kernel, doc->raw, &request, &transaction);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    mapped = cdin_kernel_resource_new(host, host->transactions, (uint64_t)transaction, document, out_transaction);
    if (mapped != CDIN_BACKEND_OK) { (void)cdin_txn_abort(host->kernel, transaction); return mapped; }
    *out_base_version = effective_expected;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_transaction_replace(cdin_kernel_host *host, cdin_host_handle transaction,
                                                           uint64_t start, uint64_t finish,
                                                           const uint8_t *bytes, size_t length,
                                                           cdin_host_error_details *out_details)
{
    cdin_resource_slot *slot;
    cdin_replace_request request;
    cdin_edit_result result;
    cdin_status status;
    cdin_backend_status mapped;
    if (out_details != NULL) memset(out_details, 0, sizeof(*out_details));
    mapped = cdin_kernel_resource_get(host, host == NULL ? NULL : host->transactions, transaction, &slot);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    if (finish < start || (bytes == NULL && length != 0u)) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.expected_version = CDIN_ANY_VERSION;
    request.start = start; request.end = finish; request.bytes = bytes; request.len = length;
    memset(&result, 0, sizeof(result)); result.struct_size = (uint32_t)sizeof(result);
    status = cdin_txn_replace(host->kernel, (cdin_txn)slot->raw, &request, &result);
    if (out_details != NULL) cdin_kernel_error_details_copy(&result.details, out_details);
    return cdin_kernel_map_status(status);
}

cdin_backend_status cdin_kernel_host_transaction_read(cdin_kernel_host *host, cdin_host_handle transaction,
                                                        uint64_t start, uint64_t length,
                                                        cdin_host_read_result *out_result)
{
    cdin_resource_slot *slot;
    cdin_status status;
    uint64_t total = 0u, end;
    uint8_t *buffer = NULL;
    size_t wanted;
    cdin_backend_status mapped;
    if (out_result == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    mapped = cdin_kernel_resource_get(host, host == NULL ? NULL : host->transactions, transaction, &slot);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    status = cdin_txn_len(host->kernel, (cdin_txn)slot->raw, &total);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    if (start > total) {
        out_result->details.requested = start;
        out_result->details.length = total;
        return CDIN_BACKEND_OUT_OF_RANGE;
    }
    end = length > total - start ? total : start + length;
    if (end - start > SIZE_MAX) return CDIN_BACKEND_LIMIT_EXCEEDED;
    wanted = (size_t)(end - start);
    if (wanted != 0u) {
        buffer = (uint8_t *)malloc(wanted);
        if (buffer == NULL) return CDIN_BACKEND_NO_MEMORY;
        status = cdin_txn_read(host->kernel, (cdin_txn)slot->raw, start, end, buffer, wanted, &wanted);
        if (status != CDIN_OK) { free(buffer); return cdin_kernel_map_status(status); }
    }
    out_result->bytes = buffer; out_result->length = wanted; out_result->total_length = total;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_transaction_commit(cdin_kernel_host *host,
                                                         cdin_host_handle transaction,
                                                         const uint8_t *blob_before, size_t before_length,
                                                         const uint8_t *blob_after, size_t after_length,
                                                         cdin_host_edit_result *out_result)
{
    cdin_resource_slot *slot;
    cdin_txn_commit_request request;
    cdin_edit_result result;
    cdin_status status;
    cdin_backend_status mapped;
    if (out_result == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    mapped = cdin_kernel_resource_get(host, host == NULL ? NULL : host->transactions, transaction, &slot);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.blob_before = blob_before; request.blob_before_len = before_length;
    request.blob_after = blob_after; request.blob_after_len = after_length;
    memset(&result, 0, sizeof(result)); result.struct_size = (uint32_t)sizeof(result);
    status = cdin_txn_commit(host->kernel, (cdin_txn)slot->raw, &request, &result);
    mapped = cdin_kernel_edit_call_result(status, &result, out_result);
    if (mapped == CDIN_BACKEND_OK) cdin_kernel_resource_remove(slot);
    return mapped;
}

cdin_backend_status cdin_kernel_host_transaction_abort(cdin_kernel_host *host,
                                                        cdin_host_handle transaction)
{
    cdin_resource_slot *slot;
    cdin_status status;
    cdin_backend_status mapped = cdin_kernel_resource_get(host, host == NULL ? NULL : host->transactions, transaction, &slot);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    status = cdin_txn_abort(host->kernel, (cdin_txn)slot->raw);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    cdin_kernel_resource_remove(slot);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_subscribe(cdin_kernel_host *host, cdin_host_handle document,
                                               cdin_host_handle *out_subscription,
                                               uint64_t *out_version)
{
    cdin_doc_slot *doc;
    cdin_notif request_handle;
    cdin_version version;
    cdin_subscribe_request request;
    cdin_status status;
    cdin_backend_status mapped = cdin_kernel_document_get(host, document, &doc);
    if (mapped != CDIN_BACKEND_OK || out_subscription == NULL || out_version == NULL) return mapped == CDIN_BACKEND_OK ? CDIN_BACKEND_INVALID_ARGUMENT : mapped;
    memset(&request, 0, sizeof(request)); request.struct_size = (uint32_t)sizeof(request);
    status = cdin_notif_subscribe(host->kernel, doc->raw, &request, &version, &request_handle);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    mapped = cdin_kernel_resource_new(host, host->subscriptions, (uint64_t)request_handle, document, out_subscription);
    if (mapped != CDIN_BACKEND_OK) { (void)cdin_notif_unsubscribe(host->kernel, request_handle); return mapped; }
    *out_version = version;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_unsubscribe(cdin_kernel_host *host,
                                                  cdin_host_handle subscription)
{
    cdin_resource_slot *slot;
    cdin_status status;
    cdin_backend_status mapped = cdin_kernel_resource_get(host, host == NULL ? NULL : host->subscriptions, subscription, &slot);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    status = cdin_notif_unsubscribe(host->kernel, (cdin_notif)slot->raw);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    cdin_kernel_resource_remove(slot);
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_drain(cdin_kernel_host *host, cdin_host_handle subscription,
                                            size_t max_events, cdin_host_notification *out_events,
                                            size_t *out_count)
{
    cdin_resource_slot *slot;
    cdin_notif_event *events = NULL;
    size_t count = 0u, i;
    cdin_status status;
    cdin_backend_status mapped = cdin_kernel_resource_get(host, host == NULL ? NULL : host->subscriptions, subscription, &slot);
    if (mapped != CDIN_BACKEND_OK || out_count == NULL || (max_events != 0u && out_events == NULL)) return mapped == CDIN_BACKEND_OK ? CDIN_BACKEND_INVALID_ARGUMENT : mapped;
    *out_count = 0u;
    if (max_events == 0u) return CDIN_BACKEND_OK;
    if (max_events > SIZE_MAX / sizeof(*events)) return CDIN_BACKEND_LIMIT_EXCEEDED;
    events = (cdin_notif_event *)calloc(max_events, sizeof(*events));
    if (events == NULL) return CDIN_BACKEND_NO_MEMORY;
    for (i = 0u; i < max_events; i++) events[i].struct_size = (uint32_t)sizeof(events[i]);
    status = cdin_notif_drain(host->kernel, (cdin_notif)slot->raw, events, max_events, &count);
    if (status != CDIN_OK) { free(events); return cdin_kernel_map_status(status); }
    for (i = 0u; i < count; i++) {
        size_t edit;
        out_events[i].kind = (uint32_t)events[i].kind;
        out_events[i].from_version = events[i].from_version;
        out_events[i].to_version = events[i].to_version;
        out_events[i].current_version = events[i].current_version;
        out_events[i].edit_count = events[i].edit_count;
        out_events[i].edits_stored = events[i].edits_stored;
        out_events[i].edits_truncated = events[i].edits_truncated;
        out_events[i].evicted = events[i].evicted;
        out_events[i].is_clean = events[i].is_clean;
        for (edit = 0u; edit < events[i].edits_stored && edit < 8u; edit++) {
            out_events[i].edits[edit].start = events[i].edits[edit].start;
            out_events[i].edits[edit].removed = events[i].edits[edit].removed;
            out_events[i].edits[edit].inserted = events[i].edits[edit].inserted;
        }
    }
    free(events);
    *out_count = count;
    return CDIN_BACKEND_OK;
}

