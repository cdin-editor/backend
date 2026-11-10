## [1.2.1] - 2026-11-10

### Added

- Prepare native install-prefix packages for Linux x86_64/ARM64, macOS
  x86_64/ARM64 and Windows x86_64 UCRT64 with immutable artifact identity,
  SHA-256 and build/source provenance.
- Export relocatable CMake package targets and complete pkg-config metadata
  for Backend, Protocol and Kernel-independent services.
- Add independent clean-prefix consumer tests for CMake and static pkg-config
  linking. Kernel ABI, the independently versioned logger SDK and Lua 5.4
  remain externally provisioned dependencies.
- Add a fail-closed metadata generator that requires all five target archives,
  exact source revision, artifact identity, URI, size and checksum before it
  emits a new immutable Component Release Metadata document.

### Verification

- Release readiness is gated by the exact-tag five-target Pipeline matrix,
  downloaded-artifact checksum checks and native external-consumer tests.
- Native CI exercises the package archive/extract/consumer path on Linux,
  macOS and Windows UCRT64.
- Registry Catalog publication is a separate immutable metadata update after
  the real release assets have been created and verified.

> **Package publication boundary:** Backend `1.2.0` remains the active
> Registry Catalog identity until the deliberate `v1.2.1` tag is published,
> its five native assets are verified, and the new metadata/Catalog identity
> is committed. This changelog section prepares the release notes; it does
> not itself create a tag, release or Catalog update.


### Implemented follow-on changes

- Correct the Protocol server regression test to expect the reattach role notification at the next per-connection frame sequence after the earlier promotion event.
- Add an optional, snapshot-pinned Host document line-lookup bridge: callers provide document identity, byte offset and expected document version, and receive a versioned line result without exposing Kernel handles to Runtime.
- Add optional bounded `host.fs:read_file()` for exact binary file bytes with structured file-read errors.
- Add the disabled-by-default, Backend-owned Runtime extension resolver with canonical root confinement, no-follow file traversal, reparse/symlink denial, exact-byte source reads, owner-thread enforcement and per-module/module-count/aggregate limits.
- Add trusted owner-thread Backend C entry points for Runtime extension load/unload; keep extension lifecycle out of Frontend Protocol and fail closed without an explicitly configured resolver root.
- Publish the trusted extension lifecycle entry points in Backend 1.2.0; the Runtime Host API and Frontend Protocol compatibility versions remain independent and unchanged.
- Append resolver source-root and budget fields to `cdin_backend_config`; older callers remain compatible through `struct_size`. Bump Backend library/package version to `1.1.0`; Host API remains `1.0.0` because the resolver is an optional embedding configuration, not a Host API change.

- Add standalone deterministic-CBOR Frontend Protocol codec, bounded bidirectional queues, headless replay tests and fuzz entry point.
- Resolve Spec ADR-0018/0019 links and document remaining Runtime, transport, Kernel/Lua and platform integration gates.

# Changelog

## [1.0.0] - 2026-11-04

### Added

- Freeze the Backend library source version at `1.0.0` and align the independent Frontend Protocol codec/server with shared Protocol `1.0.0`.
- Require Protocol 1.0.0 `ui.options.options` and validate its typed option values.
- Accept nullable PaneTree leaf `active_view` and project an omitted Runtime nil value as CBOR null.
- Record shared Protocol 1.0.0 contract and Backend/Runtime integration gates.

### Changed

- Update the companion Runtime integration patch to advertise Frontend Protocol `1.0.0`; Runtime Host API is `1.0.0`.
- Preserve the Backend's explicit empty optional-capability set; no clipboard/dialog capability is advertised by this build.

### Verification limits

- Kernel-independent Protocol, Headless and Backend service tests pass in the supplied Linux build. The full Kernel + Lua 5.4 Backend target and the Runtime patch have not been built/applied in this environment.
- No concrete UDS/Named Pipe transport or native macOS/Windows verification is included. This is a source/API and wire-contract version freeze, not a claim that the complete CDIN product is production-integrated.

All notable changes to the Backend are recorded here. This project follows Semantic Versioning while the `0.x` API remains provisional.

## [0.1.0] - 2026-11-02

### Added

- Consume `cdin-kernel` as a pinned, checksum-verified release package installed under `.deps/kernel/`; resolve the public header and static Kernel/logger link requirements through `pkg-config` instead of a sibling source path.
- Add a real Kernel link/lifecycle smoke test, reusable Pipeline tasks, native Linux/macOS/Windows CI, PSX library checks and a standalone gix flow.
- Add repository governance files, GitHub issue/PR templates, `.editorconfig`, source-level Spec references and dependency/traceability guides.
- Rewrite the README around the Backend's role, current status, dependency setup and test workflow.

- C11 Backend lifecycle with create, start, pump, shutdown, and destroy operations.
- Kernel ABI 1.1.0+ adapter with ABI/feature checks and generation-checked document, snapshot, transaction, and notification handles.
- Initial Runtime Host API bridge for document operations, snapshots, transactions, undo/redo, notifications, literal/byte search, UI model publication, logging, atomic file saves, clocks, and child processes. The current Backend API version is 0.3.0; see `docs/host-api.md`.
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

- Historical note: at this release snapshot, the Frontend Protocol wire encoding was not yet decided. Spec ADR-0018 now settles deterministic CBOR framing; this Backend version implements the codec but not OS transports or Runtime projection/dispatch.
- Runtime UI models are retained in-process but are not yet translated into Frontend messages.
- Optional Frontend capability bit assignments and concurrent multi-driver mutation remain open Spec decisions.
- Lua 5.4 and cross-platform builds require validation in environments with their development toolchains.
