# Development guide

This guide explains how to build, test and change the Backend repository. It
starts with the common commands, then describes the repository's module
boundaries and the tools used by contributors.

## 1. Repository layout

Backend is a standalone repository. It does not read source files from a sibling
Kernel checkout. The Kernel is installed as a pinned binary dependency under
`.deps/kernel/`; see [Dependencies](dependencies.md).

| Path | Responsibility |
| --- | --- |
| `include/cdin/backend.h` | Public Backend lifecycle and diagnostic API. |
| `include/cdin/backend/` | Reusable public service APIs. |
| `src/core/` | Shared status and low-level helpers. |
| `src/diagnostics/` | One formatting facade for the Kernel logger. |
| `src/kernel/` | Kernel ABI adapter and error/handle translation. |
| `src/runtime/` | Runtime Host bridge; Lua state stays on the owner thread. |
| `src/session/` | Session and view metadata. |
| `src/protocol/` | In-process Frontend Protocol semantics and bounded queues. |
| `src/jobs/`, `src/os/`, `src/platform/` | Standalone, Kernel-independent services. |
| `tests/e2e/` | Real filesystem, process and concurrency workflows. |
| `tests/integration/` | Backend/Kernel and document editing workflows. |
| `examples/` | Complete examples that can be built and run. |
| `scripts/` | Dependency bootstrap, repository checks and packaging. |
| `docs/` | Beginner guides, architecture and Spec traceability. |

The reusable services are built as `cdin_backend_services`. They have no Kernel
or Runtime dependency. The `cdin_backend` host library links those services and
the installed Kernel package. Keep that dependency direction one-way: reusable
services must not include Backend-private or Kernel headers.

Keep each module responsible for one concern. When two modules need the same
logic, move it into a small shared module instead of copying it. Public headers
belong in `include/cdin/`; implementation-only contracts belong beside their
implementation under `src/`.

## 2. Install the Kernel dependency

The Kernel is an external binary library dependency. Install the exact release
pinned in `deps.lock`:

```sh
make deps
```

The installer downloads the platform package, verifies its SHA-256 checksum,
and installs headers, libraries and `pkg-config` metadata under
`.deps/kernel/`. The repository must not vendor Kernel source or use a path such
as `../../kernel/include`.

Backend consumes the package through `pkg-config`:

```sh
pkg-config --modversion cdin-kernel
pkg-config --static --libs cdin-kernel
```

The static metadata includes the Kernel's private `m-mdy-m/log.c` dependency.
Backend must call the Kernel logger API and must not include or link logger
internals independently.

## Artifact-first consumer packages

The release workflow publishes target-specific install prefixes for the
Backend, Protocol and independent services libraries. These archives do not
contain Kernel, logger, Lua, Runtime or Registry. The Composition Root
provisions Kernel ABI, the independently versioned m-mdy-m/log.c SDK and PUC
Lua 5.4 as separate environment contracts.

Read [Artifact packages](artifact-packages.md) for the target ABI matrix,
CMake/pkg-config consumer examples, external contracts and immutable Registry
metadata publication procedure. A binary package is published only after its
five target builds and downloaded-package consumer tests pass.

## 3. Build the host and reusable services

Build only the reusable services, without installing Kernel:

```sh
make services
```

Build all libraries after installing Kernel:

```sh
make deps
make all
```

The output archives are:

| Archive | Dependency | Intended consumer |
| --- | --- | --- |
| `libcdin_backend_services.a` | C11 runtime and platform threads | Any C project that needs portable paths, files, jobs, clocks or processes |
| `libcdin_backend.a` | `cdin_backend_services` and `cdin-kernel` | CDIN host application |

CMake installation also installs public headers and the `cdin-backend-services`
`pkg-config` file. See [Reusing services](reusing-services.md) for a separate
consumer example.

## 4. Run end-to-end and integration scenarios

The suite focuses on observable workflows instead of isolated unit tests.
Run the platform scenarios without Kernel:

```sh
make e2e
```

Run the Backend and Kernel integration scenarios:

```sh
make integration
```

The full local quality gate is:

```sh
make check
```

