#ifndef CDIN_BACKEND_NATIVE_PATH_H
#define CDIN_BACKEND_NATIVE_PATH_H
#include <stddef.h>
#ifdef _WIN32
wchar_t *cdin_utf8_to_wide(const char *value);
char *cdin_wide_to_utf8(const wchar_t *value);
#endif
#endif
