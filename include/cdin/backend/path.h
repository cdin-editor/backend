#ifndef CDIN_BACKEND_PATH_H
#define CDIN_BACKEND_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Paths accepted by this API are UTF-8. Returned paths use '/' on every
 * platform, so callers can persist and compare them without host separators.
 * Every allocated result belongs to the caller and must be freed with free().
 */
int cdin_path_is_absolute(const char *path);
int cdin_path_normalize(const char *path, char **out_path);
int cdin_path_absolute(const char *path, char **out_path);
int cdin_path_join(const char *left, const char *right, char **out_path);
int cdin_path_relative(const char *from_directory, const char *to_path, char **out_path);

/* Returns a borrowed pointer into path. It must not be freed separately. */
const char *cdin_path_basename(const char *path);

#ifdef __cplusplus
}
#endif

#endif
