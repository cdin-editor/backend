/*
 * BE-007 / ARCH-025 define the thread boundary: workers never enter Lua and
 * Lua remains owned by the Backend's creating thread. This file isolates the
 * native thread primitives so the job pool has the same contract on POSIX and
 * Windows without importing a UI or event-loop toolkit.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md
 */
#include "thread.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif
#ifdef _WIN32
typedef struct cdin_thread_start {
    CDIN_THREAD_RETURN (CDIN_THREAD_CALL *entry)(void *);
    void *argument;
} cdin_thread_start;

static DWORD WINAPI cdin_thread_entry(LPVOID value)
{
    cdin_thread_start *start = (cdin_thread_start *)value;
    CDIN_THREAD_RETURN (CDIN_THREAD_CALL *entry)(void *) = start->entry;
    void *argument = start->argument;
    (void)HeapFree(GetProcessHeap(), 0, start);
    return entry(argument);
}

int cdin_thread_create(cdin_thread *thread, CDIN_THREAD_RETURN (CDIN_THREAD_CALL *entry)(void *), void *arg)
{
    cdin_thread_start *start;
    if (thread == NULL || entry == NULL) return -1;
    start = (cdin_thread_start *)HeapAlloc(GetProcessHeap(), 0, sizeof(*start));
    if (start == NULL) return -1;
    start->entry = entry;
    start->argument = arg;
    *thread = CreateThread(NULL, 0, cdin_thread_entry, start, 0, NULL);
    if (*thread == NULL) { (void)HeapFree(GetProcessHeap(), 0, start); return -1; }
    return 0;
}

void cdin_thread_join(cdin_thread thread)
{
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
}

void cdin_thread_sleep_ms(uint32_t milliseconds)
{
    Sleep((DWORD)milliseconds);
}
#else
int cdin_thread_create(cdin_thread *thread, CDIN_THREAD_RETURN (CDIN_THREAD_CALL *entry)(void *), void *arg)
{
    return pthread_create(thread, NULL, entry, arg);
}
void cdin_thread_join(cdin_thread thread)
{
    (void)pthread_join(thread, NULL);
}

void cdin_thread_sleep_ms(uint32_t milliseconds)
{
    struct timespec pause;
    pause.tv_sec = (time_t)(milliseconds / 1000u);
    pause.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) { }
}
#endif
