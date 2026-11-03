# Known limitations — Backend 1.0.0

This page describes the supplied Backend source snapshot and distinguishes implemented C-side functionality from cross-repository integration that still requires the Runtime source change and real dependencies.

## 1. Frontend Protocol

Implemented in the Backend source:

- deterministic-CBOR encode/decode and four-byte big-endian stream framing;
- payload validators for the complete Protocol 1.0.0 catalogue;
- bounded client queues, input sequence checks, Driver/Observer permissions and FIFO Driver promotion;
- atomic UI snapshot/response batches with real `ui.flush` markers, ACK release and backpressure;
- Lua-side `cdin.ui-model.v1` projection to typed Protocol messages;
- owner-thread inbound event dispatch from the bounded Protocol queue to the Runtime event entry point;
- `view.request_lines` → `host.documents.read_lines` → version-pinned Kernel Snapshot → `view.lines` response;
- display-free Headless Frontend and deterministic frame/event replay tests;
- standalone Protocol build/test target and fuzz decode entry point.

Still missing or externally gated:

- no TCP/Unix-domain socket, Windows Named Pipe or terminal-stream adapter;
- Live Backend/Kernel startup has not yet been run against the paired Runtime 1.0.0-rc.1 source (the Runtime Lua unit and simulated-Host integration suites pass);
- no real Lua 5.4 + installed Kernel live UI/event/line-read integration result in this workspace;
- no production Frontend toolkit/application; Headless Frontend is a test harness;
- optional clipboard/dialog capabilities are not advertised (accepted capability set is empty).

See [Frontend Protocol implementation status](frontend-protocol.md) for the exact source paths and owner-thread flow.

## 2. Runtime and Kernel integration

Backend 1.0.0 reports Host API `1.0.0` and Frontend Protocol `1.0.0`. The paired Runtime 1.0.0-rc.1 source advertises the same Protocol and implements the matching `documents.read_lines` and Frontend event paths. Its Lua 5.4.7 unit and simulated-Host integration suites pass (137 / 131 assertions). The public GitHub Runtime repository was not modified. Live Backend + Lua C bridge + Kernel integration remains unverified in this environment.

Kernel public header declares ABI `1.1.0` and package version `1.2.1`; Backend consumes it only through the existing public-header adapter. No Kernel ABI change is included.

The local environment does not have the installed `cdin-kernel >= 1.2.1` pkg-config package or PUC Lua 5.4 development headers. Therefore the real Kernel/Lua Backend target could not be configured, compiled or exercised here. Kernel-independent Protocol/server/Headless and Backend service E2E tests were run; those results do not prove a live Runtime connection.

## 3. Transport and platform matrix

The framing helpers accept partial-I/O callbacks, but the Backend does not own OS connection setup or transport security. Before an independent Frontend can connect out of process, add Unix Domain Socket on Linux/macOS and Named Pipe on Windows (or explicitly decide a different release scope), including permission/authentication policy, timeout, cancellation, partial I/O, bounded buffers and disconnect cleanup.

Only the available Linux environment was built and tested in this workspace. macOS and Windows native builds, Kernel package integration, Lua 5.4 integration and platform transport tests remain outstanding.

## 4. ABI and release status

Backend source/API and Frontend Protocol are versioned `1.0.0` in this package. The paired Runtime package is `0.4.1`, Host API `1.0.0`, Protocol `1.0.0`. Complete product integration remains gated: pass real Lua + Kernel tests, implement and test the selected transport, obtain the platform matrix, and verify production dependencies before claiming a production-ready CDIN editor.
