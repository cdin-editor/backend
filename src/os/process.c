/*
 * BE-011 / ARCH-024 require argument-array process creation and explicitly
 * forbid routing an arbitrary command string through a shell. Keeping argv as
 * the API on both POSIX and Windows avoids platform-specific injection rules
 * leaking into Runtime callers.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md
 */
#include "process.h"
#include "../platform/clock.h"
#include "../platform/native_path.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <stdint.h>
#include <limits.h>
struct cdin_process {
    PROCESS_INFORMATION info;
    HANDLE stdout_read;
    HANDLE stderr_read;
    int stdout_closed;
    int stderr_closed;
    int exited;
    int exit_code;
};

static wchar_t *cdin_process_utf8(const char *value)
{
    return cdin_utf8_to_wide(value);
}

static int cdin_wide_append(wchar_t **buffer, size_t *length, size_t *capacity, wchar_t value)
{
    wchar_t *next;
    size_t new_capacity;
    if (*length + 2u > *capacity) {
        new_capacity = *capacity == 0u ? 128u : *capacity * 2u;
        if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(wchar_t)) return -1;
        next = (wchar_t *)realloc(*buffer, new_capacity * sizeof(wchar_t));
        if (next == NULL) return -1;
        *buffer = next;
        *capacity = new_capacity;
    }
    (*buffer)[(*length)++] = value;
    (*buffer)[*length] = L'\0';
    return 0;
}

static int cdin_append_quoted_argument(wchar_t **buffer, size_t *length,
                                       size_t *capacity, const wchar_t *argument)
{
    size_t i = 0u;
    size_t slashes = 0u;
    int quote = argument[0] == L'\0';
    for (i = 0u; argument[i] != L'\0'; i++) {
        if (argument[i] == L' ' || argument[i] == L'\t' || argument[i] == L'\n' || argument[i] == L'"') quote = 1;
    }
    if (!quote) {
        for (i = 0u; argument[i] != L'\0'; i++) if (cdin_wide_append(buffer, length, capacity, argument[i]) != 0) return -1;
        return 0;
    }
    if (cdin_wide_append(buffer, length, capacity, L'"') != 0) return -1;
    for (i = 0u;; i++) {
        wchar_t value = argument[i];
        if (value == L'\\') { slashes++; continue; }
        if (value == L'"') {
            size_t n;
            for (n = 0u; n < slashes * 2u + 1u; n++) if (cdin_wide_append(buffer, length, capacity, L'\\') != 0) return -1;
            if (cdin_wide_append(buffer, length, capacity, L'"') != 0) return -1;
            slashes = 0u;
            continue;
        }
        if (value == L'\0') {
            size_t n;
            for (n = 0u; n < slashes * 2u; n++) if (cdin_wide_append(buffer, length, capacity, L'\\') != 0) return -1;
            break;
        }
        while (slashes > 0u) {
            if (cdin_wide_append(buffer, length, capacity, L'\\') != 0) return -1;
            slashes--;
        }
        if (cdin_wide_append(buffer, length, capacity, value) != 0) return -1;
    }
    return cdin_wide_append(buffer, length, capacity, L'"');
}

static wchar_t *cdin_build_command_line(const char *const argv[])
{
    wchar_t *line = NULL;
    size_t length = 0u, capacity = 0u, i;
    for (i = 0u; argv[i] != NULL; i++) {
        wchar_t *argument = cdin_process_utf8(argv[i]);
        if (argument == NULL) { free(line); return NULL; }
        if (i != 0u && cdin_wide_append(&line, &length, &capacity, L' ') != 0) { free(argument); free(line); return NULL; }
        if (cdin_append_quoted_argument(&line, &length, &capacity, argument) != 0) { free(argument); free(line); return NULL; }
        free(argument);
    }
    if (length == 0u) return NULL;
    return line;
}

static int cdin_create_pipe(HANDLE *read_handle, HANDLE *write_handle, int capture)
{
    SECURITY_ATTRIBUTES attributes;
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = NULL;
    attributes.bInheritHandle = TRUE;
    *read_handle = NULL;
    *write_handle = NULL;
    if (!capture) return 0;
    if (!CreatePipe(read_handle, write_handle, &attributes, 0u)) return -1;
    if (!SetHandleInformation(*read_handle, HANDLE_FLAG_INHERIT, 0u)) {
        CloseHandle(*read_handle); CloseHandle(*write_handle);
        *read_handle = NULL; *write_handle = NULL;
        return -1;
    }
    return 0;
}

