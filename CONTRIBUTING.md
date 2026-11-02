# Contributing to CDIN Backend

Thanks for helping improve the CDIN Backend. A small correction, a focused test,
a clearer explanation, or a portability fix is as valuable as a large feature.

The Backend is a standalone C11 library. It is not a subdirectory of the CDIN
application repository and must not include a sibling Kernel source checkout.
Its Kernel dependency is installed as a versioned binary package; see
[Dependencies](docs/dependencies.md).

## Before Starting

For a typo or a self-contained defect, a pull request is welcome. For a new
feature, a public API change, or a change that affects another CDIN component,
open an issue first so the contract and ownership can be discussed.

Read the [CDIN Specification](https://github.com/cdin-editor/spec) before
changing cross-project behavior. It is the source of truth for component
boundaries and shared contracts. A local implementation must not silently
promote a `PROPOSED` or `OPEN` requirement into an accepted contract.

## What Belongs in Backend?

- Session identity, document/view relationships, and lifecycle orchestration
  belong here; document bytes and undo history belong to Kernel.
- The Backend is the only direct client of the Kernel C ABI. Put all Kernel
  calls in `src/kernel/adapter.c` and keep Kernel structs out of public headers.
- Lua state belongs to the Backend owner thread. Worker threads return plain C
  results and never call Lua.
- Files, paths, processes, clocks and jobs belong behind the OS/platform APIs.
- The Frontend owns pixels, terminal cells, wrapping, shaping and drawing.
- The Frontend Protocol implementation must remain transport-independent until
  the Spec selects a serialized encoding.

When a change crosses one of these boundaries, explain why in the pull request
and add a traceability note to `docs/spec-traceability.md`.

## Set Up a Development Environment

You need Git, a C11 compiler, GNU Make or CMake, `curl`, `pkg-config`, and the
usual archive tools. The pinned Kernel package is downloaded and checksum-
verified by the repository script:

```sh
git clone https://github.com/cdin-editor/backend.git
cd backend
make deps
make check
```

The downloaded headers and libraries live under `.deps/kernel/`, which is
ignored by Git. No Kernel source checkout is required. Lua 5.4 development
headers are optional for portable end-to-end scenarios; they are
required to build and run the native Lua Host bridge.

## Making a Change

1. Start a feature or bugfix branch using the repository's [gix flow](.gix/README.md).
2. Keep each module focused and put shared behavior in a reusable internal
   module rather than copying logic between callers.
3. Add tests for success, invalid input, resource limits and shutdown paths.
4. Run `pipeline run check` or the equivalent `make check` gate.
5. Add comments that explain non-obvious ownership, ordering and safety choices.
   Link comments to the exact Spec requirement or decision when one applies.
6. Update beginner and developer documentation when behavior or setup changes.
7. Update `CHANGELOG.md` when the change is intended for a release.

## Coding Rules

- Compile as C11 and keep strict warnings enabled. Fix warnings instead of
  weakening the warning flags.
- Use the public Kernel package through `pkg-config`; do not add relative paths
  into another repository or vendor Kernel source.
- Do not include `cdin/kernel.h` outside `src/kernel/adapter.c`.
- Do not add a second logger. Route Backend diagnostics through the Kernel log
  API, which owns the bundled `m-mdy-m/log.c` implementation.
- Do not pass command strings to a shell. Process APIs accept argument arrays.
- Bound every queue and buffer, and define what happens when the bound is hit.
- Do not let workers access Lua, Session state or the Kernel directly.
- Preserve UTF-8 path boundaries and document-byte fidelity.

## Git Flow and Commits

Initialize the repository with `bash scripts/init-git-flow.sh` after the first
commit. Use `gix feature start <name>` or `gix bugfix start <name>` for work.
Follow the commit conventions used by the project, for example
`feat(protocol): add queue coalescing test` or `docs(dependencies): explain package installation`.

## Pull Requests

A pull request should explain the problem, the chosen boundary, the Spec IDs it
implements, the tests run, and any platform or dependency checks that could not
be completed. Do not describe a compile-only check as an integration test.
