<p align="center">
  <a href="https://github.com/cdin-editor/cdin-editor.github.io/tree/main/public/cdin-icons" target="_blank" rel="noreferrer">
    <img src="https://raw.githubusercontent.com/cdin-editor/cdin-editor.github.io/main/public/cdin-icons/source/cdin-original-transparent-512.png" width="180" alt="CDIN logo" />
  </a>
</p>
<p align="center">
  <a href="https://github.com/cdin-editor/backend/actions/workflows/ci.yml"><img src="https://github.com/cdin-editor/backend/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0-blue.svg" alt="License: GPL-3.0-or-later"></a>
  <a href="https://en.cppreference.com/w/c/11"><img src="https://img.shields.io/badge/Language-C11-blue.svg" alt="C11"></a>
  <a href="https://github.com/cdin-editor/spec"><img src="https://img.shields.io/badge/CDIN-Specification-purple.svg" alt="CDIN specification"></a>
</p>

<h1 align="center">CDIN Backend</h1>

<p align="center">The headless coordinator between the CDIN Kernel, Runtime and Frontends.</p>

CDIN Backend is the native core that hosts the Kernel and Runtime, provides
operating-system services, manages editor sessions, and connects Frontends. It
is written in C11 and is designed to run without a display toolkit or an
attached Frontend.

## What is Backend?

When a user edits a document, the Backend connects the parts that need to work
together. It does not decide what an editor command means and it does not draw
the screen. Its job is to host the components that do those things and provide
the services they need.

The parts of CDIN have separate responsibilities:

| Part | Responsibility |
| --- | --- |
| **Frontend** | Receives input and displays the editor. It owns pixels, terminal cells, visual layout and drawing. |
| **Runtime** | Coordinates commands, key bindings, views, settings, extensions and the semantic UI model. |
| **Backend** | Owns the session, hosts Kernel and Runtime, provides OS services, and relays Frontend Protocol messages. |
| **Kernel** | Owns document bytes, edits, versions, snapshots, notifications and undo/redo history. |

This separation prevents the Backend or Runtime from keeping a second,
authoritative copy of document contents. Kernel operations go through the
Kernel's public C ABI, and only the Backend's Kernel adapter includes the
Kernel header.

## Current status

The next package identity prepared by the release publisher is
`1.2.2`. The published `1.2.1` release predates the ADR-0028 publisher
merge and remains immutable; its five native package assets are not retrofitted
with a canonical metadata asset. The `1.2.2` tag workflow will build and verify
all five target packages, then generate the canonical
`backend-1.2.2.json` Release Asset from that exact tag commit. It does not
become an active Catalog candidate until the Release assets are published and
Registry discovers the metadata.

The package provides the shared Frontend Protocol 1.0.0 codec/server, reusable
OS/job services, Kernel adapter, PUC Lua 5.4 bridge, Runtime UI-model
projection, owner-thread inbound event dispatcher, version-pinned line-read
service, Headless Frontend harness and integration workflows. The artifact
package model and external-consumer requirements are documented in
[Artifact packages](docs/artifact-packages.md). Backend is
**not yet a complete runnable CDIN editor** on its own; the paired Runtime
1.0.0-rc.2, live Lua + Kernel integration and concrete Unix Socket/Windows
Named Pipe transport remain product/release boundaries tracked by the
composition root and Spec.

