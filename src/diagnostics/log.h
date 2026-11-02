#ifndef CDIN_BACKEND_DIAGNOSTICS_LOG_H
#define CDIN_BACKEND_DIAGNOSTICS_LOG_H

#include "cdin/backend.h"
#include <stdint.h>

typedef struct cdin_kernel_host cdin_kernel_host;

#if defined(__GNUC__) || defined(__clang__)
#define CDIN_BACKEND_PRINTF_FORMAT(format_index, argument_index) \
    __attribute__((format(printf, format_index, argument_index)))
#else
#define CDIN_BACKEND_PRINTF_FORMAT(format_index, argument_index)
#endif

/*
 * Backend diagnostics are records in the Kernel-owned logger. This small
 * facade gives every native record the same searchable prefix and source
 * location without exposing Kernel headers to ordinary Backend modules.
 */
void cdin_backend_logf(cdin_kernel_host *kernel,
                       cdin_backend_log_level level,
                       const char *area,
                       const char *file,
                       uint32_t line,
                       const char *function,
                       const char *format,
                       ...) CDIN_BACKEND_PRINTF_FORMAT(7, 8);

#define CDIN_BACKEND_TRACE(kernel, area, ...) \
    cdin_backend_logf((kernel), CDIN_BACKEND_LOG_TRACE, (area), __FILE__, \
                      (uint32_t)__LINE__, __func__, __VA_ARGS__)
#define CDIN_BACKEND_DEBUG(kernel, area, ...) \
    cdin_backend_logf((kernel), CDIN_BACKEND_LOG_DEBUG, (area), __FILE__, \
                      (uint32_t)__LINE__, __func__, __VA_ARGS__)
#define CDIN_BACKEND_INFO(kernel, area, ...) \
    cdin_backend_logf((kernel), CDIN_BACKEND_LOG_INFO, (area), __FILE__, \
                      (uint32_t)__LINE__, __func__, __VA_ARGS__)
#define CDIN_BACKEND_WARN(kernel, area, ...) \
    cdin_backend_logf((kernel), CDIN_BACKEND_LOG_WARN, (area), __FILE__, \
                      (uint32_t)__LINE__, __func__, __VA_ARGS__)
#define CDIN_BACKEND_ERROR(kernel, area, ...) \
    cdin_backend_logf((kernel), CDIN_BACKEND_LOG_ERROR, (area), __FILE__, \
                      (uint32_t)__LINE__, __func__, __VA_ARGS__)
#define CDIN_BACKEND_FATAL(kernel, area, ...) \
    cdin_backend_logf((kernel), CDIN_BACKEND_LOG_FATAL, (area), __FILE__, \
                      (uint32_t)__LINE__, __func__, __VA_ARGS__)

#endif
