# Dependencies

This guide explains how Backend obtains and links the Kernel library. You do not
need to download or build the Kernel source code to build Backend.

## 1. Why Kernel is a library dependency

The Kernel owns document bytes, versions, edits, snapshots, notifications and
undo/redo history. Backend calls those operations through the Kernel's public C
ABI. That ABI is a library boundary, not a request to copy Kernel source files
into this repository.

Backend therefore consumes a published `cdin-kernel` release package. The
package contains the public header, static library, `pkg-config` metadata and
the Kernel's private logger library. The package version and the C ABI version
are different numbers: this repository currently pins package `1.2.1` and
requires ABI `1.1.0`.

The pin is recorded in [`deps.lock`](../deps.lock). Change it only after checking
the Kernel release notes, ABI macros and the compatibility range required by
`src/kernel/adapter.c`.

## 2. Install the dependency

From the Backend repository root, run:

```sh
make deps
```

The command runs `scripts/install-deps.sh`. The script:

1. Detects the operating system and CPU architecture.
2. Selects the matching release archive from the Kernel repository.
3. Downloads the archive and its `.sha256` sidecar.
4. Checks the archive's SHA-256 before extracting or installing anything.
5. Runs the installer shipped inside the release package.
6. Checks the installed package version and public ABI version.
7. Confirms that the Kernel and logger static libraries are present.

The package is installed under `.deps/kernel/`. This directory is ignored by
Git because it contains downloaded, platform-specific build dependencies. It is
safe to remove and recreate by running `make deps` again.

To force a fresh download, use:

```sh
bash scripts/install-deps.sh --force
```

To install somewhere else, use:

```sh
bash scripts/install-deps.sh --prefix /path/to/cdin-dependencies/kernel
```

Then set `KERNEL_PREFIX` when invoking Make, or set `PKG_CONFIG_PATH` for other
build tools.

## 3. Supported release packages

The installer currently recognizes the release targets published by Kernel:

| Operating system | Architecture | Archive |
| --- | --- | --- |
| Linux | x86-64 | `.tar.gz` |
| Linux | ARM64 | `.tar.gz` |
| macOS | x86-64 | `.tar.gz` |
| macOS | ARM64 | `.tar.gz` |
| Windows (MSYS2 UCRT64) | x86-64 | `.zip` |

Windows ARM64 is not published by the pinned Kernel release. The installer
fails explicitly instead of downloading a package built for a different
architecture.

## 4. How the linker finds Kernel

Kernel installs a `cdin-kernel.pc` file for `pkg-config`. Backend build files ask
for that package instead of hardcoding include and library paths:

```sh
pkg-config --modversion cdin-kernel
pkg-config --cflags cdin-kernel
pkg-config --static --libs cdin-kernel
```

The last command is important for static linking. Kernel declares
`m-mdy-m-log` in `Requires.private`, so the static link flags include the
matching logging library. Backend does not include the logger header, build a
second logger, or guess the path to a logger archive.

On Linux and macOS, Make adds `.deps/kernel/lib/pkgconfig` to
`PKG_CONFIG_PATH`. In MSYS2, it converts that directory to a Windows path before
calling the native `pkgconf` executable.

## 5. Build with Make

```sh
make deps
make all
make integration
```

`make all` creates `build/libcdin_backend.a`. An archive is not a final
executable and does not embed its dependencies. The integration executable
links the Backend source with the exact Kernel and logger libraries resolved by
`pkg-config --static`.

When another application links `libcdin_backend.a`, it must also link the
Kernel dependency. Use the same `pkg-config` metadata rather than copying the
library paths from this guide.

## 6. Build with CMake

CMake 3.22 or newer and `pkg-config` are required. The Make bootstrap installs
the package and CMake uses the installed metadata:

```sh
make deps
PKG_CONFIG_PATH="$PWD/.deps/kernel/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
  cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --parallel
ctest --test-dir build-cmake --output-on-failure
```

The imported CMake target asks `pkg-config` for static requirements. This keeps
Kernel and logger linkage attached to one dependency target rather than adding
manual `-I` or `-L` paths to Backend source files.

## 7. Lua is a separate optional dependency

Lua is not a Kernel dependency. To compile the current Runtime Host bridge, use
PUC Lua 5.4 development headers and library:

```sh
make WITH_LUA=1
```

For CMake, enable `-DCDIN_WITH_LUA=ON`. The default build compiles a small
`UNSUPPORTED` stub when Lua development files are not available. That makes
Kernel and OS service tests possible without pulling Lua into every Backend
consumer.

The bridge mechanism remains a Spec open decision, not a permanent public ABI
choice. Spec ADR-0019 selects the PUC Lua 5.4 C API: https://github.com/cdin-editor/spec/blob/main/docs/adr/ADR-0019-runtime-bridge.md.

## 8. Removing downloaded files

To remove the downloaded package and all local build output:

```sh
make clean
rm -rf .deps
```

The next `make deps` downloads and verifies the pinned package again.
