# Frontend Integration Guide — Protocol 0.2.0

This guide describes the Frontend-facing contract implemented by this Backend
source snapshot. The normative schema remains in the CDIN Spec:

- `cdin-editor/spec/docs/architecture/protocol-wire.md`
- `cdin-editor/spec/docs/architecture/ui-model-v1.md`
- `cdin-editor/spec/docs/adr/ADR-0018-frontend-protocol-wire.md`

## 1. Connection lifecycle

A Frontend connection is a protocol client. The in-process C API can create and
negotiate clients; a concrete OS transport must provide the byte stream for an
out-of-process Frontend.

1. Create a client with `cdin_backend_frontend_open()`.
2. Send `hello` with the supported Protocol major/minor range, frontend name
   and capability list. Receive `hello_ack` and retain `session_id`, `client_id`
   and the assigned `driver` role.
3. Send `attach` with the `session_id` and initial dimensions/units.
4. Read the ordered outbound frames. A successful attach queues the latest
   semantic UI snapshot when the Lua Runtime is present.
5. Apply each complete group only when its final `ui.flush` arrives. Send
   `ack` with that flush payload's `sequence` after the group has been applied.
6. On orderly close, send `detach`; then close the transport and C client.

Every stream frame is `uint32_be payload_length` followed by one deterministic
CBOR envelope. Envelope `seq` is the per-connection message sequence. The
`ui.flush.payload.sequence` is a different sequence for flush groups and is the
value acknowledged by the Frontend. Never ACK the envelope sequence.

The message codec is transport-neutral. `cdin_protocol_read_frame()` and
`cdin_protocol_write_frame()` operate on caller-provided partial-I/O callbacks;
they do not open sockets or named pipes.

## 2. Snapshot application semantics

A full Runtime model projection is an ordered group ending in:

```text
ui.flush { scope: "snapshot", revision: N, sequence: F }
```

The Frontend stages all preceding messages in that group. Only when this flush
is received does it atomically replace its semantic UI state with the staged
snapshot. Views and panels absent from the new snapshot are removed. This is
important when a view or panel has been closed since the previous revision.
The Frontend ACKs `F` after applying the staged snapshot.

A response group ends in:

```text
ui.flush { scope: "response", sequence: F }
```

A response flush never clears or replaces the full UI state. `view.lines` and
`error` use response scope. A receiver rejects unknown scopes, a snapshot
without a revision, or a response that includes a revision.

The Backend publishes a complete snapshot as one atomic queue batch. If queue
capacity or the unacknowledged-flush limit prevents publication, no partial
snapshot is queued. The newest Lua model remains cached. Once an older flush is
ACKed, Backend retries only if the cached model revision is newer than the last
snapshot revision queued for that client. A detach clears the client's queued
state; the next attach therefore receives a fresh snapshot.

## 3. UI messages

The Backend maps `cdin.ui-model.v1` into these messages:

| Message | Frontend responsibility |
|---|---|
| `ui.options` | Adopt the theme identifier and semantic options for the revision. |
| `ui.style_table` | Replace the semantic style-role map. |
| `ui.layout` | Adopt the logical split tree and active leaf; calculate local geometry. |
| `ui.statusline` | Replace statusline data. |
| `ui.prompt` | Replace or clear the active prompt. |
| `ui.panel` | Replace the identified panel. Panels absent from a complete snapshot are removed. |
| `view.cursor` | Adopt the byte caret position and logical byte `scroll_anchor` for the specified document version. |
| `view.selection` | Adopt the byte anchor/caret pair for the specified document version. |
| `view.invalidate` | Mark the specified document byte ranges stale and request visible lines. |

The initial snapshot invalidates the full document range `[0, document_length)`
for every visible view. Runtime sends semantic state only. The Frontend owns
pixel geometry, line wrapping, shaping, bidi, scrolling in pixels, rendering,
windowing and toolkit-specific caches.

## 4. Document line windows

Line content is pull-based. After receiving `view.invalidate`, the Frontend asks
for only the visible logical line window:

```text
view.request_lines {
  view_id: "view-7",
  first_line: 120,
  line_count: 80,
  version: 42
}
```

All line numbers are zero-based. `version` must match the current version in the
view snapshot. The Backend obtains a temporary immutable Kernel Snapshot,
checks the requested version, asks the Kernel line-index API for exact byte
ranges, reads those bytes and releases the Snapshot before returning to Lua.
There is no Runtime-side shadow document buffer.

