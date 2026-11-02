# Known limitations in 0.1.0

This page distinguishes implemented behavior from design work the CDIN
Specification still marks as `OPEN` or `PROPOSED`. A missing feature is not
silently treated as complete.

## 1. Open Spec decisions

| Decision | Exact question | Effect on this implementation |
| --- | --- | --- |
| [OD-22](https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-22) | Encoding of the serialized protocol | No JSON, CBOR, MessagePack or custom wire codec is selected or implemented. |
| [OD-23](https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-23) | Protocol client library and where it lives | The current C API remains in the Backend public headers; no separate client package is claimed. |
| [OD-24](https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-24) | Several frontends attached: who may mutate | This release uses one driver and read-only observers as a provisional rule. |
| [OD-32](https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-32) | Lua-to-native bridge behind the Runtime Host API | The current adapter uses the PUC Lua 5.4 C API, but the Spec has not selected it over LuaJIT FFI or MetaCall. |

OD-32 is the exact Spec decision titled **“Lua-to-native bridge behind the
Runtime Host API.”** It states that the stable `cdin-kernel` boundary is a C ABI,
but Lua Runtime and `cdin-x` extensions must not call private Kernel functions
directly. Backend owns the Kernel adapter and exposes a versioned Runtime Host
API. OD-32 asks the project to compare three bridge choices:

1. Native binding functions implemented with the Lua C API, suitable for
   standard PUC Lua 5.4.
2. LuaJIT FFI, but only if LuaJIT becomes an explicit Runtime target.
3. MetaCall or another multi-language loader, if loading and invoking several
   languages becomes a product requirement.

The Spec says to compare platform availability, deployment footprint,
debugging, error mapping, memory ownership, reentrancy, startup/per-call
overhead, and a minimal end-to-end prototype. It also requires confirmation of
the Runtime target (PUC Lua versus LuaJIT), a tested C call round-trip, and that
plugins continue to see only the extension contract and Runtime Host API. **No
option is selected.** Therefore the PUC Lua 5.4 C API in
`src/runtime/lua_host.c` is only a replaceable implementation candidate; it is
not a resolved architecture decision and does not become a Kernel dependency.
MetaCall is explicitly not required by `cdin-kernel`.

## 2. Frontend delivery

The in-process API currently supplies handshake, capability filtering,
attach/detach, message queueing, flush sequences, acknowledgements, queue
coalescing and backpressure. It does not serialize messages or provide a socket,
named-pipe or terminal-stream transport.

`host.ui:publish(model)` retains the latest semantic Lua model in the Lua
registry. It does not yet translate that model into `ui.options`, `view.lines`,
`ui.flush` or other protocol messages. A model schema and delivery adapter are
required after the transport and client-library decisions are accepted.

No optional capability bits are accepted until the Spec assigns stable numeric
values to them. The capability names in sketches are not treated as an
implemented wire contract.

## 3. Runtime integration prerequisites

The Lua bridge requires PUC Lua 5.4 development headers and a matching library.
Without those development files, the Backend compiles an explicit `UNSUPPORTED`
stub. The default library can still be used for headless Kernel and OS-service
work when `enable_lua` is set to `0`.

The current headless and Kernel integration workflows do not prove a complete Runtime session.
A full integration test must load the real Runtime tree, exercise its Host API,
and verify shutdown and callback behavior with Lua 5.4 enabled.

## 4. Platform validation

Platform implementations exist for Linux, macOS and Windows. The repository CI
matrix is configured to install each platform's matching Kernel package and run
native builds, E2E scenarios and integration workflows. A local pass on one operating system does
not prove that another platform was tested; use the CI matrix results for that
claim.

## 5. Integration validation

The current Kernel integration test creates, starts, pumps and shuts down a
Backend instance while linking the installed Kernel and logger libraries. It is
a linkage and lifecycle smoke test, not the complete feature suite.

Before production, add end-to-end coverage for create/read/replace/batch/undo/
redo/snapshot/transaction/notification/search through the Backend adapter,
verify detailed error mapping, run Lua 5.4 Runtime tests, and exercise graceful
and abort shutdown with in-flight jobs.

## 6. Why these limits are documented

Version `0.1.0` establishes a standalone library dependency, clear ownership,
bounded resources, portable OS boundaries, the Kernel adapter and a repeatable
test base. It must not freeze choices that the Spec still labels `OPEN` or
`PROPOSED`.
