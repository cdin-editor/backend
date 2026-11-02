#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

static void sleep_ms(unsigned milliseconds)
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

int main(int argc, char **argv)
{
    if (argc < 2) return 64;
    if (strcmp(argv[1], "emit") == 0) {
        if (argc != 3) return 65;
        (void)fputs(argv[2], stdout);
        (void)fflush(stdout);
        return 0;
    }
    if (strcmp(argv[1], "sleep") == 0) {
        char *end = NULL;
        unsigned long duration;
        if (argc != 3) return 65;
        duration = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || duration > 60000ul) return 65;
        sleep_ms((unsigned)duration);
        return 0;
    }
    if (strcmp(argv[1], "exit") == 0) {
        char *end = NULL;
        long code;
        if (argc != 3) return 65;
        code = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || code < 0 || code > 255) return 65;
        return (int)code;
    }
    return 64;
}
