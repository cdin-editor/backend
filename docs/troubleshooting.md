# Troubleshooting

Use this page when the Backend does not start or a Host call returns an error.

## Kernel and ABI errors

### `VERSION_MISMATCH`

Backend requires Kernel ABI major version `1` and minor version `1` or newer. Check which Kernel headers were used to compile Backend and which Kernel library the executable loads. They must come from the same compatible installation.

### `UNSUPPORTED` during Backend creation

Backend requires Kernel batch edits, transactions, snapshots, notifications, literal search, and byte search. A Kernel library without one of these features cannot satisfy Runtime Host API `1.0.0` in this Backend snapshot (the paired Runtime release reports Host API `1.0.0` and Protocol `1.0.0`).

### `INVALID_HANDLE`

A handle may have been closed, released, or created by another Backend instance. Handles are opaque and scoped to the Backend that created them. Do not serialize or reuse Lua userdata values.

## Runtime and Lua errors

### `UNSUPPORTED` during Backend start

The application enabled Lua Runtime, but the Backend was built without the Lua 5.4 development package. Rebuild with `WITH_LUA=1` or disable Lua for a headless test.

### `require` cannot find `cdin.runtime`

`runtime_root` must point to the directory that contains the Lua module tree. If the module is `cdin/runtime/init.lua`, `runtime_root` should point to the parent directory of `cdin/`.

### Runtime reports `HOST_API_MISMATCH`

Check Host API major/minor version, Kernel ABI version, Runtime extension contract version, Frontend Protocol version, feature flags, and logger provider. The logger provider must be `cdin-kernel`.

## Files and processes

### Atomic write commit fails

Check that the destination directory exists, that the process can create a temporary file beside the destination, and that the target is not locked by another program. With `overwrite = false`, an existing destination is an expected failure.

### Process output appears empty

Output capture is opt-in. Set `capture_stdout = true` or `capture_stderr = true` when spawning the process, then call the corresponding read method. Reads are non-blocking and may return an empty string when no bytes are ready yet.

### Process working directory is rejected

Working-directory support relies on native spawn APIs. If the OS does not provide the required operation, Backend returns an I/O error rather than changing the parent process's current directory.

## Frontend queue results

### `QUEUE_FULL`

The client's bounded outgoing queue has reached capacity. A caller can acknowledge earlier flushes, increase the configured capacity if memory limits allow, or use a non-zero coalescing key for replaceable state updates.

### `BUSY` from flush

The client already has the maximum number of unacknowledged flushes. It must acknowledge an earlier flush before another frame can be created.

## Logging

Backend uses the Kernel's logger. Its default destination is stderr and its default
minimum level is INFO. All native Backend records include a subsystem prefix and
source location, for example:

```text
[BACKEND][CORE] backend.c:... cdin_backend_start(): ...
[BACKEND][PROTOCOL] server.c:... cdin_protocol_flush(): event=flush_backpressure ...
[BACKEND][JOBS] backend.c:... cdin_backend_shutdown(): ...
```

Set `config.log_level = CDIN_BACKEND_LOG_TRACE` to include detailed transitions
and resource lifecycle records. Use `DEBUG` for decisions and queue state,
`INFO` for major lifecycle events, `WARN` for stale requests/backpressure,
and `ERROR` for failed operations. An optional `config.log_callback` receives
the shared Kernel stream, including Kernel records as well as Backend records.
The callback must be thread-safe and must not call back into Backend APIs.

When an operation fails, begin with the `[BACKEND][SUBSYSTEM]` tag, then inspect
the file/function, `event`, `status`, and `reason` fields. Protocol diagnostics
include a client id when the event belongs to one; lifecycle diagnostics include
the session id and startup stage when applicable.
