# Operating-system services

The Backend provides operating-system functionality through small modules. The goal is to make Runtime code portable without teaching Runtime how Windows, macOS, or Linux system calls differ.

## 1. Paths

Public path input is UTF-8. Path normalization is lexical: it removes redundant separators, `.` segments, and resolvable `..` segments. It does not check whether a path exists and it does not change the process working directory.

The Backend uses `/` as its canonical path separator. The Windows platform layer converts UTF-8 paths to and from UTF-16 when calling wide-character Windows APIs.

Important rules:

- Never call `chdir()` to make a path easier to resolve. A process-wide current directory would affect every subsystem.
- Join paths through the path module rather than concatenating strings manually.
- Treat paths from user input as data, not as shell fragments.
- Lexical normalization is not a security sandbox. A host application must still enforce its own allowed roots and permissions.

## 2. Files

### Bounded reads

`cdin_fs_read_chunks()` reads a file in fixed-size chunks. The caller supplies the maximum total byte count and a callback for each chunk. This avoids allocating a buffer equal to the size of an untrusted file.

The callback returns zero to continue. A non-zero callback return stops the read and reports failure.

### Atomic writes

`cdin_fs_begin_atomic_write()` creates a temporary file next to the destination. `write_chunk()` appends bytes while enforcing a maximum size. `commit_atomic_write()` flushes the file and publishes it with a native atomic replacement operation. `abort_atomic_write()` removes the temporary file.

The overwrite policy is explicit:

- `overwrite = 1`: replace the destination when the platform supports atomic replacement.
- `overwrite = 0`: fail if the destination already exists.

The temporary file is placed beside the destination so the final rename/link operation stays on the same filesystem. If the process is interrupted before commit, the destination remains unchanged; a leftover temporary file may need cleanup by the host.

## 3. Child processes

`cdin_process_spawn()` accepts `argv[]`, where `argv[0]` is the executable and each following entry is one argument. Backend does not concatenate those arguments into a shell command.

On POSIX systems the implementation uses `posix_spawnp`. On Windows it uses `CreateProcessW` with UTF-8 to UTF-16 conversion and Windows command-line argument quoting. Both use the same C API.

The caller can request stdout/stderr capture, set an optional working directory, and choose environment inheritance. Captured streams are non-blocking reads. `wait(timeout_ms)` returns `1` when the process exits, `0` when the timeout expires, and `-1` on error.

The process API intentionally does not accept a shell script as a special case. To run a shell, a caller must explicitly name that shell as the executable and pass its arguments, making the boundary visible.

## 4. Worker jobs

The worker pool has a bounded submission queue and a bounded completion queue. A job has:

- a C-only work callback;
- an optional completion callback;
- optional destructors for the input context and output result;
- a cancellation token.

Worker callbacks must be independent of Lua and mutable Kernel state. They may process their own input and return an owned result. Completion callbacks run only when the owner thread calls `cdin_job_pump()`.

Cancellation is cooperative. A worker must check its cancellation token while it works. Shutdown with abort requested marks queued work as cancelled; it cannot safely interrupt an arbitrary C function that ignores cancellation.

## 5. Clocks and threads

The platform layer provides monotonic nanoseconds for elapsed-time measurements and wall-clock seconds for timestamps. Do not use wall-clock time for timeout measurement because the system clock can move.

Thread, mutex, and condition-variable primitives are abstracted in `platform/thread.*`. Use those wrappers in shared modules instead of adding direct pthread or Win32 threading calls.