int cdin_process_spawn(const char *const argv[], const cdin_process_options *options,
                       cdin_process **out_process)
{
    cdin_process_options defaults;
    cdin_process *process = NULL;
    wchar_t *application = NULL, *command_line = NULL, *directory = NULL;
    HANDLE out_read = NULL, out_write = NULL, err_read = NULL, err_write = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION info;
    int capture_out, capture_err, result = -1;
    size_t env_count = 0u;
    wchar_t **env_wide = NULL;
    wchar_t *environment_block = NULL;
    size_t env_chars = 1u, cursor = 0u, i;
    if (out_process == NULL || argv == NULL || argv[0] == NULL || argv[0][0] == '\0') return -1;
    *out_process = NULL;
    memset(&defaults, 0, sizeof(defaults)); defaults.inherit_environment = 1;
    if (options == NULL) options = &defaults;
    capture_out = options->capture_stdout;
    capture_err = options->capture_stderr;
    application = cdin_process_utf8(argv[0]);
    command_line = cdin_build_command_line(argv);
    if (application == NULL || command_line == NULL) goto done;
    if (options->working_directory != NULL) {
        directory = cdin_process_utf8(options->working_directory);
        if (directory == NULL) goto done;
    }
    if (cdin_create_pipe(&out_read, &out_write, capture_out) != 0 ||
        cdin_create_pipe(&err_read, &err_write, capture_err) != 0) goto done;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = capture_out ? out_write : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = capture_err ? err_write : GetStdHandle(STD_ERROR_HANDLE);
    memset(&info, 0, sizeof(info));
    if (options->environment != NULL || !options->inherit_environment) {
        while (options->environment != NULL && options->environment[env_count] != NULL) env_count++;
        env_wide = (wchar_t **)calloc(env_count + 1u, sizeof(wchar_t *));
        if (env_wide == NULL) goto done;
        for (i = 0u; i < env_count; i++) {
            env_wide[i] = cdin_process_utf8(options->environment[i]);
            if (env_wide[i] == NULL) goto done;
            if (wcslen(env_wide[i]) > SIZE_MAX - env_chars - 1u) goto done;
            env_chars += wcslen(env_wide[i]) + 1u;
        }
        environment_block = (wchar_t *)calloc(env_chars + 1u, sizeof(wchar_t));
        if (environment_block == NULL) goto done;
        for (i = 0u; i < env_count; i++) {
            size_t amount = wcslen(env_wide[i]) + 1u;
            memcpy(environment_block + cursor, env_wide[i], amount * sizeof(wchar_t));
            cursor += amount;
        }
        environment_block[cursor] = L'\0';
    }
    if (!CreateProcessW(application, command_line, NULL, NULL, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                        environment_block, directory, &startup, &info)) goto done;
    process = (cdin_process *)calloc(1u, sizeof(*process));
    if (process == NULL) {
        (void)TerminateProcess(info.hProcess, 1u);
        (void)WaitForSingleObject(info.hProcess, INFINITE);
        CloseHandle(info.hThread); CloseHandle(info.hProcess);
        goto done;
    }
    process->info = info;
    process->stdout_read = out_read; out_read = NULL;
    process->stderr_read = err_read; err_read = NULL;
    *out_process = process;
    result = 0;
done:
    if (out_write != NULL) CloseHandle(out_write);
    if (err_write != NULL) CloseHandle(err_write);
    if (out_read != NULL) CloseHandle(out_read);
    if (err_read != NULL) CloseHandle(err_read);
    if (application != NULL) free(application);
    if (command_line != NULL) free(command_line);
    if (directory != NULL) free(directory);
    if (env_wide != NULL) { for (i = 0u; i < env_count; i++) free(env_wide[i]); free(env_wide); }
    free(environment_block);
    return result;
}

