#ifndef CDIN_BACKEND_THREAD_H
#define CDIN_BACKEND_THREAD_H

#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION cdin_mutex;
typedef CONDITION_VARIABLE cdin_cond;
typedef HANDLE cdin_thread;
#define CDIN_THREAD_RETURN DWORD WINAPI
#define CDIN_THREAD_CALL WINAPI
static inline int cdin_mutex_init(cdin_mutex *m) { InitializeCriticalSection(m); return 0; }
static inline void cdin_mutex_destroy(cdin_mutex *m) { DeleteCriticalSection(m); }
static inline void cdin_mutex_lock(cdin_mutex *m) { EnterCriticalSection(m); }
static inline void cdin_mutex_unlock(cdin_mutex *m) { LeaveCriticalSection(m); }
static inline int cdin_cond_init(cdin_cond *c) { InitializeConditionVariable(c); return 0; }
static inline void cdin_cond_destroy(cdin_cond *c) { (void)c; }
static inline void cdin_cond_wait(cdin_cond *c, cdin_mutex *m) { (void)SleepConditionVariableCS(c, m, INFINITE); }
static inline void cdin_cond_signal(cdin_cond *c) { WakeConditionVariable(c); }
static inline void cdin_cond_broadcast(cdin_cond *c) { WakeAllConditionVariable(c); }
#else
#include <pthread.h>
typedef pthread_mutex_t cdin_mutex;
typedef pthread_cond_t cdin_cond;
typedef pthread_t cdin_thread;
#define CDIN_THREAD_RETURN void *
#define CDIN_THREAD_CALL
static inline int cdin_mutex_init(cdin_mutex *m) { return pthread_mutex_init(m, NULL); }
static inline void cdin_mutex_destroy(cdin_mutex *m) { (void)pthread_mutex_destroy(m); }
static inline void cdin_mutex_lock(cdin_mutex *m) { (void)pthread_mutex_lock(m); }
static inline void cdin_mutex_unlock(cdin_mutex *m) { (void)pthread_mutex_unlock(m); }
static inline int cdin_cond_init(cdin_cond *c) { return pthread_cond_init(c, NULL); }
static inline void cdin_cond_destroy(cdin_cond *c) { (void)pthread_cond_destroy(c); }
static inline void cdin_cond_wait(cdin_cond *c, cdin_mutex *m) { (void)pthread_cond_wait(c, m); }
static inline void cdin_cond_signal(cdin_cond *c) { (void)pthread_cond_signal(c); }
static inline void cdin_cond_broadcast(cdin_cond *c) { (void)pthread_cond_broadcast(c); }
#endif

int cdin_thread_create(cdin_thread *thread, CDIN_THREAD_RETURN (CDIN_THREAD_CALL *entry)(void *), void *arg);
void cdin_thread_join(cdin_thread thread);
void cdin_thread_sleep_ms(uint32_t milliseconds);

#endif
