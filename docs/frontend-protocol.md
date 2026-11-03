# Frontend Protocol 1.0.0 — Backend implementation status

The normative contract is owned by the [CDIN Spec wire contract](https://github.com/cdin-editor/spec/blob/main/docs/architecture/protocol-wire.md), [UI Model v1](https://github.com/cdin-editor/spec/blob/main/docs/architecture/ui-model-v1.md) and [ADR-0018](https://github.com/cdin-editor/spec/blob/main/docs/adr/ADR-0018-frontend-protocol-wire.md). This page records the implemented Backend source and its remaining integration boundaries.

## 1. Implemented layers

| Layer | Source | Implemented behavior | Not implied |
|---|---|---|---|
| Wire codec | `include/cdin/protocol.h`, `src/protocol/codec.c` | Deterministic-CBOR frame encode/decode, 4-byte big-endian framing, Protocol 1.0.0 payload schemas, canonical map validation, bounded depth/frame size, partial read/write callbacks | No socket, named-pipe, authentication or network listener |
| Per-client server | `src/protocol/server.c` | Handshake, one Driver lease, Observer permissions, oldest-client promotion, bounded queues, payload copies, coalescing, atomic snapshot/response batches, flush scopes, ACK validation and per-client snapshot revision tracking | No concurrent writer or last-writer-wins semantics |
| Backend C facade | `include/cdin/backend_protocol.h`, `src/protocol/backend_protocol.c` | Typed client API, receive validation/queue, output frame encoding, attach snapshot replay and ACK-triggered retry of a newer missed snapshot | Requires caller to supply transport I/O |
| Lua UI projection | `src/runtime/lua_host_protocol.c` | Validates `cdin.ui-model.v1` fields, deterministically serializes semantic maps, converts split ratios to integer basis points, projects options/styles/layout/status/prompt/views/panels and atomically appends `ui.flush` | Runtime source patch must be applied to the separate Runtime repository |
| Inbound Runtime dispatch | `src/runtime/lua_host_protocol.c`, `src/runtime/lua_host.c` | Drains validated events on Lua owner thread; dispatches `handle_frontend_event`; handles `view.request_lines` and response generation | Does not provide platform transport |
| Kernel line window | `src/kernel/adapter_documents.c`, `src/runtime/lua_host_documents.c` | Temporary immutable Snapshot, version check, Kernel line-index APIs, complete binary-safe line byte strings and bounded payload budget | No persistent/shadow text buffer |
| Headless client | `tests/headless/headless_frontend.c` | Display-free encode/decode, binary input, event record and deterministic replay | Not a GUI toolkit or OS transport |

The standalone `cdin_protocol` target depends only on the C standard library and can be built with `CDIN_BUILD_BACKEND=OFF`, without Kernel or Lua development packages.

## 2. Frame and sequence contract

Each stream frame is a four-byte unsigned big-endian body length followed by one deterministic-CBOR envelope. The body cap defaults to 1 MiB. The envelope fields are `v`, `ns`, `seq`, `type` and `payload`. `seq` counts every message per connection. `ui.flush.payload.sequence` is a separate flush-group ID and is the value carried by `ack.payload.sequence`.

`ui.flush.payload.scope` is `snapshot` or `response`. A snapshot flush must include `revision`; a response flush must not. The Frontend stages a complete snapshot and replaces its semantic UI state only after the snapshot flush. Response groups never clear UI state. `view.cursor.scroll_anchor` is a zero-based document byte offset; `view.lines.total_lines` is the logical line count observed in the same Kernel Snapshot as the returned lines.

Protocol 1.0.0 does not encode floating-point values. Pane split ratios are converted to `ratio_bp` integer basis points (`1000..8999`). All opaque content, including `input.paste` and `view.lines[].bytes`, remains CBOR byte strings.

## 3. Implemented publication and inbound paths

`lua_ui_publish()` retains the latest semantic snapshot, projects it for each attached Client, and calls the atomic `cdin_protocol_publish_snapshot()` operation. Every message is schema-validated before the whole message group plus final flush marker is committed to that Client's bounded queue. The combined snapshot has a total byte budget as well as per-message limits. Queue pressure leaves Runtime authoritative state intact and marks the Client behind; when an ACK frees capacity, Backend retries only if the cached revision is newer than the last snapshot revision queued for that Client. Detach resets this state; attach sends a full snapshot.

Inbound `cdin_backend_frontend_receive()` performs frame/schema/direction/sequence/permission checks and copies valid events to a bounded queue. `cdin_backend_pump()` runs the Lua Host pump on the owner thread. The dispatcher calls `Runtime:handle_frontend_event()` for key, text, paste, mouse, resize, focus, view-position and command events. `view.request_lines` calls `Runtime:read_view_lines()` and returns a response-scoped `view.lines` batch.

## 4. Document line-window semantics

`view.request_lines` includes `view_id`, zero-based `first_line`, `line_count` (`1..4096`) and a required document `version`. Backend reads through a temporary Kernel Snapshot and checks the version before calculating line offsets or copying bytes. The `view.lines.lines` array contains maps with one `bytes` CBOR byte string each; `total_lines` gives the logical line count at the same version. Original EOL bytes, NUL and invalid UTF-8 are preserved. Only complete lines are returned; a too-large first line produces a structured limit error, and a partial line is never sent. A stale version produces `STALE_VERSION` and requires a fresh model/line request.

## 5. Remaining integration gates

The Backend Protocol/Runtime bridge source is implemented in this local source snapshot, but the following must still happen before production Frontend use:

1. Apply and review `integration/runtime-patch/apply_runtime_integration.py` in a `cdin-editor/runtime` checkout. That patch aligns Runtime Host API `1.0.0` and Protocol `1.0.0`, adds `document_lines`, `read_lines()`, UI-model style/version fields, event dispatch and FakeHost/integration tests. The public Runtime repository was inspected read-only and was not changed or pushed.
2. Build against real PUC Lua 5.4 development files and the installed `cdin-kernel` package, then run the real Runtime + Kernel integration suite. The required development dependencies are unavailable in this workspace.
3. Implement concrete Unix Domain Socket and Windows Named Pipe transports if the Frontend is a separate process. The codec's partial-I/O callbacks do not themselves open a transport.
4. Run the current-commit Linux/macOS/Windows matrix and live two-client GUI/headless conformance tests.

See [Frontend Integration Guide](frontend-integration.md), [Host API](host-api.md), [Spec traceability](spec-traceability.md) and [1.0.0 readiness](1.0.0-readiness.md).

## 6. Message catalogue

The public enum and validator cover: `hello`, `hello_ack`, `attach`, `detach`, `detach_ack`, `ack`, `input.key`, `input.text`, `input.mouse`, `input.paste`, `window.resize`, `focus`, `view.request_lines`, `view.set_position`, `request.command`, `response.clipboard`, `response.dialog`, `ui.options`, `ui.style_table`, `ui.layout`, `view.lines`, `view.invalidate`, `view.cursor`, `view.selection`, `ui.statusline`, `ui.prompt`, `ui.panel`, `ui.notify`, `request.clipboard_get`, `request.clipboard_set`, `request.dialog`, `error` and `ui.flush`.