The response is:

```text
view.lines {
  view_id: "view-7",
  version: 42,
  first_line: 120,
  total_lines: 250,
  lines: [
    { bytes: h'6669727374206c696e650d0a' },
    { bytes: h'7365636f6e64206c696e650a' }
  ]
}
```

Each `bytes` value is a CBOR byte string, not CBOR text. It may contain NUL,
invalid UTF-8 and original CRLF/LF/CR terminators. The final empty logical line
is represented by an empty byte string when the document ends in a line
terminator. The Frontend must not normalize these bytes on receipt.

The Backend returns complete lines only. A response may contain fewer lines
than requested when the bounded byte budget is reached; request the remainder
starting at `first_line + returned_line_count`. A line larger than the allowed
single response budget returns a structured limit error rather than a partial
line. If the Kernel version changed, the request fails with `STALE_VERSION`;
use the newest model revision/invalidation and retry.

## 5. Inbound events and authority

All non-handshake inbound messages are schema-validated, sequence-checked,
permission-checked and copied into a bounded per-client queue before dispatch.
The Backend drains that queue only from `cdin_backend_pump()` on the Lua owner
thread. Worker threads never enter Lua.

| Event | Runtime behavior | Role |
|---|---|---|
| `input.key` | `Runtime:handle_key()`; Runtime resolves keymaps/commands. | Driver |
| `input.text` | Insert validated UTF-8 text bytes at the focused selection. | Driver |
| `input.paste` | Insert opaque paste bytes without UTF-8 normalization. | Driver |
| `input.mouse` | Forward semantic mouse event; optionally focus the target view. Frontend does hit-testing and sends `view.set_position` for exact byte offsets. | Driver |
| `window.resize` | Emit a Runtime resize event; Frontend owns actual layout geometry. | Driver |
| `focus` | Emit a Runtime focus event. | Driver |
| `view.set_position` | Validate document version and byte offsets; update selection/focus. | Driver |
| `request.command` | Call `Runtime:dispatch()` and the core command registry. | Driver |
| `view.request_lines` | Read a version-pinned Kernel line window and return `view.lines`. | Driver or Observer |

The Driver is the only client allowed to mutate editor state. Observers can
receive snapshots and request read-only line windows. When the Driver detaches,
the oldest still-attached client is promoted.

`input.text.text` is CBOR text and must be valid UTF-8. `input.paste.bytes` is a
CBOR byte string and is binary-safe. `view.set_position.anchor` and `caret` are
zero-based byte offsets, not grapheme columns or pixels.

## 6. Runtime and Host API prerequisites

Backend reports Host API `0.3.0`, Kernel ABI `1.1.x` and Frontend Protocol
`0.2.0`. The aligned Runtime provides the matching descriptor, requires the
`document_lines` service, and implements `DocumentClient:read_lines()`,
`Runtime:read_view_lines()` and `Runtime:handle_frontend_event()`.
The Backend's `runtime-integration.yml` checks out Runtime `main`, runs its
unit/integration/boundary suite, then builds the production Lua 5.4 + Kernel
bridge and executes the live Protocol smoke scenario. Merge the Runtime update
before merging Backend changes that consume it.

## 7. Current integration boundary

Implemented in this Backend snapshot:

- deterministic CBOR, frame encode/decode and complete message payload schemas;
- in-process client handshake, attach/detach, one Driver and Observer roles;
- bounded queues, flush groups, ACK validation and snapshot replay after ACK;
- Lua UI Model to typed Protocol projection, with an atomic snapshot group;
- inbound event queue to Runtime dispatch on the Lua owner thread;
- `view.request_lines` through a temporary Kernel Snapshot and `view.lines` byte strings;
- Headless Frontend and protocol replay/fuzz test targets.

Still required before a production Frontend can connect as an independent
process:

- obtain a green current-commit run of the Lua 5.4 + installed Kernel + Runtime
  integration workflow on the updated repositories;
- implement a concrete Unix Domain Socket adapter and Windows Named Pipe
  adapter (or formally constrain the first release to in-process embedding);
- run live two-client Frontend ↔ Backend ↔ Runtime ↔ Kernel tests and the full
  Linux/macOS/Windows matrix;
- complete transport authentication/lifecycle/timeouts and sustained fuzzing.

The current Headless Frontend is a conformance harness, not a GUI toolkit or
platform transport.
