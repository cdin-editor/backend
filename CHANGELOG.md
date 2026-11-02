# Changelog

All notable changes to the Backend are recorded here. This project follows Semantic Versioning while the `0.x` API remains provisional.

## [0.1.0] - 2026-11-02

### Added

- Consume `cdin-kernel` as a pinned, checksum-verified release package installed under `.deps/kernel/`; resolve the public header and static Kernel/logger link requirements through `pkg-config` instead of a sibling source path.
- Add a real Kernel link/lifecycle smoke test, reusable Pipeline tasks, native Linux/macOS/Windows CI, PSX library checks and a standalone gix flow.
- Add repository governance files, GitHub issue/PR templates, `.editorconfig`, source-level Spec references and dependency/traceability guides.
- Rewrite the README around the Backend's role, current status, dependency setup and test workflow.

- C11 Backend lifecycle with create, start, pump, shutdown, and destroy operations.
- Kernel ABI 1.1.0+ adapter with ABI/feature checks and generation-checked document, snapshot, transaction, and notification handles.
- Runtime Host API 0.2.0 bridge for document operations, snapshots, transactions, undo/redo, notifications, literal/byte search, UI model publication, logging, atomic file saves, clocks, and child processes.
- Kernel-owned logger configuration through the Kernel C ABI; public Backend log-level constants keep the Kernel ABI private to one adapter module, and no second Backend logger is created.
- Session metadata and view lifecycle management without duplicating document contents.
- Bounded worker pool with owner-thread completion callbacks and cooperative cancellation.
- UTF-8 path handling, bounded chunked file reads, atomic file writes, directory listing, file metadata, and shell-free process spawning for POSIX and Windows targets.
- In-process Frontend Protocol handshake, driver/observer policy, bounded queues, coalescing, flush sequences, and ACK backpressure.
- Split Kernel-independent path, filesystem, process, clock, thread and job services into the separately linkable `cdin_backend_services` library so other C projects can reuse them without Kernel or Runtime.
- Add four end-to-end scenarios and two Kernel/Backend integration scenarios instead of a separate unit-test target; cover concurrency, cancellation, shutdown, atomic file publication, process supervision, Frontend backpressure, Kernel document edits and diagnostic records.
- Route native diagnostics through the Kernel logger with stable `[BACKEND][AREA]` prefixes, source locations, TRACE/DEBUG/INFO/WARN/ERROR/FATAL levels and an independent log-record size limit.
- CMake and Make builds, strict compiler warnings, Markdown lint configuration, PSX metadata, Pipeline tasks, and a standalone gix main/develop and topic-branch flow configuration.
- Beginner-oriented architecture, Host API, OS services, Frontend Protocol, development, and troubleshooting guides.

### Known limitations

- Serialized Frontend Protocol encoding and transports remain deferred while Spec OD-22 is open.
- Runtime UI models are retained in-process but are not yet translated into Frontend messages.
- Optional Frontend capability bit assignments and concurrent multi-driver mutation remain open Spec decisions.
- Lua 5.4 and cross-platform builds require validation in environments with their development toolchains.
