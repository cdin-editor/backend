# Runtime Host API 1.1.0

Runtime does not call the Kernel directly. It calls a table named `host`, which
the Backend creates. This page documents the Host services implemented by this
Backend source snapshot.

Backend reports Host API `1.1.0`, Kernel ABI `1.1.x` and Frontend Protocol
`1.0.0`. Host API `1.x` is stable and uses same-major, minimum-minor compatibility;
additive minor versions are accepted. Frontend Protocol `1.x` is a separate
stable contract and uses its own same-major, minimum-minor compatibility rule. The paired Runtime `1.0.0-rc.1` source in this delivery advertises
Protocol `1.0.0`; never disable or weaken either version guard to hide a mismatch.

## 1. Host descriptor

Host API `1.1.0` adds an optional document line-lookup method without changing existing Host API 1.0.0 behavior. Host API `1.0.0` freezes the Lua method shapes, ownership/lifetime rules,
thread-affinity requirements, required capabilities and stable error identities
documented here and in the paired Runtime contract. Compatible `1.x` minor
versions may add optional methods, fields and capabilities only. Existing
method meanings and required behavior cannot change within major version 1.
Backend is the authoritative descriptor provider; Runtime validates it at
startup. Kernel ABI and Frontend Protocol remain independent version axes.

`host:info()` reports the ABI/version actually detected from the Kernel and the
capabilities exposed by Backend. In addition to the established document,
notification, snapshot, transaction, batch, search, UI and logging features,
Host API `1.0.0` reports:

- `document_lines = true`: bounded version-pinned byte line-window reads;
- `document_line_lookup = true`: version-pinned byte-offset to logical-line lookup via the Kernel line index; optional for older 1.x Hosts.
- `frontend_events = true|false`: true only when the in-process Protocol server
  is enabled and the Lua event dispatcher is compiled in.

Runtime requires `document_lines`. `frontend_events` is a boolean optional
capability; a Runtime can still run without Frontend clients when it is false.

The logging descriptor identifies the single Kernel-owned logger:

```lua
logging = {
  provider = "cdin-kernel",
  levels = { "error", "warn", "info", "debug", "trace", "fatal" },
  max_message_bytes = 16384,
}
```

`logging.max_message_bytes` is the Kernel logger record limit. It is independent
of `max_message_bytes`, which limits each Frontend Protocol payload.

## 2. Document service

`host.documents` provides opaque Kernel-backed document handles. The relevant
methods are:

| Method | Purpose |
|---|---|
| `create(initial_bytes, metadata?)` / `close(handle)` | Create or close a Kernel document. |
| `read(handle, request)` | Read a bounded byte range; `start=0,length=0,expected_version=nil` refreshes latest metadata only. |
| `read_lines(handle, request)` | Read complete logical line byte strings from one immutable Kernel Snapshot. |
| `line_from_offset(handle, { offset, expected_version })` | Resolve a document byte offset to a zero-based logical line/column against one immutable snapshot; optional in Host API 1.1.0. |
| `replace(handle, request)` / `replace_batch(handle, request)` | Apply one or several half-open byte-range edits. |
| `undo(handle, request)` / `redo(handle, request)` | Move through Kernel-owned history. |
| `acquire_snapshot(handle)` / `read_snapshot(handle, request)` / `release_snapshot(handle)` | Explicit version-pinned byte reads. |
| `begin_transaction(handle, request)` / `transaction_replace()` / `transaction_read()` | Use a Kernel-owned tentative version. |
| `commit_transaction()` / `abort_transaction()` | Commit or discard tentative changes. |
| `mark_clean()` / `mark_clean_if_version()` / `is_clean()` | Maintain saved-state metadata. |
| `subscribe()` / `unsubscribe()` / `drain()` | Consume bounded Kernel document notifications. |
| `search(handle, request)` | Literal or byte search; regex returns `UNSUPPORTED`. |

### `line_from_offset()` contract

```lua
host.documents:line_from_offset(handle, {
  offset = 8192,           -- zero-based byte offset; EOF is valid
  expected_version = 42,  -- mandatory
})
-- -> { version = 42, line = 120, column = 5 }
```

The Backend acquires an immutable Kernel Snapshot, checks the mandatory
`expected_version`, and calls the public
`cdin_text_line_from_offset()` API on that Snapshot. It releases the Snapshot
before returning. The returned `version`, `line` and `column` therefore
describe one exact document state. A stale version returns `STALE_VERSION`
with expected/current details; an offset past EOF returns `OUT_OF_RANGE`
with requested/document-length details. The Kernel owns LF, CRLF and lone-CR
line semantics; Backend does not rescan or reinterpret document bytes.

This method is optional under Host API 1.x. A Host advertises
`features.document_line_lookup = true` only when it implements the method.
Runtime 1.0 remains compatible with Host 1.1 and can ignore the additional
feature/method. Frontend Protocol is unchanged; this does not itself publish a
line anchor or alter `scroll_anchor`.

### `read_lines()` contract

```lua
host.documents:read_lines(handle, {
  first_line = 120,       -- zero-based logical line index
  line_count = 80,        -- 1..4096
  expected_version = 42,  -- mandatory for a pinned Frontend request
  max_bytes = 65536,      -- optional, bounded by the negotiated frame size
})
-- -> { version, first_line, total_lines, lines = { "raw bytes", ... } }
```

