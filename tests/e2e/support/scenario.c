#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

int scenario_temp_directory(char *buffer, size_t capacity)
{
    if (buffer == NULL || capacity == 0u) return -1;
#ifdef _WIN32
    {
        char base[MAX_PATH];
        char candidate[MAX_PATH];
        DWORD length = GetTempPathA((DWORD)sizeof(base), base);
        if (length == 0u || length >= sizeof(base)) return -1;
        if (GetTempFileNameA(base, "cbe", 0u, candidate) == 0u) return -1;
        if (!DeleteFileA(candidate) || !CreateDirectoryA(candidate, NULL)) return -1;
        if (strlen(candidate) + 1u > capacity) {
            (void)RemoveDirectoryA(candidate);
            return -1;
        }
        (void)strcpy(buffer, candidate);
        return 0;
    }
#else
    {
        int written = snprintf(buffer, capacity, "/tmp/cdin-backend-e2e-XXXXXX");
        if (written < 0 || (size_t)written >= capacity) return -1;
        return mkdtemp(buffer) != NULL ? 0 : -1;
    }
#endif
}

int scenario_path_join(const char *left, const char *right, char *buffer, size_t capacity)
{
    size_t left_length;
    int written;
    if (left == NULL || right == NULL || buffer == NULL || capacity == 0u) return -1;
    left_length = strlen(left);
    written = snprintf(buffer, capacity, "%s%s%s", left,
                       left_length != 0u && left[left_length - 1u] != '/' && left[left_length - 1u] != '\\' ? "/" : "",
                       right);
    if (written < 0 || (size_t)written >= capacity) return -1;
#ifdef _WIN32
    char *cursor;
    /*
     * Backend path APIs expose normalized slash-separated paths on every
     * platform. Keep the independent test helper's expected path in that
     * representation while preserving compatibility with Win32 file APIs.
     */
    for (cursor = buffer; *cursor != '\0'; cursor++) {
        if (*cursor == '\\') *cursor = '/';
    }
#endif
    return 0;
}

void scenario_sleep_ms(unsigned milliseconds)
{
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
#endif
}

int scenario_remove_file(const char *path)
{
    if (path == NULL) return -1;
#ifdef _WIN32
    return DeleteFileA(path) ? 0 : -1;
#else
    return unlink(path);
#endif
}

int scenario_remove_directory(const char *path)
{
    if (path == NULL) return -1;
#ifdef _WIN32
    return RemoveDirectoryA(path) ? 0 : -1;
#else
    return rmdir(path);
#endif
}
