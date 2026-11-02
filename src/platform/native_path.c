/*
 * BE-005 requires OS services to behave independently of the chosen Frontend.
 * Keep Backend paths in UTF-8 and convert only at the Windows API boundary;
 * rejecting invalid sequences is safer than silently changing a path that
 * identifies a user file.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 */
#include "native_path.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
wchar_t *cdin_utf8_to_wide(const char *value)
{
    int count;
    wchar_t *result;
    if (value == NULL) return NULL;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (count <= 0) return NULL;
    result = (wchar_t *)calloc((size_t)count, sizeof(wchar_t));
    if (result == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, count) != count) {
        free(result); return NULL;
    }
    return result;
}
char *cdin_wide_to_utf8(const wchar_t *value)
{
    int count;
    char *result;
    if (value == NULL) return NULL;
    count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    if (count <= 0) return NULL;
    result = (char *)calloc((size_t)count, 1u);
    if (result == NULL) return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result, count, NULL, NULL) != count) {
        free(result); return NULL;
    }
    return result;
}
#else
typedef int cdin_native_path_posix_translation_unit;
#endif