static int cdin_process_read_handle(HANDLE handle, int *closed, void *buffer, size_t capacity, size_t *out_read)
{
    DWORD available = 0u, amount = 0u;
    if (out_read == NULL || (buffer == NULL && capacity != 0u)) return -1;
    *out_read = 0u;
    if (handle == NULL || *closed) return -1;
    if (!PeekNamedPipe(handle, NULL, 0u, NULL, &available, NULL)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) { *closed = 1; return -1; }
        return -1;
    }
    if (available == 0u) return 0;
    if (capacity < (size_t)available) available = (DWORD)capacity;
    if (available == 0u) return 0;
    if (!ReadFile(handle, buffer, available, &amount, NULL)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) *closed = 1;
        return -1;
    }
    *out_read = (size_t)amount;
    return 0;
}
int cdin_process_read_stdout(cdin_process *process, void *buffer, size_t capacity, size_t *out_read)
{ return process == NULL ? -1 : cdin_process_read_handle(process->stdout_read, &process->stdout_closed, buffer, capacity, out_read); }
int cdin_process_read_stderr(cdin_process *process, void *buffer, size_t capacity, size_t *out_read)
{ return process == NULL ? -1 : cdin_process_read_handle(process->stderr_read, &process->stderr_closed, buffer, capacity, out_read); }
int cdin_process_poll(cdin_process *process, int *out_exit_code)
{
    DWORD code;
    if (process == NULL) return -1;
    if (process->exited) { if (out_exit_code != NULL) *out_exit_code = process->exit_code; return 1; }
    if (WaitForSingleObject(process->info.hProcess, 0u) == WAIT_TIMEOUT) return 0;
    if (!GetExitCodeProcess(process->info.hProcess, &code)) return -1;
    process->exited = 1; process->exit_code = (int)code;
    if (out_exit_code != NULL) *out_exit_code = process->exit_code;
    return 1;
}
int cdin_process_wait(cdin_process *process, uint32_t timeout_ms, int *out_exit_code)
{
    DWORD value;
    if (process == NULL) return -1;
    value = WaitForSingleObject(process->info.hProcess, timeout_ms);
    if (value == WAIT_TIMEOUT) return 0;
    if (value != WAIT_OBJECT_0) return -1;
    return cdin_process_poll(process, out_exit_code);
}
int cdin_process_terminate(cdin_process *process)
{ return process != NULL && !process->exited && TerminateProcess(process->info.hProcess, 1u) ? 0 : -1; }
void cdin_process_destroy(cdin_process *process)
{
    if (process == NULL) return;
    if (!process->exited && process->info.hProcess != NULL) {
        (void)TerminateProcess(process->info.hProcess, 1u);
        (void)WaitForSingleObject(process->info.hProcess, INFINITE);
    }
    if (process->stdout_read != NULL) CloseHandle(process->stdout_read);
    if (process->stderr_read != NULL) CloseHandle(process->stderr_read);
    if (process->info.hThread != NULL) CloseHandle(process->info.hThread);
    if (process->info.hProcess != NULL) CloseHandle(process->info.hProcess);
    free(process);
}
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
struct cdin_process {
    pid_t pid;
    int stdout_read;
    int stderr_read;
    int exited;
    int exit_code;
};

static int cdin_pipe_cloexec(int descriptors[2])
{
    if (pipe(descriptors) != 0) return -1;
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) == -1 || fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(descriptors[0]); close(descriptors[1]); return -1;
    }
    return 0;
}
static int cdin_set_nonblocking(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
}

