# Reusing the portable services library

This guide shows how to use the Backend's operating-system helpers from a
completely separate C project. The services library does not start CDIN, does
not create a Kernel instance, and does not depend on Lua or a Frontend.

## What is included

The standalone archive is named `cdin_backend_services`.

| Public header | What it provides | Typical use |
| --- | --- | --- |
| `cdin/backend/path.h` | UTF-8 path normalization, joining, absolute and relative paths | Workspace and cache paths |
| `cdin/backend/filesystem.h` | Bounded chunk reads, directory listing, metadata and atomic writes | Safe document save and backup |
| `cdin/backend/process.h` | Process spawn, output capture, timeout and termination | Git, formatter or language-server process management |
| `cdin/backend/jobs.h` | Bounded worker queue, cancellation and owner-thread callbacks | Indexing, parsing and background I/O |
| `cdin/backend/clock.h` | Monotonic and wall clocks | Timeouts, duration and timestamps |
| `cdin/backend/services.h` | Convenience umbrella header for all services | Small standalone consumers |

The public API is deliberately narrow. It exposes C data and opaque handles,
not operating-system structures. This makes callers independent from whether
the target uses POSIX APIs or Windows APIs.

## Build and install

Build only the services archive. No Kernel package is needed for this target:

```sh
make services
```

For a reusable installation, use CMake:

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --target cdin_backend_services
cmake --install build
```

CMake installs the archive, public headers, and `cdin-backend-services.pc`.
A consuming project can discover the dependency through `pkg-config`:

```sh
pkg-config --cflags --libs --static cdin-backend-services
```

On platforms without `pkg-config`, link the installed archive and the platform
thread library selected by the consumer's build system. CMake consumers can
link the exported build target as `cdin::backend_services` when using this
repository as a subproject.

## Real-world example: atomic backup

The [atomic-backup example](../examples/atomic-backup/main.c) copies a workspace
file without replacing the previous backup until the new copy is complete.
It also handles several failure cases:

- input and output resolving to the same path;
- missing input or a directory passed instead of a file;
- a file larger than the configured safety limit;
- a read/write failure in the middle of streaming;
- a source file changing while it is being copied;
- a failure while publishing the temporary file.

Build the example with Make after building the services archive:

```sh
make example-atomic-backup
./build/examples/atomic-backup ./notes.cdin ./notes.cdin.backup
```

Or compile it in another project using `pkg-config`:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  $(pkg-config --cflags cdin-backend-services) \
  examples/atomic-backup/main.c \
  $(pkg-config --libs --static cdin-backend-services) \
  -o atomic-backup
```

## Ownership and threading rules

Allocated paths are released with `free()`. Filesystem callback byte views and
directory entry names are borrowed only for the duration of the callback.
Atomic writers are consumed by `commit`; call `abort` when a write does not
reach commit.

Job workers may use plain C data and the portable services, but they must not
call UI code or enter Lua. Completion callbacks are delivered only when the
thread that created the job pool calls `cdin_backend_job_pool_pump()` or
performs graceful shutdown. A job token is borrowed and becomes invalid after
its completion has been delivered or discarded.

## Error handling

The OS service functions return zero on success and a non-zero result on
failure. Do not infer that a failed write published a partial destination:
atomic writer failures leave the previous destination unchanged. Process wait
returns `0` on timeout, `1` on process exit, and `-1` on an API or OS error.
The exact native error is intentionally not exposed as a stable ABI value in
this release; applications should add their own contextual message at the call
site.
