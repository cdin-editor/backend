/*
 * Private Kernel adapter state shared by the small implementation modules.
 * Only files in this directory include the Kernel ABI header. This preserves
 * one auditable ABI boundary while keeping document, resource and search
 * operations independently readable and testable.
 */
#ifndef CDIN_BACKEND_KERNEL_ADAPTER_INTERNAL_H
#define CDIN_BACKEND_KERNEL_ADAPTER_INTERNAL_H

#include "adapter.h"
#include "cdin/kernel.h"

#include <stddef.h>
#include <stdint.h>

typedef struct cdin_doc_slot {
    cdin_doc raw;
    uint32_t generation;
    int active;
} cdin_doc_slot;

typedef struct cdin_resource_slot {
    uint64_t raw;
    uint32_t generation;
    uint32_t document_slot;
    uint32_t document_generation;
    int active;
} cdin_resource_slot;

struct cdin_kernel_host {
    cdin_kernel *kernel;
    cdin_doc_slot *documents;
    cdin_resource_slot *snapshots;
    cdin_resource_slot *transactions;
    cdin_resource_slot *subscriptions;
    size_t document_capacity;
    size_t resource_capacity;
    uint64_t features;
    uint64_t max_document_bytes;
    uint64_t max_blob_bytes;
    uint32_t abi_major, abi_minor, abi_patch;
    cdin_backend_log_callback log_callback;
    void *log_context;
};

cdin_backend_status cdin_kernel_map_status(cdin_status status);
cdin_backend_status cdin_kernel_document_get(cdin_kernel_host *host, cdin_host_handle handle,
                                              cdin_doc_slot **out_slot);
cdin_backend_status cdin_kernel_resource_get(cdin_kernel_host *host, cdin_resource_slot *slots,
                                              cdin_host_handle handle, cdin_resource_slot **out_slot);
cdin_backend_status cdin_kernel_resource_new(cdin_kernel_host *host, cdin_resource_slot *slots,
                                              uint64_t raw, cdin_host_handle document,
                                              cdin_host_handle *out_handle);
void cdin_kernel_resource_remove(cdin_resource_slot *slot);
cdin_backend_status cdin_kernel_document_source_info(cdin_kernel_host *host, cdin_doc_slot *slot,
                                                       uint64_t *out_version, uint64_t *out_length);
void cdin_kernel_error_details_copy(const cdin_error_details *source, cdin_host_error_details *target);
cdin_backend_status cdin_kernel_edit_call_result(cdin_status status, const cdin_edit_result *result,
                                                  cdin_host_edit_result *out_result);
cdin_backend_status cdin_kernel_read_source(cdin_kernel_host *host, cdin_source source,
                                             uint64_t start, uint64_t length,
                                             int has_expected, uint64_t expected,
                                             cdin_host_read_result *out_result);

#endif