int cdin_process_spawn(const char *const argv[], const cdin_process_options *options,
                       cdin_process **out_process)
{
    cdin_process_options defaults;
    posix_spawn_file_actions_t actions;
    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    int actions_ready = 0, result = -1, spawn_error;
    cdin_process *process = NULL;
    pid_t pid = -1;
    char *const *environment;
    if (out_process == NULL || argv == NULL || argv[0] == NULL || argv[0][0] == '\0') return -1;
    *out_process = NULL;
    memset(&defaults, 0, sizeof(defaults)); defaults.inherit_environment = 1;
    if (options == NULL) options = &defaults;
    if (options->capture_stdout && cdin_pipe_cloexec(out_pipe) != 0) goto done;
    if (options->capture_stderr && cdin_pipe_cloexec(err_pipe) != 0) goto done;
    if (posix_spawn_file_actions_init(&actions) != 0) goto done;
    actions_ready = 1;
    if (options->capture_stdout) {
        if (posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO) != 0 ||
            posix_spawn_file_actions_addclose(&actions, out_pipe[0]) != 0 ||
            posix_spawn_file_actions_addclose(&actions, out_pipe[1]) != 0) goto done;
    }
    if (options->working_directory != NULL) {
#if defined(__APPLE__) || defined(__GLIBC__)
        if (posix_spawn_file_actions_addchdir_np(&actions, options->working_directory) != 0) goto done;
#else
        goto done;
#endif
    }
    if (options->capture_stderr) {
        if (posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO) != 0 ||
            posix_spawn_file_actions_addclose(&actions, err_pipe[0]) != 0 ||
            posix_spawn_file_actions_addclose(&actions, err_pipe[1]) != 0) goto done;
    }
    environment = options->environment != NULL ? (char *const *)options->environment :
                  (options->inherit_environment ? environ : (char *const[]){ NULL });
    spawn_error = posix_spawnp(&pid, argv[0], &actions, NULL, (char *const *)argv, environment);
    if (spawn_error != 0) { errno = spawn_error; goto done; }
    process = (cdin_process *)calloc(1u, sizeof(*process));
    if (process == NULL) { (void)kill(pid, SIGTERM); (void)waitpid(pid, NULL, 0); goto done; }
    process->pid = pid;
    process->stdout_read = -1;
    process->stderr_read = -1;
    process->stdout_read = out_pipe[0]; out_pipe[0] = -1;
    process->stderr_read = err_pipe[0]; err_pipe[0] = -1;
    if ((process->stdout_read >= 0 && cdin_set_nonblocking(process->stdout_read) != 0) ||
        (process->stderr_read >= 0 && cdin_set_nonblocking(process->stderr_read) != 0)) {
        cdin_process_destroy(process); process = NULL; (void)kill(pid, SIGTERM); (void)waitpid(pid, NULL, 0); goto done;
    }
    *out_process = process;
    result = 0;
done:
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);
    return result;
}

static int cdin_process_read_fd(int descriptor, void *buffer, size_t capacity, size_t *out_read)
{
    ssize_t count;
    if (out_read == NULL || (buffer == NULL && capacity != 0u) || descriptor < 0) return -1;
    *out_read = 0u;
    if (capacity == 0u) return 0;
    count = read(descriptor, buffer, capacity);
    if (count > 0) { *out_read = (size_t)count; return 0; }
    if (count == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    if (errno == EINTR) return 0;
    return -1;
}
int cdin_process_read_stdout(cdin_process *process, void *buffer, size_t capacity, size_t *out_read)
{ return process == NULL ? -1 : cdin_process_read_fd(process->stdout_read, buffer, capacity, out_read); }
int cdin_process_read_stderr(cdin_process *process, void *buffer, size_t capacity, size_t *out_read)
{ return process == NULL ? -1 : cdin_process_read_fd(process->stderr_read, buffer, capacity, out_read); }
int cdin_process_poll(cdin_process *process, int *out_exit_code)
{
    int status;
    pid_t result;
    if (process == NULL) return -1;
    if (process->exited) { if (out_exit_code != NULL) *out_exit_code = process->exit_code; return 1; }
    do { result = waitpid(process->pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0) return 0;
    if (result < 0) return -1;
    process->exited = 1;
    process->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    if (out_exit_code != NULL) *out_exit_code = process->exit_code;
    return 1;
}
int cdin_process_wait(cdin_process *process, uint32_t timeout_ms, int *out_exit_code)
{
    uint64_t start, duration;
    struct timespec pause = {0, 1000000L};
    if (process == NULL) return -1;
    start = cdin_clock_monotonic_ns();
    duration = (uint64_t)timeout_ms * UINT64_C(1000000);
    for (;;) {
        int result = cdin_process_poll(process, out_exit_code);
        uint64_t now;
        if (result != 0) return result;
        now = cdin_clock_monotonic_ns();
        if (now - start >= duration) return 0;
        (void)nanosleep(&pause, NULL);
    }
}
int cdin_process_terminate(cdin_process *process)
{ return process != NULL && !process->exited && kill(process->pid, SIGTERM) == 0 ? 0 : -1; }
void cdin_process_destroy(cdin_process *process)
{
    if (process == NULL) return;
    if (process->stdout_read >= 0) close(process->stdout_read);
    if (process->stderr_read >= 0) close(process->stderr_read);
    if (!process->exited) { (void)kill(process->pid, SIGTERM); (void)waitpid(process->pid, NULL, 0); }
    free(process);
}
#endif
