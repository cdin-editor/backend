# Confined Runtime extension source resolver

The Backend may provide Runtime's optional `module_resolver(plugin_name, module_name)`
callback when the embedding application explicitly configures
`cdin_backend_config.extension_source_root`. The resolver is disabled by
default. A null source root does not expose a resolver to Runtime.

## Trusted source layout

The configured root is an administrator-controlled directory. Every extension
is stored in one direct child directory named by its Runtime namespace:

```text
extensions/
  sample-tool/
    init.lua
    helpers/
      greeting.lua
```

Runtime asks Backend for the entry module `init`. A nested call such as
`require("helpers.greeting")` maps to
`extensions/sample-tool/helpers/greeting.lua`. Module identifiers are dotted
ASCII names; separators, empty segments, absolute paths, drive/UNC syntax and
traversal are rejected. The resolver returns exact file bytes and a virtual
chunk name such as `extension://sample-tool/helpers.greeting`; it never sends
a host filesystem path to Lua diagnostics.

The root directory itself may be configured through a symlink, but Backend
canonicalizes it once during Runtime startup and uses that resolved root as
the trust boundary. Descendant symlinks and Windows reparse points are
rejected. POSIX traversal is descriptor-relative with `openat()`,
`O_NOFOLLOW`, and regular-file checks. Windows rejects reparse points and
checks the final path of each opened handle against the canonical root. Files
with multiple hard links are rejected.

Directory entries are checked for case-fold ambiguity. A request must match
the exact spelling, and a directory containing two names that compare equal
under ASCII case folding is rejected instead of selecting an arbitrary file.

## Configuration and limits

`cdin_backend_config_init()` supplies these defaults:

| Field | Default | Accepted range |
| --- | ---: | ---: |
| `extension_source_root` | `NULL` (disabled) | Non-empty UTF-8 directory when enabled |
| `max_extension_source_bytes` | 256 KiB | 1 byte–1 MiB |
| `max_extension_modules` | 128 | 1–256 successful source reads |

The product of per-module bytes and module count may not exceed 64 MiB. The
module count and aggregate byte budget are enforced for the lifetime of one
Backend Runtime instance, including after an extension unload/reload. Failed
and missing module lookups do not consume a successful-read slot. Empty source
files are allowed but still consume a module slot.

```c
cdin_backend_config config;
cdin_backend_config_init(&config);
config.extension_source_root = "/srv/cdin/extensions";
config.max_extension_source_bytes = 256u * 1024u;
config.max_extension_modules = 128u;
```

The application owns the configured directory's trust and update policy.
Do not place untrusted or user-writable content beneath this root. The Lua
sandbox is defense in depth, not OS process isolation.

## Runtime error behavior

- Missing extension directories and modules return `nil` to Runtime. Runtime
  reports a structured `NOT_FOUND` result for the entry module.
- Invalid identifiers, traversal, symlink/reparse points, non-regular files,
  hard-linked files and case-fold ambiguity raise `PERMISSION_DENIED`.
- Per-file, module-count and aggregate-source limits raise `LIMIT_EXCEEDED`.
- Allocation failures raise `NO_MEMORY`; other filesystem failures raise
  `IO_ERROR`.

Resolver callbacks run only on the Backend's Runtime owner thread. The
callback is not a Frontend Protocol command and does not grant a remote Driver
permission to load local code. An application exposing extension management
must provide a separate local administrative authorization boundary.


## Trusted application lifecycle API

The C embedding API exposes the Runtime's existing local extension lifecycle
without adding a wire command:

- `cdin_backend_extension_load(backend, name)` resolves and loads one named
  extension through the configured root. It returns only a Backend status;
  module tables, source bytes and sandbox objects remain private to Runtime.
- `cdin_backend_extension_unload(backend, name)` invokes the extension's
  explicit unload hook and removes its owner-scoped registrations.
- Both calls are synchronous, require a running Backend, and must execute on
  the Backend/Runtime owner thread. They return `INVALID_STATE` from another
  thread or outside the running lifecycle.
- Loading returns `UNSUPPORTED` unless Lua is active and
  `extension_source_root` was explicitly configured. A missing entry returns
  `NOT_FOUND`; a failed extension initialization is returned as an internal
  failure after Runtime has rolled back its registrations.
- Unloading an unknown name returns `NOT_FOUND`.

These APIs are an application/embedding boundary, not a remote command
surface. A CDIN CLI, bundle manager or other local administrative interface
may call them after applying its own authorization policy. The Backend does
not automatically discover or load every directory below the extension root.
An empty root therefore remains an empty extension set unless the trusted
application explicitly requests a name.

## Verification

The Backend integration scenario exercises exact source bytes (including NUL
and non-UTF-8 bytes), nested modules, missing modules, invalid identifiers,
per-file and module-count limits, symlink denial where the platform permits
creating a symlink, and POSIX case-fold collisions. The same scenario is
compiled into the CMake integration suite and the Lua-enabled Make integration
target. Platform-specific symlink creation may be unavailable on locked-down
Windows runners; in that case the reparse-point rejection is implemented but
that particular test path reports as skipped by the test's capability check.
