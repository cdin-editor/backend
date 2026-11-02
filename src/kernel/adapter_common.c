/*
 * Shared handle and result helpers for the Kernel adapter modules.
 * ABI-014 keeps all status translation and generation checks at this boundary.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/abi-and-compatibility.md
 */
#include "adapter_internal.h"
#include <string.h>
#include <stdlib.h>

cdin_backend_status cdin_kernel_map_status(cdin_status status)
{
    switch (status) {
        case CDIN_OK: return CDIN_BACKEND_OK;
        case CDIN_INVALID_ARGUMENT: return CDIN_BACKEND_INVALID_ARGUMENT;
        case CDIN_INVALID_HANDLE: return CDIN_BACKEND_NOT_FOUND;
        case CDIN_STALE_VERSION: return CDIN_BACKEND_STALE_VERSION;
        case CDIN_OUT_OF_RANGE: return CDIN_BACKEND_OUT_OF_RANGE;
        case CDIN_INVALID_POSITION: return CDIN_BACKEND_INVALID_POSITION;
        case CDIN_INVALID_STATE: return CDIN_BACKEND_INVALID_STATE;
        case CDIN_BUSY: return CDIN_BACKEND_BUSY;
        case CDIN_LIMIT_EXCEEDED: return CDIN_BACKEND_LIMIT_EXCEEDED;
        case CDIN_NO_MEMORY: return CDIN_BACKEND_NO_MEMORY;
        case CDIN_PATTERN_INVALID: return CDIN_BACKEND_PATTERN_INVALID;
        case CDIN_CANCELLED: return CDIN_BACKEND_CANCELLED;
        case CDIN_SHUTDOWN: return CDIN_BACKEND_SHUTDOWN;
        case CDIN_UNSUPPORTED: return CDIN_BACKEND_UNSUPPORTED;
        case CDIN_BUFFER_TOO_SMALL: return CDIN_BACKEND_BUFFER_TOO_SMALL;
        case CDIN_VERSION_MISMATCH: return CDIN_BACKEND_VERSION_MISMATCH;
        case CDIN_INTERNAL: default: return CDIN_BACKEND_INTERNAL;
    }
}

cdin_backend_status cdin_kernel_document_get(cdin_kernel_host *host, cdin_host_handle handle,
                                         cdin_doc_slot **out_slot)
{
    cdin_doc_slot *slot;
    if (host == NULL || out_slot == NULL || handle.slot == 0u || handle.slot > host->document_capacity) return CDIN_BACKEND_INVALID_ARGUMENT;
    slot = &host->documents[handle.slot - 1u];
    if (!slot->active || slot->generation != handle.generation) return CDIN_BACKEND_NOT_FOUND;
    *out_slot = slot;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_resource_get(cdin_kernel_host *host, cdin_resource_slot *slots,
                                         cdin_host_handle handle, cdin_resource_slot **out_slot)
{
    cdin_resource_slot *slot;
    if (host == NULL || slots == NULL || out_slot == NULL || handle.slot == 0u || handle.slot > host->resource_capacity) return CDIN_BACKEND_INVALID_ARGUMENT;
    slot = &slots[handle.slot - 1u];
    if (!slot->active || slot->generation != handle.generation) return CDIN_BACKEND_NOT_FOUND;
    if (slot->document_slot == 0u || slot->document_slot > host->document_capacity) return CDIN_BACKEND_NOT_FOUND;
    if (!host->documents[slot->document_slot - 1u].active ||
        host->documents[slot->document_slot - 1u].generation != slot->document_generation) return CDIN_BACKEND_NOT_FOUND;
    *out_slot = slot;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_kernel_resource_new(cdin_kernel_host *host, cdin_resource_slot *slots,
                                         uint64_t raw, cdin_host_handle document,
                                         cdin_host_handle *out_handle)
{
    size_t i;
    if (out_handle == NULL || document.slot == 0u || document.slot > host->document_capacity) return CDIN_BACKEND_INVALID_ARGUMENT;
    for (i = 0u; i < host->resource_capacity; i++) {
        cdin_resource_slot *slot = &slots[i];
        if (!slot->active) {
            if (slot->generation == 0u) slot->generation = 1u;
            slot->raw = raw;
            slot->document_slot = document.slot;
            slot->document_generation = document.generation;
            slot->active = 1;
            out_handle->slot = (uint32_t)(i + 1u);
            out_handle->generation = slot->generation;
            return CDIN_BACKEND_OK;
        }
    }
    return CDIN_BACKEND_LIMIT_EXCEEDED;
}

void cdin_kernel_resource_remove(cdin_resource_slot *slot)
{
    uint32_t generation;
    if (slot == NULL) return;
    generation = slot->generation + 1u;
    if (generation == 0u) generation = 1u;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
}

cdin_backend_status cdin_kernel_document_source_info(cdin_kernel_host *host, cdin_doc_slot *slot,
                                                  uint64_t *out_version, uint64_t *out_length)
{
    cdin_status status;
    cdin_source source = cdin_src_doc(slot->raw);
    status = cdin_text_version(host->kernel, source, out_version);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    status = cdin_text_len(host->kernel, source, out_length);
    return cdin_kernel_map_status(status);
}

void cdin_kernel_error_details_copy(const cdin_error_details *source, cdin_host_error_details *target)
{
    if (source == NULL || target == NULL) return;
    target->limit_id = source->limit_id;
    target->expected = source->expected;
    target->current = source->current;
    target->requested = source->requested;
    target->length = source->length;
    target->required = source->required;
}

static void edit_result_copy(const cdin_edit_result *source, cdin_host_edit_result *target)
{
    target->version = source->new_version;
    target->accepted_sequence = source->accepted_seq;
    target->start = source->start;
    target->removed = source->removed;
    target->inserted = source->inserted;
    target->new_length = source->new_length;
    target->edit_count = source->edit_count;
}

cdin_backend_status cdin_kernel_edit_call_result(cdin_status status, const cdin_edit_result *result,
                                             cdin_host_edit_result *out_result)
{
    if (out_result == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    cdin_kernel_error_details_copy(&result->details, &out_result->details);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    edit_result_copy(result, out_result);
    return CDIN_BACKEND_OK;
}
