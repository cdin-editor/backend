/*
 * Document lifecycle, reads, edits and clean-state operations.
 * KERNEL-001 and KERNEL-002 keep byte ownership and edit semantics in Kernel;
 * this module only translates stable host handles and copies result metadata.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/kernel/document-model.md
 * https://github.com/cdin-editor/spec/blob/main/docs/kernel/editing-operations.md
 */
#include "adapter_internal.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

cdin_backend_status cdin_kernel_host_document_create(cdin_kernel_host *host,
                                                      const uint8_t *bytes, size_t length,
                                                      cdin_host_handle *out_handle,
                                                      cdin_host_doc_info *out_info)
{
    size_t i;
    cdin_doc raw = CDIN_INVALID_DOC;
    cdin_status status;
    if (host == NULL || out_handle == NULL || out_info == NULL || (bytes == NULL && length != 0u)) return CDIN_BACKEND_INVALID_ARGUMENT;
    if ((uint64_t)length > host->max_document_bytes) return CDIN_BACKEND_LIMIT_EXCEEDED;
    for (i = 0u; i < host->document_capacity; i++) if (!host->documents[i].active) break;
    if (i == host->document_capacity) return CDIN_BACKEND_LIMIT_EXCEEDED;
    status = cdin_doc_create(host->kernel, bytes, length, &raw);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    if (host->documents[i].generation == 0u) host->documents[i].generation = 1u;
    host->documents[i].raw = raw;
    host->documents[i].active = 1;
    out_handle->slot = (uint32_t)(i + 1u);
    out_handle->generation = host->documents[i].generation;
    out_info->version = 0u;
    out_info->length = (uint64_t)length;
    out_info->accepted_sequence = 0u;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_document_close(cdin_kernel_host *host, cdin_host_handle handle)
{
    cdin_doc_slot *slot;
    cdin_status status;
    size_t i;
    cdin_backend_status result = cdin_kernel_document_get(host, handle, &slot);
    if (result != CDIN_BACKEND_OK) return result;
    for (i = 0u; i < host->resource_capacity; i++) {
        if (host->transactions[i].active && host->transactions[i].document_slot == handle.slot &&
            host->transactions[i].document_generation == handle.generation) {
            (void)cdin_txn_abort(host->kernel, (cdin_txn)host->transactions[i].raw);
            cdin_kernel_resource_remove(&host->transactions[i]);
        }
        if (host->snapshots[i].active && host->snapshots[i].document_slot == handle.slot &&
            host->snapshots[i].document_generation == handle.generation) {
            (void)cdin_snapshot_release(host->kernel, (cdin_snapshot)host->snapshots[i].raw);
            cdin_kernel_resource_remove(&host->snapshots[i]);
        }
        if (host->subscriptions[i].active && host->subscriptions[i].document_slot == handle.slot &&
            host->subscriptions[i].document_generation == handle.generation) {
            (void)cdin_notif_unsubscribe(host->kernel, (cdin_notif)host->subscriptions[i].raw);
            cdin_kernel_resource_remove(&host->subscriptions[i]);
        }
    }
    status = cdin_doc_close(host->kernel, slot->raw);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    slot->active = 0;
    slot->raw = CDIN_INVALID_DOC;
    slot->generation++;
    if (slot->generation == 0u) slot->generation = 1u;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_document_info(cdin_kernel_host *host, cdin_host_handle handle,
                                                    cdin_host_doc_info *out_info)
{
    cdin_doc_slot *slot;
    cdin_backend_status result;
    if (out_info == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    result = cdin_kernel_document_get(host, handle, &slot);
    if (result != CDIN_BACKEND_OK) return result;
    result = cdin_kernel_document_source_info(host, slot, &out_info->version, &out_info->length);
    out_info->accepted_sequence = 0u;
    return result;
}

cdin_backend_status cdin_kernel_read_source(cdin_kernel_host *host, cdin_source source,
                                       uint64_t start, uint64_t length, int has_expected,
                                       uint64_t expected, cdin_host_read_result *out_result)
{
    cdin_status status;
    uint64_t version = 0u, total = 0u, end = 0u;
    uint32_t flags = 0u;
    size_t wanted;
    uint8_t *buffer = NULL;
    if (out_result == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    status = cdin_text_version(host->kernel, source, &version);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    status = cdin_text_len(host->kernel, source, &total);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    if (has_expected && version != expected) {
        out_result->details.expected = expected;
        out_result->details.current = version;
        return CDIN_BACKEND_STALE_VERSION;
    }
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
        status = cdin_text_read(host->kernel, source, start, end, buffer, wanted, &wanted, &flags);
        if (status != CDIN_OK) { free(buffer); return cdin_kernel_map_status(status); }
    }
    out_result->bytes = buffer;
    out_result->length = wanted;
    out_result->version = version;
    out_result->total_length = total;
    out_result->flags = flags;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_document_read(cdin_kernel_host *host, cdin_host_handle handle,
                                                    uint64_t start, uint64_t length,
                                                    int has_expected, uint64_t expected,
                                                    cdin_host_read_result *out_result)
{
    cdin_doc_slot *slot;
    cdin_backend_status result;
    if (out_result == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    result = cdin_kernel_document_get(host, handle, &slot);
    if (result != CDIN_BACKEND_OK) return result;
    return cdin_kernel_read_source(host, cdin_src_doc(slot->raw), start, length, has_expected, expected, out_result);
}

void cdin_kernel_host_read_result_free(cdin_host_read_result *result)
{
    if (result == NULL) return;
    free(result->bytes);
    memset(result, 0, sizeof(*result));
}

void cdin_kernel_host_lines_result_free(cdin_host_lines_result *result)
{
    size_t index;
    if (result == NULL) return;
    for (index = 0u; index < result->count; index++) free(result->lines[index].bytes);
    free(result->lines);
    memset(result, 0, sizeof(*result));
}

/* Read a version-pinned line window. A temporary Kernel snapshot prevents a
 * concurrent writer from mixing offsets from one version with bytes from the
 * next; the lease is always released before returning to the Runtime. */
cdin_backend_status cdin_kernel_host_document_read_lines(cdin_kernel_host *host,
                                                          cdin_host_handle handle,
                                                          uint64_t first_line,
                                                          size_t line_count,
                                                          uint64_t expected_version,
                                                          size_t max_bytes,
                                                          cdin_host_lines_result *out_result)
{
    cdin_doc_slot *slot;
    cdin_snapshot snapshot = CDIN_INVALID_SNAPSHOT;
    cdin_source source;
    cdin_version version = 0u;
    uint64_t total_lines = 0u;
    uint64_t total_bytes = 0u;
    size_t wanted_count;
    size_t index;
    cdin_status kernel_status;
    cdin_backend_status status;
    int acquired = 0;

    if (host == NULL || out_result == NULL || line_count == 0u || line_count > 4096u || max_bytes == 0u)
        return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    status = cdin_kernel_document_get(host, handle, &slot);
    if (status != CDIN_BACKEND_OK) return status;

    kernel_status = cdin_snapshot_acquire(host->kernel, slot->raw, &version, &snapshot);
    if (kernel_status != CDIN_OK) return cdin_kernel_map_status(kernel_status);
    acquired = 1;
    source = cdin_src_snap(snapshot);
    out_result->version = (uint64_t)version;
    if (expected_version != UINT64_MAX && (uint64_t)version != expected_version) {
        out_result->details.expected = expected_version;
        out_result->details.current = (uint64_t)version;
        status = CDIN_BACKEND_STALE_VERSION;
        goto cleanup;
    }

    kernel_status = cdin_text_line_count(host->kernel, source, &total_lines);
    if (kernel_status != CDIN_OK) { status = cdin_kernel_map_status(kernel_status); goto cleanup; }
    out_result->total_lines = total_lines;
    if (first_line >= total_lines) {
        out_result->details.requested = first_line;
        out_result->details.length = total_lines;
        status = CDIN_BACKEND_OUT_OF_RANGE;
        goto cleanup;
    }
    wanted_count = (size_t)((uint64_t)line_count > total_lines - first_line
        ? total_lines - first_line : (uint64_t)line_count);
    if (wanted_count > SIZE_MAX / sizeof(*out_result->lines)) {
        status = CDIN_BACKEND_LIMIT_EXCEEDED;
        goto cleanup;
    }
    out_result->lines = (cdin_host_line *)calloc(wanted_count, sizeof(*out_result->lines));
    if (out_result->lines == NULL) { status = CDIN_BACKEND_NO_MEMORY; goto cleanup; }

    for (index = 0u; index < wanted_count; index++) {
        uint64_t line = first_line + (uint64_t)index;
        uint64_t start = 0u;
        uint64_t end = 0u;
        size_t bytes_length;
        uint32_t flags = 0u;
        kernel_status = cdin_text_line_start(host->kernel, source, line, &start);
        if (kernel_status != CDIN_OK) { status = cdin_kernel_map_status(kernel_status); goto cleanup; }
        if (line + 1u < total_lines) {
            kernel_status = cdin_text_line_start(host->kernel, source, line + 1u, &end);
            if (kernel_status != CDIN_OK) { status = cdin_kernel_map_status(kernel_status); goto cleanup; }
        } else {
            kernel_status = cdin_text_len(host->kernel, source, &end);
            if (kernel_status != CDIN_OK) { status = cdin_kernel_map_status(kernel_status); goto cleanup; }
        }
        if (end < start || end - start > SIZE_MAX) { status = CDIN_BACKEND_INTERNAL; goto cleanup; }
        bytes_length = (size_t)(end - start);
        if (bytes_length > max_bytes - (size_t)total_bytes) {
            if (index == 0u) {
                out_result->details.required = (uint64_t)bytes_length;
                status = CDIN_BACKEND_LIMIT_EXCEEDED;
                goto cleanup;
            }
            break; /* Return a complete prefix; caller can request the remainder. */
        }
        if (bytes_length != 0u) {
            out_result->lines[index].bytes = (uint8_t *)malloc(bytes_length);
            if (out_result->lines[index].bytes == NULL) { status = CDIN_BACKEND_NO_MEMORY; goto cleanup; }
            out_result->count = index + 1u;
            kernel_status = cdin_text_read(host->kernel, source, start, end,
                out_result->lines[index].bytes, bytes_length, &bytes_length, &flags);
            if (kernel_status != CDIN_OK) { status = cdin_kernel_map_status(kernel_status); goto cleanup; }
        }
        out_result->lines[index].length = bytes_length;
        total_bytes += (uint64_t)bytes_length;
        if (bytes_length == 0u) out_result->count = index + 1u;
    }
    status = CDIN_BACKEND_OK;

cleanup:
    if (acquired) {
        cdin_status release_status = cdin_snapshot_release(host->kernel, snapshot);
        if (status == CDIN_BACKEND_OK && release_status != CDIN_OK)
            status = cdin_kernel_map_status(release_status);
    }
    if (status != CDIN_BACKEND_OK) {
        cdin_host_error_details details = out_result->details;
        uint64_t observed_version = out_result->version;
        uint64_t observed_total_lines = out_result->total_lines;
        cdin_kernel_host_lines_result_free(out_result);
        /* The Lua bridge must retain structured failure data after releasing
         * any partially allocated line buffers. */
        out_result->details = details;
        out_result->version = observed_version;
        out_result->total_lines = observed_total_lines;
    }
    return status;
}

cdin_backend_status cdin_kernel_host_document_replace(cdin_kernel_host *host, cdin_host_handle handle,
                                                       uint64_t expected_version,
                                                       uint64_t start, uint64_t finish,
                                                       const uint8_t *bytes, size_t length,
                                                       uint64_t coalesce_key,
                                                       const uint8_t *blob_before, size_t blob_before_length,
                                                       const uint8_t *blob_after, size_t blob_after_length,
                                                       cdin_host_edit_result *out_result)
{
    cdin_doc_slot *slot;
    cdin_replace_request request;
    cdin_edit_result result;
    cdin_status status;
    cdin_backend_status mapped;
    if (out_result == NULL || (bytes == NULL && length != 0u) || (blob_before == NULL && blob_before_length != 0u) ||
        (blob_after == NULL && blob_after_length != 0u)) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    mapped = cdin_kernel_document_get(host, handle, &slot);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.expected_version = expected_version;
    request.start = start; request.end = finish;
    request.bytes = bytes; request.len = length;
    request.coalesce_key = coalesce_key;
    request.blob_before = blob_before; request.blob_before_len = blob_before_length;
    request.blob_after = blob_after; request.blob_after_len = blob_after_length;
    memset(&result, 0, sizeof(result)); result.struct_size = (uint32_t)sizeof(result);
    status = cdin_doc_replace(host->kernel, slot->raw, &request, &result);
    return cdin_kernel_edit_call_result(status, &result, out_result);
}

cdin_backend_status cdin_kernel_host_document_replace_batch(cdin_kernel_host *host, cdin_host_handle handle,
                                                             uint64_t expected_version,
                                                             const cdin_host_edit *edits, size_t count,
                                                             const uint8_t *blob_before, size_t blob_before_length,
                                                             const uint8_t *blob_after, size_t blob_after_length,
                                                             cdin_host_edit_result *out_result)
{
    cdin_doc_slot *slot;
    cdin_batch_request request;
    cdin_edit_result result;
    uint64_t *starts = NULL, *ends = NULL;
    const uint8_t **bytes = NULL;
    size_t *lengths = NULL;
    size_t i;
    cdin_status status;
    cdin_backend_status mapped;
    if (out_result == NULL || edits == NULL || count == 0u || count > CDIN_MAX_BATCH_EDITS_HARD) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    mapped = cdin_kernel_document_get(host, handle, &slot);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    if (count > SIZE_MAX / sizeof(uint64_t) || count > SIZE_MAX / sizeof(const uint8_t *) || count > SIZE_MAX / sizeof(size_t)) return CDIN_BACKEND_LIMIT_EXCEEDED;
    starts = (uint64_t *)malloc(count * sizeof(uint64_t));
    ends = (uint64_t *)malloc(count * sizeof(uint64_t));
    bytes = (const uint8_t **)malloc(count * sizeof(const uint8_t *));
    lengths = (size_t *)malloc(count * sizeof(size_t));
    if (starts == NULL || ends == NULL || bytes == NULL || lengths == NULL) { mapped = CDIN_BACKEND_NO_MEMORY; goto done; }
    for (i = 0u; i < count; i++) {
        if (edits[i].finish < edits[i].start || (edits[i].bytes == NULL && edits[i].length != 0u)) { mapped = CDIN_BACKEND_INVALID_ARGUMENT; goto done; }
        if (i != 0u && edits[i].start < edits[i - 1u].finish) { mapped = CDIN_BACKEND_INVALID_ARGUMENT; goto done; }
        starts[i] = edits[i].start; ends[i] = edits[i].finish; bytes[i] = edits[i].bytes; lengths[i] = edits[i].length;
    }
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.expected_version = expected_version;
    request.count = count; request.starts = starts; request.ends = ends; request.bytes = bytes; request.lens = lengths;
    request.blob_before = blob_before; request.blob_before_len = blob_before_length;
    request.blob_after = blob_after; request.blob_after_len = blob_after_length;
    memset(&result, 0, sizeof(result)); result.struct_size = (uint32_t)sizeof(result);
    status = cdin_doc_replace_batch(host->kernel, slot->raw, &request, &result);
    mapped = cdin_kernel_edit_call_result(status, &result, out_result);
done:
    free(starts); free(ends); free(bytes); free(lengths);
    return mapped;
}

cdin_backend_status cdin_kernel_host_document_history(cdin_kernel_host *host, cdin_host_handle handle,
                                                       uint64_t expected_version, size_t blob_capacity,
                                                       int redo, cdin_host_edit_result *out_result)
{
    cdin_doc_slot *slot;
    cdin_history_request request;
    cdin_edit_result result;
    uint8_t *blob = NULL;
    cdin_status status;
    cdin_backend_status mapped;
    if (host == NULL || out_result == NULL || blob_capacity > host->max_blob_bytes) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    mapped = cdin_kernel_document_get(host, handle, &slot);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    if (blob_capacity != 0u) {
        blob = (uint8_t *)malloc(blob_capacity);
        if (blob == NULL) return CDIN_BACKEND_NO_MEMORY;
    }
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.expected_version = expected_version;
    request.blob_out = blob;
    request.blob_capacity = blob_capacity;
    memset(&result, 0, sizeof(result)); result.struct_size = (uint32_t)sizeof(result);
    status = redo ? cdin_doc_redo(host->kernel, slot->raw, &request, &result) : cdin_doc_undo(host->kernel, slot->raw, &request, &result);
    mapped = cdin_kernel_edit_call_result(status, &result, out_result);
    if (mapped == CDIN_BACKEND_OK && result.blob_len != 0u) {
        out_result->selection_blob = blob;
        out_result->selection_blob_length = (size_t)result.blob_len;
        blob = NULL;
    }
    free(blob);
    return mapped;
}

void cdin_kernel_host_edit_result_free(cdin_host_edit_result *result)
{
    if (result == NULL) return;
    free(result->selection_blob);
    memset(result, 0, sizeof(*result));
}

cdin_backend_status cdin_kernel_host_document_mark_clean(cdin_kernel_host *host, cdin_host_handle handle)
{
    cdin_doc_slot *slot;
    cdin_backend_status result = cdin_kernel_document_get(host, handle, &slot);
    if (result != CDIN_BACKEND_OK) return result;
    return cdin_kernel_map_status(cdin_doc_mark_clean(host->kernel, slot->raw));
}

cdin_backend_status cdin_kernel_host_document_mark_clean_if_version(cdin_kernel_host *host,
                                                                     cdin_host_handle handle,
                                                                     uint64_t expected, int *out_clean)
{
    cdin_doc_slot *slot;
    uint64_t version;
    cdin_backend_status result;
    cdin_status status;
    if (out_clean == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_clean = 0;
    result = cdin_kernel_document_get(host, handle, &slot);
    if (result != CDIN_BACKEND_OK) return result;
    status = cdin_text_version(host->kernel, cdin_src_doc(slot->raw), &version);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    if (version != expected) return CDIN_BACKEND_OK;
    status = cdin_doc_mark_clean(host->kernel, slot->raw);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    *out_clean = 1;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_host_document_is_clean(cdin_kernel_host *host, cdin_host_handle handle,
                                                        int *out_clean)
{
    cdin_doc_slot *slot;
    cdin_backend_status result;
    if (out_clean == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    result = cdin_kernel_document_get(host, handle, &slot);
    if (result != CDIN_BACKEND_OK) return result;
    return cdin_kernel_map_status(cdin_doc_is_clean(host->kernel, slot->raw, out_clean));
}

