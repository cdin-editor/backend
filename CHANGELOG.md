# Changelog

All notable changes to CDIN Backend are documented in this file.

The format follows [Keep a Changelog].
Versioning follows [Semantic Versioning].

The `1.2.1` entry describes the next Backend package candidate. The Registry
Catalog continues to select the immutable `1.2.0` package until the matching
tag, all five native artifacts, their checksums, and the new release metadata
have been published and verified.

## [1.2.1] - 2026-11-10

### Added

- Add a snapshot-pinned Host document line-lookup bridge. Callers provide a
  document identity, byte offset, and expected document version, and receive
  a versioned line result without exposing Kernel handles to Runtime.
- Prepare native install-prefix packages for Linux x86_64/ARM64, macOS
  x86_64/ARM64, and Windows x86_64 UCRT64 with immutable artifact identity,
  SHA-256 checksums, and build/source provenance.
- Export relocatable CMake package targets and complete pkg-config metadata
  for Backend, Protocol, and Kernel-independent services.
- Add clean-prefix consumer tests for CMake and static pkg-config linking.
- Add a fail-closed release-metadata generator. It requires all five target
  archives, the exact source revision, artifact identity, URI, size, and
  checksum before emitting immutable Component Release Metadata.

### Fixed

- Correct the Protocol server regression test to expect the reattach role
  notification at the next per-connection frame sequence after the earlier
  promotion event.

## [1.2.0] - 2026-11-07

### Added

- Add bounded `host.fs:read_file()` for exact binary file bytes with
  structured file-read errors.
- Add trusted owner-thread Backend C entry points for Runtime extension
  load/unload. These calls remain outside Frontend Protocol and require an
  explicitly configured resolver root.
- Add a standalone deterministic-CBOR Frontend Protocol codec, bounded
  bidirectional queues, Headless replay tests, and a fuzz entry point.
- Add protocol and Runtime integration documentation covering the remaining
  transport, Kernel/Lua, and platform verification gates.

### Changed

- Append resolver source-root and per-module budget fields to
  `cdin_backend_config`. The `struct_size` convention preserves compatibility
  with callers that provide an older configuration structure.
- Keep the Runtime Host API and Frontend Protocol compatibility versions
  independently versioned at `1.0.0`; the optional extension resolver does
  not change either contract.

### Security

- Add the disabled-by-default, Backend-owned Runtime extension resolver with
  canonical root confinement, no-follow file traversal, reparse-point and
  symlink denial, exact-byte source reads, owner-thread enforcement, and
  per-module, module-count, and aggregate limits.
- Keep extension loading unavailable unless an application explicitly
  configures a trusted source root. The resolver does not expose extension
  lifecycle operations to remote Frontend clients.

## [1.0.0] - 2026-11-04

### Added

- Freeze the Backend library source version at `1.0.0` and align the
  independent Frontend Protocol codec/server with shared Protocol `1.0.0`.
- Require Protocol 1.0.0 `ui.options.options` and validate its typed option
  values.
- Accept nullable PaneTree leaf `active_view` and project an omitted Runtime
  nil value as CBOR null.
- Record the shared Protocol 1.0.0 contract and Backend/Runtime integration
  gates.

### Changed

- Update the companion Runtime integration patch to advertise Frontend
  Protocol `1.0.0`; Runtime Host API is `1.0.0`.
- Preserve the Backend's explicit empty optional-capability set. This release
  does not advertise clipboard or dialog capabilities.

## [0.1.0] - 2026-11-02

### Added

- Consume `cdin-kernel` as a pinned, checksum-verified release package
  installed under `.deps/kernel/`; resolve the public header and static
  Kernel/logger link requirements through `pkg-config` instead of a sibling
  source path.
- Add a real Kernel link/lifecycle smoke test, reusable Pipeline tasks, native
  Linux/macOS/Windows CI, PSX library checks, and a standalone gix flow.
- Add repository governance files, GitHub issue/PR templates, `.editorconfig`,
  source-level Spec references, and dependency/traceability guides.
- Rewrite the README around the Backend's role, current status, dependency
  setup, and test workflow.
- Add the C11 Backend lifecycle with create, start, pump, shutdown, and
  destroy operations.
- Add the Kernel ABI 1.1.0+ adapter with ABI/feature checks and
  generation-checked document, snapshot, transaction, and notification
  handles.
- Add the initial Runtime Host API bridge for document operations, snapshots,
  transactions, undo/redo, notifications, literal/byte search, UI model
  publication, logging, atomic file saves, clocks, and child processes.
- Configure Kernel-owned logging through the Kernel C ABI and expose public
  Backend log-level constants without creating a second Backend logger.
- Add session metadata and view lifecycle management without duplicating
  document contents.
- Add a bounded worker pool with owner-thread completion callbacks and
  cooperative cancellation.
- Add UTF-8 path handling, bounded chunked file reads, atomic file writes,
  directory listing, file metadata, and shell-free process spawning for POSIX
  and Windows targets.
- Add the in-process Frontend Protocol handshake, driver/observer policy,
  bounded queues, coalescing, flush sequences, and ACK backpressure.
- Split Kernel-independent path, filesystem, process, clock, thread, and job
  services into the separately linkable `cdin_backend_services` library.
- Add end-to-end and Kernel/Backend integration scenarios covering
  concurrency, cancellation, shutdown, atomic file publication, process
  supervision, Frontend backpressure, Kernel document edits, and diagnostics.
- Route native diagnostics through the Kernel logger with stable
  `[BACKEND][AREA]` prefixes, source locations, and bounded log records.
- Add CMake and Make builds, strict compiler warnings, Markdown lint
  configuration, PSX metadata, Pipeline tasks, and a standalone gix
  main/develop and topic-branch flow configuration.
- Add architecture, Host API, OS services, Frontend Protocol, development,
  and troubleshooting guides.

At this release, the Frontend Protocol wire encoding and Runtime-to-Frontend
projection were still evolving. Later entries record the shared Protocol
1.0.0 implementation and subsequent integration work.

[Keep a Changelog]: https://keepachangelog.com/en/1.1.0/
[Semantic Versioning]: https://semver.org/spec/v2.0.0.html
