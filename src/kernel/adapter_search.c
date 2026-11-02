/*
 * Literal/byte search adapter. Regex remains unsupported until the Kernel
 * feature contract advertises it; never silently substitute another engine.
 * https://github.com/cdin-editor/spec/blob/main/docs/kernel/search-engine.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/abi-and-compatibility.md
 */
#include "adapter_internal.h"
#include <stdlib.h>
#include <string.h>

cdin_backend_status cdin_kernel_host_search(cdin_kernel_host *host, cdin_host_handle document,
                                             const uint8_t *pattern, size_t pattern_length,
                                             uint32_t mode, uint32_t direction,
                                             uint64_t from, uint64_t bound, uint64_t limit,
                                             int has_expected, uint64_t expected,
                                             uint32_t search_flags, uint64_t step_limit,
                                             cdin_host_search_result *out_result)
{
    cdin_doc_slot *doc;
    cdin_search_request request;
    cdin_search_result result;
    uint64_t *ranges = NULL;
    cdin_status status;
    cdin_backend_status mapped;
    cdin_search_mode kernel_mode;
    cdin_search_direction kernel_direction;
    uint32_t kernel_flags = CDIN_SEARCH_WANT_PARTIAL;
    if (out_result == NULL || (pattern == NULL && pattern_length != 0u) || limit == 0u || limit > SIZE_MAX / (2u * sizeof(uint64_t))) return CDIN_BACKEND_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    switch (mode) {
        case CDIN_HOST_SEARCH_LITERAL:
            kernel_mode = CDIN_SEARCH_LITERAL;
            break;
        case CDIN_HOST_SEARCH_REGEX:
            return CDIN_BACKEND_UNSUPPORTED;
        case CDIN_HOST_SEARCH_BYTES:
            kernel_mode = CDIN_SEARCH_BYTES;
            break;
        default:
            return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    switch (direction) {
        case CDIN_HOST_SEARCH_FORWARD:
            kernel_direction = CDIN_SEARCH_FORWARD;
            break;
        case CDIN_HOST_SEARCH_BACKWARD:
            kernel_direction = CDIN_SEARCH_BACKWARD;
            break;
        default:
            return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if ((search_flags & (uint32_t)~(CDIN_HOST_SEARCH_CASE_INSENSITIVE |
                                    CDIN_HOST_SEARCH_ANY_NEWLINE)) != 0u) {
        return CDIN_BACKEND_INVALID_ARGUMENT;
    }
    if ((search_flags & CDIN_HOST_SEARCH_CASE_INSENSITIVE) != 0u)
        kernel_flags |= CDIN_SEARCH_CASE_INSENSITIVE;
    if ((search_flags & CDIN_HOST_SEARCH_ANY_NEWLINE) != 0u)
        kernel_flags |= CDIN_SEARCH_ANY_NEWLINE;
    mapped = cdin_kernel_document_get(host, document, &doc);
    if (mapped != CDIN_BACKEND_OK) return mapped;
    ranges = (uint64_t *)calloc((size_t)limit * 2u, sizeof(uint64_t));
    if (ranges == NULL) return CDIN_BACKEND_NO_MEMORY;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.flags = kernel_flags;
    request.src = cdin_src_doc(doc->raw);
    request.expected_version = has_expected ? expected : CDIN_ANY_VERSION;
    request.pattern = pattern; request.pattern_len = pattern_length;
    request.mode = kernel_mode;
    request.direction = kernel_direction;
    request.from = from; request.bound = bound;
    request.limit = limit; request.step_limit = step_limit;
    request.ranges = ranges; request.ranges_capacity = (size_t)limit;
    memset(&result, 0, sizeof(result)); result.struct_size = (uint32_t)sizeof(result);
    status = cdin_search(host->kernel, &request, &result);
    if (status != CDIN_OK) {
        cdin_kernel_error_details_copy(&result.details, &out_result->details);
        free(ranges);
        return cdin_kernel_map_status(status);
    }
    out_result->version = result.version;
    out_result->count = result.count;
    out_result->truncated = result.truncated;
    out_result->partial = result.partial;
    out_result->skipped_raw = result.skipped_raw;
    out_result->next_from = result.next_from;
    out_result->next_bound = result.next_bound;
    out_result->exhausted = result.exhausted;
    out_result->steps = result.steps;
    out_result->ranges = ranges;
    return CDIN_BACKEND_OK;
}

void cdin_kernel_host_search_result_free(cdin_host_search_result *result)
{
    if (result == NULL) return;
    free(result->ranges);
    memset(result, 0, sizeof(*result));
}

void cdin_kernel_host_abi_version(cdin_kernel_host *host, uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (major != NULL) *major = host == NULL ? 0u : host->abi_major;
    if (minor != NULL) *minor = host == NULL ? 0u : host->abi_minor;
    if (patch != NULL) *patch = host == NULL ? 0u : host->abi_patch;
}
