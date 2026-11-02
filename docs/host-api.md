# Runtime Host API

Runtime does not call the Kernel directly. It calls a table named `host`, which the Backend creates. This document describes the services supplied by that table.

The contract version is Host API `0.2.0`. It is provisional and must be version-checked by Runtime.

## 1. Host information

`host:info()` returns version numbers and feature descriptions. The Backend queries the Kernel first, then reports the versions that it actually detected. It must not claim regex search when Kernel ABI 1.1.0 does not provide it.

The logging descriptor identifies the shared logger:

```lua
logging = {
  provider = "cdin-kernel",
  levels = { "error", "warn", "info", "debug", "trace", "fatal" },
  max_message_bytes = 16384, -- Kernel logger record limit
}
```

The logger limit is configured with `max_log_message_bytes` (default 16 KiB) and is independent from `max_message_bytes`, which limits Frontend Protocol payloads. The logger limit is passed to the Kernel logger and can be set from 1 byte through 1 MiB. In `host.info()`, `logging.max_message_bytes` reports the logger limit, not the Protocol payload limit.

## 2. Document service

`host.documents` contains these methods:

| Method | Purpose |
|---|---|
| `create(initial_bytes, metadata?)` | Ask Kernel to create a document. Returns an opaque handle, version, and length. |
| `close(handle)` | Close the document after its views have been removed. |
| `read(handle, request)` | Read a bounded byte range, optionally requiring a particular version. |
| `replace(handle, request)` | Apply one half-open byte-range edit. |
| `replace_batch(handle, request)` | Apply a list of non-overlapping edits as one history item. |
| `undo(handle, request)` / `redo(handle, request)` | Move backward or forward in Kernel history. |
| `acquire_snapshot(handle)` | Pin a version so a save or background read sees stable bytes. |
| `read_snapshot(handle, request)` / `release_snapshot(handle)` | Read and release that pinned version. |
| `begin_transaction(handle, request)` | Start a Kernel-owned tentative edit. |
| `transaction_replace(handle, edit)` / `transaction_read(handle, request)` | Edit or read the tentative state. |
| `commit_transaction(handle, request)` / `abort_transaction(handle)` | Finish or discard a transaction. |
| `mark_clean(handle)` / `mark_clean_if_version(handle, version)` | Mark a document as saved. The conditional form compares and marks on the owner thread. |
| `is_clean(handle)` | Ask whether the current document version is marked clean. |
| `subscribe(handle)` / `unsubscribe(handle)` | Subscribe to Kernel document notifications. |
| `drain(handle, max_events)` | Read a bounded batch of notifications. |
| `search(handle, request)` | Search with literal or byte mode. Regex returns `UNSUPPORTED`. |

All document handles and snapshot/transaction/subscription handles are opaque Lua userdata. Runtime never sees a raw Kernel pointer or handle.

### Edit ranges

All positions are zero-based byte offsets. A replacement covers `[start, finish)`: `start` is included and `finish` is excluded. Batch edits must be ordered and must not overlap in the same pre-edit version.

### Reads and snapshots

A regular read returns transient bytes for the requested range. A snapshot is a pinned version. Runtime should release every snapshot after it finishes using it, including when a save fails.

### Notifications

Notifications preserve their version range and edit data. If Kernel reports truncated edits or a version gap, Runtime should resynchronize from the document rather than apply an incomplete edit sequence.

## 3. Error shape

A failed Host call throws a Lua table similar to this:

```lua
{
  domain = "cdin.runtime",
  code = "STALE_VERSION",
  message = "documents.replace failed: STALE_VERSION",
  details = { expected = 4, current = 5 },
}
```

The Backend preserves the Kernel status identity and available details. For example, an out-of-range request can include `requested` and `length`; a too-small output buffer can include `required`; resource limits can include `limit_id`.

| Backend/Kernel result | Runtime code |
|---|---|
| `INVALID_ARGUMENT` | `INVALID_ARGUMENT` |
| `INVALID_HANDLE` / `NOT_FOUND` | `INVALID_HANDLE` |
| `STALE_VERSION` | `STALE_VERSION` |
| `OUT_OF_RANGE` | `OUT_OF_RANGE` |
| `INVALID_POSITION` | `INVALID_POSITION` |
| `INVALID_STATE` | `INVALID_STATE` |
| `BUSY` / `QUEUE_FULL` | `BUSY` |
| `LIMIT_EXCEEDED` | `LIMIT_EXCEEDED` |
| `NO_MEMORY` | `NO_MEMORY` |
| `PATTERN_INVALID` | `PATTERN_INVALID` |
| `CANCELLED` | `CANCELLED` |
| `SHUTDOWN` | `SHUTDOWN` |
| `UNSUPPORTED` | `UNSUPPORTED` |
| `BUFFER_TOO_SMALL` | `BUFFER_TOO_SMALL` |
| `VERSION_MISMATCH` | `VERSION_MISMATCH` |
| Unclassified failure | `INTERNAL` |

## 4. UI service

`host.ui:publish(model_snapshot)` accepts the Runtime's semantic UI model. A semantic model describes what should appear — views, logical positions, spans, and status content — rather than pixels or terminal cells.

In this first implementation, the most recently published Lua model is retained in the Backend's in-process Lua registry. It is not yet encoded or delivered to external Frontends. That delivery depends on the still-open serialized Protocol decision.

## 5. Logging service

`host:log(level, message, fields?)` forwards messages to the single Kernel-owned logger through `cdin_kernel_log`. Backend does not create a second logger and does not bind `m-mdy-m/log.c` directly.

The valid levels are `error`, `warn`, `info`, `debug`, `trace`, and `fatal`. Their Kernel numeric values are not in severity order, so threshold checks must use severity rules rather than comparing the enum integers.

Runtime log messages are bounded by `info.logging.max_message_bytes`. The Backend's default is 16 KiB and is controlled by `max_log_message_bytes`. The optional `fields` table supports a limited number of short scalar values; because the Kernel logging ABI has no structured-fields parameter, Backend appends supported metadata to the message representation.

A message filtered by the configured log threshold is not an operation failure. The logging call returns `false` when the record is not accepted.

## 6. Optional atomic file service

The Backend provides `host.fs` methods for atomic save:

- `begin_atomic_write(path, options)` creates a temporary writer;
- `write_chunk(writer, bytes)` appends a bounded chunk;
- `commit_atomic_write(writer)` atomically publishes the completed file;
- `abort_atomic_write(writer)` discards the temporary file.

The default overwrite behaviour is enabled. Passing `{ overwrite = false }` prevents replacing an existing destination. Runtime should snapshot a document, stream its bytes in chunks, commit the file, and then call `mark_clean_if_version` so edits made during the save are not accidentally marked clean.

## 7. System services

`host.system.time` supplies monotonic nanoseconds and wall-clock seconds. `host.system.process.spawn` starts a process from an argv array. It never joins user-provided arguments into a shell command.

The process handle can be polled, waited on with a timeout, read from stdout/stderr, terminated, and closed. These operations are described in [Operating-system services](os-services.md).

## 8. Threading rule

Every Runtime Host service that touches Lua or Kernel state is called on the Backend owner thread. Worker threads must return plain data to the owner thread; they must not invoke the Host API themselves.
