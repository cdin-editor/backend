#ifndef CDIN_BACKEND_PROCESS_H
#define CDIN_BACKEND_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdin_process cdin_process;

typedef struct cdin_process_options {
    int capture_stdout;
    int capture_stderr;
    int inherit_environment;
    const char *working_directory;
    const char *const *environment;
} cdin_process_options;

typedef enum cdin_process_state {
    CDIN_PROCESS_RUNNING = 0,
    CDIN_PROCESS_EXITED,
    CDIN_PROCESS_FAILED
} cdin_process_state;

/* argv[0] is the executable. No shell or command-string parsing is involved. */
int cdin_process_spawn(const char *const argv[], const cdin_process_options *options,
                       cdin_process **out_process);
/* Returns bytes currently available; zero means none now, negative means EOF/error. */
int cdin_process_read_stdout(cdin_process *process, void *buffer, size_t capacity,
                             size_t *out_read);
int cdin_process_read_stderr(cdin_process *process, void *buffer, size_t capacity,
                             size_t *out_read);
/* poll: 1 exited, 0 running, -1 error. wait: 1 exited, 0 timeout, -1 error. */
int cdin_process_poll(cdin_process *process, int *out_exit_code);
int cdin_process_wait(cdin_process *process, uint32_t timeout_ms, int *out_exit_code);
int cdin_process_terminate(cdin_process *process);
void cdin_process_destroy(cdin_process *process);

#ifdef __cplusplus
}
#endif

#endif
