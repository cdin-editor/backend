# Specification traceability

This page connects the implementation to the exact CDIN Spec text. It is useful
when a reader asks why a function exists, which component owns it, or whether a
behavior is already accepted. Requirement IDs describe the contract; a source
comment explains the local implementation decision.

The canonical Spec repository is
[cdin-editor/spec](https://github.com/cdin-editor/spec). The status of a
requirement (`ACCEPTED`, `PROPOSED`, `EXISTING`, `OPEN` or `DEFERRED`) remains
owned by that repository.

## 1. Lifecycle and Kernel boundary

| Backend code | Spec reference | What the code is responsible for |
| --- | --- | --- |
| `cdin_backend_create()` in `src/backend.c` | [BE-004 — Kernel creation, ABI check and graceful ownership](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [LIFE-001 — process initialization order](https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md); [ABI-014 — query and reject incompatible Kernel ABI](https://github.com/cdin-editor/spec/blob/main/docs/architecture/abi-and-compatibility.md) | Validate configuration, query the ABI/features through the adapter, create Kernel ownership and fail before exposing a partially usable Backend. |
| `cdin_backend_start()` | [LIFE-001 — allocator/logging → ABI check → Kernel → Runtime → Protocol → Frontends](https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md); [LIFE-002 — unwind partial startup](https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md); [LIFE-004 — start without a Frontend](https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md) | Create services in dependency order, permit headless startup and unwind resources if a later stage fails. |
| `cdin_backend_shutdown()` / `cdin_backend_destroy()` | [OD-18 — shutdown with operations in flight](https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-18); [LIFE-011 — release resources and invalidate owned handles](https://github.com/cdin-editor/spec/blob/main/docs/architecture/lifecycle.md) | Separate graceful drain from abort behavior, then release every resource owned by the instance. |
| `src/kernel/adapter.c` | [BE-003 — Backend is the only direct Kernel ABI client](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [BE-004 — Kernel host owns create/limits/lifetime](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [ABI-014 — ABI-version and feature checks](https://github.com/cdin-editor/spec/blob/main/docs/architecture/abi-and-compatibility.md) | Keep all Kernel headers, structs, status values and handles inside one adapter. Convert them to Backend-owned types before crossing the module boundary. |

## 2. Threads, queues and system services

| Backend code | Spec reference | What the code is responsible for |
| --- | --- | --- |
| `src/runtime/lua_host.c` | [BE-007 — workers never touch Lua](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [ARCH-025 — main-thread ownership of Lua](https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md); [OD-32 — Lua-to-native bridge behind the Runtime Host API](https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-32) | Keep Lua calls on the owner thread. The PUC Lua 5.4 C API is the current isolated candidate; OD-32 has not selected a bridge. |
| `src/jobs/pool.c` | [BE-008 — move potentially unbounded work off the main thread](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [BE-009 — bound Backend queues](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [ARCH-021 — every queue has a cap and refusal/eviction policy](https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md); [ARCH-023 — no unbounded work on the event thread](https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md) | Bound submissions and completions, keep worker callbacks in plain C, and deliver completions only when the owner thread pumps them. |
| `src/os/process.c` | [BE-011 — argument-array process creation](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [ARCH-024 — never spawn through a shell](https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md) | Accept an executable plus argument array and use native spawn APIs. Do not interpret an arbitrary command string through a shell. |
| `src/os/fs.c`, `src/os/path.c` and `src/platform/` | [BE-005 — toolkit-independent OS services](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [ARCH-021 — bounded buffers and queues](https://github.com/cdin-editor/spec/blob/main/docs/architecture/overview.md) | Keep file/path behavior independent from a display toolkit, preserve UTF-8 path boundaries and apply explicit size limits. |

## 3. Session and Frontend Protocol

| Backend code | Spec reference | What the code is responsible for |
| --- | --- | --- |
| `src/session/session.c` | [BE-001 — headless session and attach/detach](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md); [ARCH-050 — Runtime does not own document contents](https://github.com/cdin-editor/spec/blob/main/docs/architecture/module-boundaries.md) | Store session, document and view metadata only. Document bytes and authoritative versions remain in Kernel. |
| `src/protocol/server.c` | [PROTO-010/011 — handshake and capability negotiation](https://github.com/cdin-editor/spec/blob/main/docs/architecture/frontend-protocol.md); [PROTO-012/013 — simultaneous clients and provisional driver rule](https://github.com/cdin-editor/spec/blob/main/docs/architecture/frontend-protocol.md); [OD-24 — who may mutate](https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-24) | Implement the current in-process handshake and provisional driver/observer behavior without claiming that the open multi-Frontend policy is final. |
| `src/protocol/server.c` and `src/protocol/backend_protocol.c` | [PROTO-021 — bounded unacknowledged flushes and coalescing](https://github.com/cdin-editor/spec/blob/main/docs/architecture/frontend-protocol.md); [OD-22 — serialized protocol encoding](https://github.com/cdin-editor/spec/blob/main/docs/adr/open-decisions.md#od-22); [PROTO-030/031 — transport-independent messages and framed serialized transport](https://github.com/cdin-editor/spec/blob/main/docs/architecture/frontend-protocol.md) | Implement bounded in-process semantics. Do not add a wire codec until OD-22 selects a representation. |

## 4. Logging and diagnostics

Backend diagnostics use the Kernel's public logging API through `src/kernel/adapter.c`.
The release package supplies `m-mdy-m/log.c` as a private transitive dependency
of `cdin-kernel.pc` (`Requires.private`). Backend deliberately does not include
`m-mdy-m/log.h`, compile a second logger, or link the logger archive directly.
This follows the Backend diagnostics responsibility in
[Backend responsibilities](https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md)
and the public Kernel C ABI described by
[ABI and compatibility](https://github.com/cdin-editor/spec/blob/main/docs/architecture/abi-and-compatibility.md).

## 5. How to document a new Spec-linked function

When a function implements a requirement, place a short comment near the
function or module boundary. State the requirement's exact title and explain
why this implementation choice follows it. For example:

```c
/* BE-011 — the Backend starts processes with argv only; ARCH-024 forbids
 * routing an arbitrary command string through a shell. The OS adapter keeps
 * that rule identical on POSIX and Windows. */
```

Use the permanent Spec path and a stable anchor when it exists. Do not invent an
OD number, paraphrase an open decision as an accepted rule, or claim that a
proposed requirement is already accepted. If the implementation intentionally
differs from the Spec, explain the gap in `docs/limitations.md` and raise it
for review rather than hiding it in a source comment.