The Backend acquires a temporary `cdin_snapshot`, calls the Kernel's public
`cdin_text_line_count()`, `cdin_text_line_start()` and `cdin_text_read()` APIs,
then releases the snapshot before returning. A concurrent edit cannot mix line
offsets and bytes from different versions. A stale expected version returns
`STALE_VERSION`; a line window contains complete lines only. A bounded response
may contain fewer than the requested number of lines, allowing the Frontend to
continue at `first_line + #lines`. `total_lines` is captured from the same
Kernel snapshot and is suitable for scrollbar/viewport bounds only while its
`version` remains current. If the first line alone exceeds the byte
budget, the call returns `LIMIT_EXCEEDED` with `required` details.

Each returned Lua string is a transient byte string, not retained on the
Runtime document client. Line terminators are included exactly as stored by the
Kernel. NUL and invalid UTF-8 are preserved. A final empty logical line is
returned when the document ends with a line terminator.

## 3. UI service and Frontend projection

`host.ui:publish(model_snapshot)` accepts a semantic `cdin.ui-model.v1` table.
It is not forwarded as Lua data. Backend retains the newest semantic snapshot
in the Lua registry and projects it to deterministic-CBOR messages for every
attached Frontend:

1. `ui.options` and `ui.style_table`;
2. `ui.layout`, `ui.statusline` and `ui.prompt`;
3. `view.cursor` (including the logical byte `scroll_anchor`), `view.selection`
   and full-document `view.invalidate` for each visible view;
4. one `ui.panel` per panel in the current snapshot;
5. a final `ui.flush` with `scope="snapshot"`, the exact model `revision` and a
   per-client flush-group `sequence`.

The entire model projection is validated, bounded and queued atomically. The
combined projection stays within the configured per-message budget, leaving
space for its final flush marker. A failed or backpressured publication does
not roll back Runtime state and never exposes a partial UI snapshot. Backend
retains the newest snapshot and retries a lagging attached client after an ACK
frees queue capacity; a detached client receives a fresh full snapshot on its
next attach.

`view.request_lines` is answered as a `view.lines` response group with
`scope="response"`. A response flush does not replace the Frontend's whole UI
state. See [Frontend Integration Guide](frontend-integration.md) for the exact
wire shapes and application sequence.

## 4. Inbound event dispatch

`cdin_backend_frontend_receive()` validates a complete frame, its schema,
sequence and client role, then copies non-handshake messages into a bounded
inbound queue. `cdin_backend_pump()` calls the Lua Host pump on the Backend
owner thread. The event dispatcher decodes payloads to Lua values and invokes
`Runtime:handle_frontend_event()`; `view.request_lines` has a dedicated
version-pinned path to `Runtime:read_view_lines()`.

The dispatcher covers key, text, paste, mouse, resize, focus, view-position and
command events. It never calls Lua from a worker thread. Driver-only mutation
messages are rejected from Observer clients before queuing; line-window reads
are available to either attached role.

## 5. Optional filesystem read service

When the Runtime bridge is compiled, `host.fs:read_file(path, options?)` reads
exact file bytes through Backend-owned filesystem services and returns one
Lua byte string. It preserves NUL, invalid UTF-8, BOM and line terminators.
The optional `options.max_bytes` must be a positive integer no greater than
Backend's configured `max_document_bytes`; the default is that configured
document limit. A size violation never returns a partial byte string.

This method is additive and optional under Host API 1.x. Runtime startup does
not require it; the built-in `document.open` command checks for the method and
returns `UNSUPPORTED` when absent. Read failures retain stable codes:
`NOT_FOUND` for missing paths, `LIMIT_EXCEEDED` for the bound, `NO_MEMORY`
for bounded-buffer allocation failures, and `IO_ERROR` for other filesystem
failures. The implementation uses `cdin_fs_read_chunks`; it does not expose
native file handles or move file I/O into Runtime.

## 6. Errors and logging

A failed Host operation throws a Lua error table with a stable `code` and
machine-readable details. Kernel identities such as `STALE_VERSION`,
`OUT_OF_RANGE`, `LIMIT_EXCEEDED`, `NO_MEMORY` and `UNSUPPORTED` are preserved.

`host:log(level, message, fields?)` forwards to `cdin_kernel_log`. Backend does
not create a second logger, bind `log.c`, or write Runtime records to stdout.
Logging failures never change the operation being logged.

## 7. Ownership and threading

All Host calls that touch Kernel or Lua state run on the Backend owner thread.
The Backend owns opaque Lua userdata handles; Runtime never receives native
pointers or Kernel handles. Worker threads may return plain data through the
Backend completion queue, but must never enter the Lua state.

## 8. Current verification boundary

Protocol-only and service tests pass in this workspace. The actual Runtime
repository remains a separate, read-only inspected source tree; the companion
patch must be applied and reviewed there. A production build with real Lua 5.4
development headers and the installed Kernel package, plus live Runtime/Kernel
integration tests, has not yet been executed. The OS transport adapters are
also not part of this source snapshot.
