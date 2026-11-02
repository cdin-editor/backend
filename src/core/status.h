#ifndef CDIN_BACKEND_STATUS_INTERNAL_H
#define CDIN_BACKEND_STATUS_INTERNAL_H
#include "cdin/backend.h"
#include <stdint.h>

#define CDIN_BACKEND_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define CDIN_BACKEND_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CDIN_BACKEND_MAX(a, b) ((a) > (b) ? (a) : (b))

int cdin_backend_size_add(size_t a, size_t b, size_t *out);
int cdin_backend_u64_add(uint64_t a, uint64_t b, uint64_t *out);

#endif
