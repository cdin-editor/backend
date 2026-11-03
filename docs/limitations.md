# Known limitations — Backend 0.2.0

This page describes the supplied Backend source snapshot and distinguishes implemented C-side functionality from the remaining live CI, transport and platform verification gates.

## 1. Frontend Protocol

Implemented in the Backend source:

- deterministic-CBOR encode/decode and four-byte big-endian stream framing;
- payload validators for the complete Protocol 0.2.0 catalogue;
- bounded client queues, input sequence checks, Driver/Observer permissions and FIFO Driver promotion;
- atomic UI snapshot/response batches with real `ui.flush` markers, ACK release and backpressure;
- Lua-side `cdin.ui-model.v1` projection to typed Protocol messages;
- owner-thread inbound event dispatch from the bounded Protocol queue to the Runtime event entry point;
- `view.request_lines` → `host.documents.read_lines` → version-pinned Kernel Snapshot → `view.lines` response;
- display-free Headless Frontend and deterministic frame/event replay tests;
- standalone Protocol build/test target and fuzz decode entry point.

Still missing or externally gated:

- no TCP/Unix-domain socket, Windows Named Pipe or terminal-stream adapter;
- a green live Lua 5.4 + installed Kernel + Runtime integration result is not yet available from this workspace;
- no production Frontend toolkit/application; Headless Frontend is a test harness;
- optional clipboard/dialog capabilities are not advertised (accepted capability set is empty).

See [Frontend Protocol implementation status](frontend-protocol.md) for the exact source paths and owner-thread flow.

## 2. Runtime and Kernel integration

Backend and the aligned Runtime source now both target Host API `0.3.0` and
Frontend Protocol `0.2.0`. Runtime implements `documents.read_lines`,
`Runtime:read_view_lines()`, `Runtime:handle_frontend_event()`, UI snapshot
version/style metadata and bounded model limits. The Backend CI workflow consumes
the Runtime repository directly and runs its tests before the live native smoke
test. The Runtime change must merge before the Backend workflow update because the
live integration workflow checks out Runtime `main`.

Kernel public header declares ABI `1.1.0` and package version `1.2.1`; Backend consumes it only through the existing public-header adapter. No Kernel ABI change is included.

The local environment does not have the installed `cdin-kernel >= 1.2.1` pkg-config package or PUC Lua 5.4 development headers. Therefore the real Kernel/Lua Backend target could not be configured, compiled or exercised here. Kernel-independent Protocol/server/Headless and Backend service E2E tests were run; those results do not prove a live Runtime connection.

## 3. Transport and platform matrix

The framing helpers accept partial-I/O callbacks, but the Backend does not own OS connection setup or transport security. Before an independent Frontend can connect out of process, add Unix Domain Socket on Linux/macOS and Named Pipe on Windows (or explicitly decide a different release scope), including permission/authentication policy, timeout, cancellation, partial I/O, bounded buffers and disconnect cleanup.

Only the available Linux environment was built and tested in this workspace. macOS and Windows native builds, Kernel package integration, Lua 5.4 integration and platform transport tests remain outstanding.

## 4. ABI and release status

Backend library version `0.2.0`, Host API `0.3.0` and Frontend Protocol `0.2.0` are provisional. Do not tag or publish Backend `1.0.0` until the live Runtime + Lua 5.4 + Kernel workflow passes, the transport scope is resolved and implemented, the platform matrix is green, and the public ABI/Host API/Protocol stability policy is accepted.
