/*
 * BE-003 keeps the Kernel ABI behind one adapter; this facade is the only
 * place that formats native Backend diagnostic records. It deliberately
 * emits through cdin_kernel_log rather than creating a second logger.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 */
#include "log.h"
#include "../kernel/adapter.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CDIN_BACKEND_LOG_DETAIL_CAPACITY 1536u
#define CDIN_BACKEND_LOG_RECORD_CAPACITY 2048u

static const char *source_name(const char *file)
{
    const char *name;
    if (file == NULL) return "unknown";
    name = file;
    for (const char *cursor = file; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    }
    return name;
}

static int valid_area(const char *area)
{
    size_t i;
    if (area == NULL || area[0] == '\0' || strlen(area) > 24u) return 0;
    for (i = 0u; area[i] != '\0'; i++) {
        const unsigned char value = (unsigned char)area[i];
        if (!((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
              value == '_' || value == '-')) return 0;
    }
    return 1;
}

void cdin_backend_logf(cdin_kernel_host *kernel,
                       cdin_backend_log_level level,
                       const char *area,
                       const char *file,
                       uint32_t line,
                       const char *function,
                       const char *format,
                       ...)
{
    char detail[CDIN_BACKEND_LOG_DETAIL_CAPACITY];
    char record[CDIN_BACKEND_LOG_RECORD_CAPACITY];
    char component[48];
    va_list arguments;
    int detail_length;
    int record_length;

    /* Logging must never become a new failure path. Before Kernel creation,
     * there is intentionally no fallback logger or direct console output. */
    if (kernel == NULL || !valid_area(area) || format == NULL) return;

    va_start(arguments, format);
    detail_length = vsnprintf(detail, sizeof(detail), format, arguments);
    va_end(arguments);
    if (detail_length < 0) {
        (void)snprintf(detail, sizeof(detail), "diagnostic message formatting failed");
    } else if ((size_t)detail_length >= sizeof(detail)) {
        static const char suffix[] = "... [truncated]";
        const size_t suffix_length = sizeof(suffix) - 1u;
        memcpy(detail + sizeof(detail) - suffix_length - 1u, suffix, suffix_length + 1u);
    }

    record_length = snprintf(record, sizeof(record),
                             "[BACKEND][%s] %s:%u %s(): %s",
                             area, source_name(file), (unsigned)line,
                             function != NULL ? function : "unknown", detail);
    if (record_length < 0) return;
    if ((size_t)record_length >= sizeof(record)) {
        static const char suffix[] = "... [truncated]";
        const size_t suffix_length = sizeof(suffix) - 1u;
        memcpy(record + sizeof(record) - suffix_length - 1u, suffix, suffix_length + 1u);
    }

    (void)snprintf(component, sizeof(component), "cdin-backend.%s", area);
    (void)cdin_kernel_host_log(kernel, (int)level, component, record);
}
