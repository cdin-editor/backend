/*
 * Kernel adapter lifecycle. BE-004 owns ABI checks, allocator configuration,
 * logger configuration and deterministic destruction. Document and resource
 * operations live in separate files so the ABI lifecycle remains small.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md
 */
#include "adapter_internal.h"
#include "../diagnostics/log.h"
#include <stdlib.h>
#include <string.h>

static void *kernel_alloc(void *context, size_t size)
{
    (void)context;
    return malloc(size == 0u ? 1u : size);
}

static void *kernel_realloc(void *context, void *pointer, size_t old_size, size_t new_size)
{
    (void)context;
    (void)old_size;
    if (new_size == 0u) return NULL;
    return realloc(pointer, new_size);
}

static void kernel_free(void *context, void *pointer, size_t size)
{
    (void)context;
    (void)size;
    free(pointer);
}

static void kernel_log_sink(void *context, int level, const char *message, size_t length)
{
    cdin_kernel_host *host = (cdin_kernel_host *)context;
    if (host != NULL && host->log_callback != NULL) {
        host->log_callback(host->log_context, level, message, length);
    }
}

/* BE-003: Backend is the only direct client of the Kernel ABI. Runtime asks
 * for semantic capabilities through this adapter and never includes
 * cdin/kernel.h directly.
 * Spec: https://github.com/cdin-editor/spec/blob/c4de9bf3d506b4e1f07b8986531cd056341708b0/docs/components/backend.md#L64 */
int cdin_kernel_host_has_feature(const cdin_kernel_host *host, cdin_host_feature feature)
{
    uint64_t bit;

    if (host == NULL) return 0;
    switch (feature) {
        case CDIN_HOST_FEATURE_BATCH: bit = CDIN_FEAT_BATCH; break;
        case CDIN_HOST_FEATURE_TRANSACTIONS: bit = CDIN_FEAT_TRANSACTIONS; break;
        case CDIN_HOST_FEATURE_SNAPSHOTS: bit = CDIN_FEAT_SNAPSHOTS; break;
        case CDIN_HOST_FEATURE_NOTIFICATIONS: bit = CDIN_FEAT_NOTIFICATIONS; break;
        case CDIN_HOST_FEATURE_SEARCH_LITERAL: bit = CDIN_FEAT_SEARCH_LITERAL; break;
        case CDIN_HOST_FEATURE_SEARCH_BYTES: bit = CDIN_FEAT_SEARCH_BYTES; break;
        case CDIN_HOST_FEATURE_SEARCH_REGEX: bit = CDIN_FEAT_SEARCH_REGEX; break;
        default: return 0;
    }
    return (host->features & bit) != 0u;
}

size_t cdin_kernel_host_default_blob_capacity(void)
{
    return (size_t)CDIN_DEFAULT_MAX_BLOB_SIZE;
}

size_t cdin_kernel_host_max_batch_edits(void)
{
    return (size_t)CDIN_MAX_BATCH_EDITS_HARD;
}