The scenario list and edge-case coverage are documented in
[Integration testing](integration-testing.md). The CMake equivalent is:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$PWD/.deps/kernel"
cmake --build build
ctest --test-dir build --output-on-failure
```

## 5. Logging and diagnostics

All native Backend records go through the Kernel logger. The diagnostic facade
is intentionally small: modules supply a severity, a subsystem name and a
reason; it formats the source location and sends the record to
`cdin_kernel_log()` through the Kernel adapter.

Examples:

```text
[BACKEND][CORE] backend.c:... cdin_backend_start(): startup requested session_id=...
[BACKEND][PROTOCOL] server.c:... cdin_protocol_hello_accept(): event=hello_version_rejected client_id=... status=VERSION_MISMATCH ...
[BACKEND][JOBS] backend.c:... cdin_backend_shutdown(): stopping worker pool pending=... mode=graceful
```

Use the levels consistently:

| Level | Use it for |
| --- | --- |
| `TRACE` | Resource creation/release, pump counts and detailed protocol transitions. |
| `DEBUG` | Lifecycle decisions, queue state and feature/configuration choices. |
| `INFO` | Successful startup, shutdown and major session transitions. |
| `WARN` | Expected but noteworthy conditions such as stale requests, rejected clients and backpressure. |
| `ERROR` | Failed operations that prevent a requested action or require cleanup. |
| `FATAL` | Unrecoverable conditions only; do not use for ordinary failures. |

Set `config.log_level = CDIN_BACKEND_LOG_TRACE` while debugging. The optional
`log_callback` receives the shared Kernel log stream, so it can capture both
Kernel and Backend records in an application's logging system. The callback is
called on the thread that emitted the record; it must be thread-safe, must not
retain the borrowed message, and must not re-enter Backend APIs.

## 6. Formatting and documentation checks

Use `.editorconfig` for indentation, UTF-8 and LF line endings. Run Markdown
lint with:

```sh
make lint
```

The `check` target does not silently skip lint if `markdownlint-cli2` is
missing. Install the CLI before running the full local gate. The shared
Pipeline documentation workflow also checks Markdown and GitHub Actions.

## 7. Pipeline

CI and release workflows use
[m-mdy-m/pipeline](https://github.com/m-mdy-m/pipeline). `pipeline.toml` is the
source of the native task graph. Common tasks are:

```sh
pipeline run deps
pipeline run e2e
pipeline run build
pipeline run integration
pipeline run check
pipeline run package
```

`pipeline run check` installs the pinned Kernel package, runs the E2E and
integration scenarios, builds both libraries with strict warnings, and checks
module boundaries. The documentation workflow runs Markdown lint separately.
Do not recreate the whole task graph in a new workflow; keep the project-specific
commands in `pipeline.toml` and call the shared Pipeline workflow.

## 8. PSX project checks

`psx.yml` declares this repository as a C library and checks for source, public
headers, examples, E2E and integration scenarios, documentation, contribution
guidance and workflows. Generated directories such as `.deps/`, `build/` and
`.pipeline/` are excluded.

PSX checks repository structure. It does not replace compilation, integration
scenarios or runtime validation.

## 9. Git flow with gix

`.gix/config.default` is the reviewed branch-flow template. Run the repository
initializer and then `gix flow init` after cloning. The generated
`.gix/config` is local Git-flow state and is intentionally not committed.

The expected branches are `main` and `develop`, with `feature/*`, `bugfix/*`,
`release/*` and `hotfix/*` topic branches. Do not add a local `.gix/config` to a
source archive.

## 10. Before opening a pull request

Run the native checks, inspect the diff, and update documentation alongside
code:

```sh
make check
git diff --check
git status --short
```

Every behavior that comes from the Spec should link to the exact requirement or
open decision in a source comment or in
[Spec traceability](spec-traceability.md). Explain the reason for non-obvious
ownership, bounds, cleanup order and platform-specific behavior. A comment
should help the next contributor understand why the code is shaped this way,
not merely repeat the statement on the next line.

## Independent resource limits

`max_message_bytes` bounds Frontend Protocol payloads. `max_log_message_bytes` independently bounds Kernel Logger records and defaults to 16 KiB. Keep these limits separate: a deliberately small network/UI payload must never suppress the diagnostic record that explains why a request was rejected.
