# Getting started

This page walks through the smallest useful Backend program. It assumes that you have not used the Backend before.

## 1. Install the Kernel package

Backend uses `cdin-kernel` as a real external library dependency. It does not
read Kernel headers from a sibling folder. From the repository root, install
the pinned package first:

```sh
make deps
```

This downloads the platform package, verifies its SHA-256 checksum, and installs
the header and static libraries under `.deps/kernel/`. See
[Dependencies](dependencies.md) for package versions, supported platforms and
`pkg-config` details.

## 2. Understand the two stages

Creating a Backend object and starting it are different operations:

- `create` checks that the Kernel ABI is compatible and allocates the shared Kernel logger.
- `start` creates Session metadata and the worker pool, initializes the optional Lua Runtime, and only then starts the Frontend Protocol. A Frontend cannot attach to a half-initialized Runtime.

This separation makes startup failures easier to report and lets the caller inspect configuration before starting the Runtime.

## 3. Prepare a configuration

Always begin with `cdin_backend_config_init()`. The configuration struct has a `struct_size` field so newer releases can add fields without breaking older callers.

```c
#include <stdio.h>
#include "cdin/backend.h"

int main(void)
{
    cdin_backend_config config;
    cdin_backend *backend = NULL;
    cdin_backend_status status;

    cdin_backend_config_init(&config);
    config.enable_lua = 0; /* set to 1 when built with Lua 5.4 */
    config.enable_headless_protocol = 1;

    status = cdin_backend_create(&config, &backend);
    if (status != CDIN_BACKEND_OK) {
        fprintf(stderr, "Backend create failed: %s\n", cdin_backend_status_name(status));
        return 1;
    }

    status = cdin_backend_start(backend);
    if (status != CDIN_BACKEND_OK) {
        fprintf(stderr, "Backend start failed: %s\n", cdin_backend_status_name(status));
        cdin_backend_destroy(backend);
        return 1;
    }

    /* Run your application's event loop here. */
    status = cdin_backend_shutdown(backend, 0);
    if (status != CDIN_BACKEND_OK) {
        fprintf(stderr, "Backend shutdown reported: %s\n", cdin_backend_status_name(status));
    }
    cdin_backend_destroy(backend);
    return status == CDIN_BACKEND_OK ? 0 : 1;
}
```

This example disables Lua so that it can also run in a build that has no Lua development package. A real editor normally enables Lua 5.4 and provides the Runtime module path.

The log threshold can be selected with the public `cdin_backend_log_level` values:

| Value | Meaning |
|---|---|
| `CDIN_BACKEND_LOG_ERROR` | Errors only and more severe messages |
| `CDIN_BACKEND_LOG_WARN` | Warnings and more severe messages |
| `CDIN_BACKEND_LOG_INFO` | Informational messages (the default) |
| `CDIN_BACKEND_LOG_DEBUG` | Debug messages |
| `CDIN_BACKEND_LOG_TRACE` | Trace messages |
| `CDIN_BACKEND_LOG_FATAL` | Fatal messages only |

These are Backend API constants. The Kernel adapter maps them to the Kernel logger's public enum; application code does not need to include `cdin/kernel.h` just to configure logging.

## 4. Link the libraries

The Backend is a static library. Static archives contain object files, not their
external libraries; the final executable supplies those dependencies. Build the
Backend first:

```sh
make all
```

Then link your application with the Backend archive and the Kernel package's
static requirements:

```sh
cc -Iinclude app.c -Lbuild -lcdin_backend \
  $(pkg-config --static --libs cdin-kernel) -o app
```

`pkg-config --static` includes `m-mdy-m/log.c` because Kernel declares it as a
private static dependency. Do not guess paths or link the logger separately.
If the Backend was compiled with `WITH_LUA=1`, add the Lua 5.4 link flags after
the Backend archive as well:

```sh
$(pkg-config --libs lua5.4)
```

## 5. Pump the Backend from the owner thread

When the Backend is running, call `cdin_backend_pump()` from the same thread that created the instance. For a GUI application, call it from the application's main event loop. For a command-line host, call it from the main loop.

```c
size_t processed = 0;
cdin_backend_status status = cdin_backend_pump(backend, 32u, &processed);
if (status != CDIN_BACKEND_OK) {
    /* Report the error and decide whether the application should stop. */
}
```

The `32` is a work budget, not a required constant. Keeping the budget bounded prevents the Backend from monopolizing the UI thread when many worker jobs finish at once.

## 6. Shut down cleanly

Call `cdin_backend_shutdown(backend, 0)` for graceful shutdown. The Backend stops accepting new jobs, joins workers, drains accepted completions, stops Lua Runtime, and releases Kernel resources. Pass a non-zero `abort_pending` value when the application must cancel pending work instead of draining it.

Call `cdin_backend_destroy()` after shutdown. Destroy is also a cleanup path for partially initialized instances. Do not call lifecycle functions from a different thread.

## 7. Common first-run problems

| Message or result | What it usually means | What to check |
|---|---|---|
| `VERSION_MISMATCH` | The Kernel ABI is too old or has another major version. | Use a Kernel library that reports ABI major 1 and at least the minor version required by this Backend build. |
| `UNSUPPORTED` during create | A required Kernel feature is missing. | Check the Kernel feature mask and the linked library. |
| `UNSUPPORTED` during start | This build has no Lua 5.4 bridge, but Lua is enabled. | Install Lua 5.4 development headers and rebuild with `WITH_LUA=1`, or disable Lua for a headless test. |
| Runtime cannot load `cdin.runtime` | Lua's module search path does not include the Runtime source tree. | Set `runtime_root` to the folder that contains the `cdin/` Lua module directory. |
| No log file appears | The Kernel logger writes to its configured output. | The default Backend logger output is stderr; configure the shared Kernel logger in the owning application if needed. |