cdin_backend_status cdin_kernel_host_create(uint64_t max_documents, uint64_t max_document_bytes,
                                             uint64_t max_undo_bytes, int log_level, int log_output,
                                             uint64_t max_log_message_bytes,
                                             cdin_backend_log_callback log_callback, void *log_context,
                                             cdin_kernel_host **out_host)
{
    cdin_kernel_host *host = NULL;
    cdin_kernel_config config;
    cdin_log_options log_options;
    cdin_status status;
    uint32_t major = 0u, minor = 0u, patch = 0u;
    size_t resources;
    if (out_host == NULL || max_documents == 0u || max_documents > SIZE_MAX / sizeof(cdin_doc_slot) ||
        max_documents > SIZE_MAX / 16u || max_documents > UINT32_MAX || max_log_message_bytes == 0u ||
        max_log_message_bytes > CDIN_LOG_MAX_MESSAGE_BYTES || log_output < CDIN_LOG_OUTPUT_CALLBACK ||
        log_output > CDIN_LOG_OUTPUT_BOTH || (log_output == CDIN_LOG_OUTPUT_CALLBACK && log_callback == NULL))
        return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_host = NULL;
    status = cdin_abi_version(&major, &minor, &patch);
    if (status != CDIN_OK) return cdin_kernel_map_status(status);
    if (major != 1u || minor < 1u) return CDIN_BACKEND_VERSION_MISMATCH;
    host = (cdin_kernel_host *)calloc(1u, sizeof(*host));
    if (host == NULL) return CDIN_BACKEND_NO_MEMORY;
    status = cdin_features(&host->features);
    if (status != CDIN_OK) { free(host); return cdin_kernel_map_status(status); }
    if ((host->features & (CDIN_FEAT_BATCH | CDIN_FEAT_TRANSACTIONS | CDIN_FEAT_SNAPSHOTS |
                           CDIN_FEAT_NOTIFICATIONS | CDIN_FEAT_SEARCH_LITERAL | CDIN_FEAT_SEARCH_BYTES)) !=
        (CDIN_FEAT_BATCH | CDIN_FEAT_TRANSACTIONS | CDIN_FEAT_SNAPSHOTS |
         CDIN_FEAT_NOTIFICATIONS | CDIN_FEAT_SEARCH_LITERAL | CDIN_FEAT_SEARCH_BYTES)) {
        free(host);
        return CDIN_BACKEND_UNSUPPORTED;
    }
    resources = (size_t)max_documents * 16u;
    if (resources < (size_t)max_documents || resources > SIZE_MAX / sizeof(cdin_resource_slot)) { free(host); return CDIN_BACKEND_LIMIT_EXCEEDED; }
    host->documents = (cdin_doc_slot *)calloc((size_t)max_documents, sizeof(*host->documents));
    host->snapshots = (cdin_resource_slot *)calloc(resources, sizeof(*host->snapshots));
    host->transactions = (cdin_resource_slot *)calloc(resources, sizeof(*host->transactions));
    host->subscriptions = (cdin_resource_slot *)calloc(resources, sizeof(*host->subscriptions));
    if (host->documents == NULL || host->snapshots == NULL || host->transactions == NULL || host->subscriptions == NULL) goto fail;
    host->document_capacity = (size_t)max_documents;
    host->resource_capacity = resources;
    host->abi_major = major; host->abi_minor = minor; host->abi_patch = patch;
    host->max_document_bytes = max_document_bytes == 0u ? CDIN_DEFAULT_MAX_DOCUMENT_SIZE : max_document_bytes;
    host->max_blob_bytes = CDIN_DEFAULT_MAX_BLOB_SIZE;
    host->log_callback = log_callback;
    host->log_context = log_context;
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.abi_major = CDIN_KERNEL_ABI_MAJOR;
    config.allocator.struct_size = (uint32_t)sizeof(config.allocator);
    config.allocator.alloc = kernel_alloc;
    config.allocator.realloc = kernel_realloc;
    config.allocator.free = kernel_free;
    if (log_callback != NULL) {
        config.log.struct_size = (uint32_t)sizeof(config.log);
        config.log.ctx = host;
        config.log.log = kernel_log_sink;
    }
    config.max_documents = max_documents;
    config.max_document_size = host->max_document_bytes;
    config.max_undo_memory = max_undo_bytes;
    config.max_snapshots_per_doc = 16u;
    config.max_subscribers_per_doc = 16u;
    config.notif_queue_capacity = 64u;
    status = cdin_kernel_create(&config, &host->kernel);
    if (status != CDIN_OK) goto kernel_fail;
    status = cdin_log_options_init(&log_options);
    if (status != CDIN_OK) goto kernel_fail_destroy;
    log_options.min_level = (uint32_t)log_level;
    log_options.output = (uint32_t)log_output;
    log_options.format_flags = CDIN_LOG_F_DEFAULT;
    log_options.max_message_bytes = (uint32_t)max_log_message_bytes;
    status = cdin_kernel_set_log_options(host->kernel, &log_options);
    if (status != CDIN_OK) goto kernel_fail_destroy;
    *out_host = host;
    CDIN_BACKEND_INFO(host, "KERNEL",
                      "connected abi=%u.%u.%u features=0x%016llx documents=%llu document_limit=%llu",
                      (unsigned)host->abi_major, (unsigned)host->abi_minor, (unsigned)host->abi_patch,
                      (unsigned long long)host->features,
                      (unsigned long long)max_documents,
                      (unsigned long long)host->max_document_bytes);
    return CDIN_BACKEND_OK;

kernel_fail_destroy:
    (void)cdin_kernel_destroy(host->kernel);
kernel_fail:
    free(host->subscriptions); free(host->transactions); free(host->snapshots); free(host->documents); free(host);
    return cdin_kernel_map_status(status);
fail:
    free(host->subscriptions); free(host->transactions); free(host->snapshots); free(host->documents); free(host);
    return CDIN_BACKEND_NO_MEMORY;
}

void cdin_kernel_host_destroy(cdin_kernel_host *host)
{
    size_t i;
    if (host == NULL) return;
    if (host->kernel != NULL) {
        CDIN_BACKEND_TRACE(host, "KERNEL", "releasing adapter resources capacity=%llu",
                           (unsigned long long)host->resource_capacity);
        for (i = 0u; i < host->resource_capacity; i++) {
            if (host->transactions[i].active) (void)cdin_txn_abort(host->kernel, (cdin_txn)host->transactions[i].raw);
            if (host->snapshots[i].active) (void)cdin_snapshot_release(host->kernel, (cdin_snapshot)host->snapshots[i].raw);
            if (host->subscriptions[i].active) (void)cdin_notif_unsubscribe(host->kernel, (cdin_notif)host->subscriptions[i].raw);
        }
        for (i = 0u; i < host->document_capacity; i++) if (host->documents[i].active) (void)cdin_doc_close(host->kernel, host->documents[i].raw);
        (void)cdin_kernel_shutdown(host->kernel, CDIN_SHUTDOWN_ABORT);
        (void)cdin_kernel_destroy(host->kernel);
    }
    free(host->subscriptions); free(host->transactions); free(host->snapshots); free(host->documents); free(host);
}

cdin_backend_status cdin_kernel_host_log(cdin_kernel_host *host, int level,
                                          const char *component, const char *message)
{
    cdin_status status;
    cdin_log_level kernel_level;
    if (host == NULL || host->kernel == NULL || component == NULL || message == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    switch (level) {
        case CDIN_BACKEND_LOG_ERROR: kernel_level = CDIN_LOG_ERROR; break;
        case CDIN_BACKEND_LOG_WARN: kernel_level = CDIN_LOG_WARN; break;
        case CDIN_BACKEND_LOG_INFO: kernel_level = CDIN_LOG_INFO; break;
        case CDIN_BACKEND_LOG_DEBUG: kernel_level = CDIN_LOG_DEBUG; break;
        case CDIN_BACKEND_LOG_TRACE: kernel_level = CDIN_LOG_TRACE; break;
        case CDIN_BACKEND_LOG_FATAL: kernel_level = CDIN_LOG_FATAL; break;
        default: return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    status = cdin_kernel_log(host->kernel, kernel_level, component, message);
    if (status == CDIN_UNSUPPORTED) return CDIN_BACKEND_UNSUPPORTED;
    return cdin_kernel_map_status(status);
}