The [CDIN specification](https://github.com/cdin-editor/spec) is the source of
truth for cross-project contracts. This README describes the current state of
this repository; it does not override the specification. See
[Spec traceability](docs/spec-traceability.md) for the exact requirements and
decisions that shape the implementation.

## Try it

You need a C11 compiler, GNU Make or CMake, `curl`, and `pkg-config`. On Windows,
use the MSYS2 UCRT64 environment and its matching compiler and build tools.

Clone the repository and install the pinned Kernel **binary package**:

```sh
git clone https://github.com/cdin-editor/backend.git
cd backend
make deps
```

The dependency installer downloads the matching release archive, verifies its
SHA-256 checksum, and installs headers and libraries under `.deps/kernel/`.
The source of `cdin/kernel` is not copied into this repository and no sibling
Kernel checkout is needed.

Run the tests and build the library:

```sh
make services
make e2e
make all
make integration
```

`make services` builds the reusable OS/job library without Kernel. `make e2e`
runs real filesystem, process and concurrency workflows. `make integration`
links against the installed Kernel and tests Backend session behavior plus
real document operations such as replace, transaction, undo/redo, snapshots,
notifications and search. For the complete local gate, install
[`markdownlint-cli2`](https://github.com/DavidAnson/markdownlint-cli2) and run:

```sh
make check
```

CI and releases run through
[m-mdy-m/pipeline](https://github.com/m-mdy-m/pipeline). The task graph is in
[`pipeline.toml`](pipeline.toml); see [Build and test](docs/development.md).

## A small example

A Backend object is created first, then started. This minimal example disables
Lua so it can be used to verify the native Kernel connection without requiring
a Lua development package:

```c
#include "cdin/backend.h"

int main(void)
{
    cdin_backend_config config;
    cdin_backend *backend = NULL;

    cdin_backend_config_init(&config);
    config.enable_lua = 0;
    config.enable_headless_protocol = 0;

    if (cdin_backend_create(&config, &backend) != CDIN_BACKEND_OK) return 1;
    if (cdin_backend_start(backend) != CDIN_BACKEND_OK) {
        cdin_backend_destroy(backend);
        return 1;
    }

    /* The application pumps Backend work from this same owner thread. */
    if (cdin_backend_shutdown(backend, 0) != CDIN_BACKEND_OK) {
        cdin_backend_destroy(backend);
        return 1;
    }
    cdin_backend_destroy(backend);
    return 0;
}
```

The full source and link commands are in the
[Getting started guide](docs/getting-started.md). A static Backend archive does
not contain Kernel code; the final executable links the Backend and the Kernel
package. `pkg-config --static --libs cdin-kernel` returns the Kernel and its
private logger link requirements together.

## What it provides

- Backend lifecycle, session identity and document/view metadata.
- A single Kernel ABI adapter with ABI/feature checks, handle mapping and error
  detail translation.
- A Runtime Host bridge, with Lua state owned by the Backend's creating thread.
- An optional trusted extension source resolver, disabled unless an application
  configures an administrator-managed root; nested modules remain confined to
  each extension namespace and source/module/aggregate reads are bounded.
- Bounded worker queues and owner-thread completion delivery.
- Portable path, bounded binary-safe file reads, atomic filesystem writes, clock, thread and argument-vector process services.
- In-process Frontend handshake, Driver/Observer roles, bounded input/output
  queues, atomic UI snapshots, `ui.flush` groups, acknowledgements and backpressure.
- Runtime UI Model projection and owner-thread inbound event dispatch.
- Version-pinned `view.request_lines` service backed by Kernel Snapshots and
  the public Kernel line APIs.
- Kernel-owned logging, with Backend diagnostics routed through the Kernel log
  API rather than a second logger.

Native Backend messages use stable component prefixes such as `[BACKEND][CORE]`,
`[BACKEND][SESSION]`, `[BACKEND][PROTOCOL]`, `[BACKEND][JOBS]`,
`[BACKEND][RUNTIME]` and `[BACKEND][KERNEL]`. Each record includes its source
file, line and function. Configure `log_level` to `TRACE`, `DEBUG`, `INFO`,
`WARN`, `ERROR` or `FATAL` when diagnosing startup, queue pressure, Frontend
handshakes or shutdown.

## Documentation

| Guide | What it covers |
| --- | --- |
| [Getting started](docs/getting-started.md) | Build the library and use the first Backend lifecycle. |
| [Dependencies](docs/dependencies.md) | Download, checksum verification and linking of the Kernel binary package. |
| [Architecture](docs/architecture.md) | Module ownership, thread model and lifecycle. |
| [Runtime Host API](docs/host-api.md) | Services exposed to the Lua Runtime. |
| [Extension resolver](docs/extension-resolver.md) | Trusted extension source layout, path confinement, opt-in configuration and read limits. |
| [Frontend Protocol](docs/frontend-protocol.md) | Message schemas, UI projection, inbound dispatch, line reads and transport limits. |
| [OS services](docs/os-services.md) | Files, paths, processes, jobs, clocks and thread primitives. |
| [Reuse services](docs/reusing-services.md) | Use the standalone `cdin_backend_services` library in other projects. |
| [Integration testing](docs/integration-testing.md) | Real-world E2E and Kernel integration scenarios. |
| [Spec traceability](docs/spec-traceability.md) | Exact requirement and open-decision references in source and docs. |
| [Development guide](docs/development.md) | Build, test, lint, Pipeline and gix workflow. |
| [Known limitations](docs/limitations.md) | Behavior that is incomplete or remains controlled by open Spec decisions. |
| [Changelog](CHANGELOG.md) | Changes recorded for each version. |

## CDIN ecosystem

Backend is one part of CDIN. These repositories describe the neighboring
projects:

| Project | Role |
| --- | --- |
| [CDIN](https://github.com/cdin-editor/cdin) | The editor application and composition root. |
| [Kernel](https://github.com/cdin-editor/kernel) | The binary-safe document and text engine. |
| [Runtime](https://github.com/cdin-editor/runtime) | The Lua editor-behavior layer. |
| [Specification](https://github.com/cdin-editor/spec) | Shared contracts and architecture decisions. |
| [Pipeline](https://github.com/m-mdy-m/pipeline) | Reusable task runner and CI workflows. |
| [gix](https://github.com/m-mdy-m/gix) | Repository branch-flow tooling. |

## Contributing

Focused bug fixes, tests, documentation improvements and portability work are
welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing a component
boundary or a cross-project contract. Public APIs and implementation choices
must remain traceable to the Spec; open decisions must not be treated as
accepted design.

## License

GNU GPL v3 or later — see [LICENSE](LICENSE). Third-party dependencies retain
their own licenses; see [NOTICE](NOTICE).
