/*
 * BE-003 / ABI-014 keep Kernel ABI values behind the adapter. This module
 * defines Backend-owned status names and overflow-safe arithmetic so callers
 * do not depend on vendor enums or duplicate size checks.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/abi-and-compatibility.md
 */
#include "status.h"
#include <limits.h>

int cdin_backend_size_add(size_t a, size_t b, size_t *out)
{
    if (out == NULL || b > SIZE_MAX - a) return 0;
    *out = a + b;
    return 1;
}

int cdin_backend_u64_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (out == NULL || b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

const char *cdin_backend_status_name(cdin_backend_status status)
{
    static const char *const names[] = {
        "OK", "INVALID_ARGUMENT", "INVALID_STATE", "NO_MEMORY", "UNSUPPORTED",
        "VERSION_MISMATCH", "LIMIT_EXCEEDED", "QUEUE_FULL", "NOT_FOUND", "BUSY", "STALE_VERSION",
        "OUT_OF_RANGE", "INVALID_POSITION", "PATTERN_INVALID", "BUFFER_TOO_SMALL", "SHUTDOWN",
        "IO_ERROR", "CANCELLED", "INTERNAL", "PERMISSION_DENIED"
    };
    if ((unsigned)status >= CDIN_BACKEND_ARRAY_COUNT(names)) return "UNKNOWN";
    return names[(unsigned)status];
}

const char *cdin_backend_state_name(cdin_backend_state state)
{
    static const char *const names[] = {
        "created", "starting", "running", "stopping", "stopped", "failed"
    };
    if ((unsigned)state >= CDIN_BACKEND_ARRAY_COUNT(names)) return "unknown";
    return names[(unsigned)state];
}

const char *cdin_backend_version(void)
{
    return "1.0.0";
}
