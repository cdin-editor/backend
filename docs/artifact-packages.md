# Backend binary packages

This guide describes immutable install-prefix artifacts produced by the
Backend release workflow. A package is a native development dependency for
a product Composition Root; it is not a runnable editor or application installer.

## Package contents

Each target archive is a relocatable CMake install prefix containing:

- Public Backend C headers.
- libcdin_backend.a, libcdin_backend_services.a and libcdin_protocol.a.
- CMake config/version/export files with the targets cdin::backend,
  cdin::backend_services and cdin::protocol.
- Relocatable pkg-config files for the same libraries.
- share/cdin-backend/BUILD-INFO.txt with package version, source revision,
  target OS/architecture/ABI, runner and dependency versions.

The archive contains only Backend-owned files. It does not bundle Kernel,
m-mdy-m/log.c, Lua, Runtime, SDL or Registry. It has no install hooks or
network scripts; Registry verifies and stages it as data.

## Supported native targets

| OS | Architecture | ABI | Release runner |
|---|---|---|---|
| Linux | x86_64 | gnu-glibc-2.39 | Ubuntu 24.04 |
| Linux | arm64 | gnu-glibc-2.39 | Ubuntu 24.04 ARM |
| macOS | x86_64 | macos-15 | macOS 15 Intel |
| macOS | arm64 | macos-15 | macOS 15 Apple Silicon |
| Windows | x86_64 | windows-ucrt64 | MSYS2 UCRT64 |

ABI matching is exact. A UCRT64 archive is not MSVC-compatible; a Linux
package built against glibc 2.39 must not be used on an older glibc baseline.

## External dependencies

The package exports its static link closure. CMake's cdinBackendConfig.cmake
requires:

- cdin-kernel >=1.2.1 for the Kernel ABI and host implementation.
- m-mdy-m-log >=0.1.0 as a separately versioned logger C SDK.
- PUC Lua 5.4 development headers and library.

The cdin-backend.pc file declares Kernel, logger, Lua, Protocol and reusable
Backend services as private link requirements. Backend does not create a
second logger or compile log.c directly; native diagnostics use the Kernel
logger API, while the logger ABI remains an independently versioned SDK.

The Composition Root provisions these dependencies in a controlled prefix.
Kernel ABI, logger C API and Lua are separate target-environment contracts
declared by the Product Definition: org.cdin.kernel-abi@1.1.0,
org.m-mdy-m.log-c-api@0.1.0 and org.lua.puc-lua-c-api@5.4.0. The Composition
Root provisions the exact pinned Kernel binary package, the independent logger
SDK and PUC Lua 5.4; the Registry engine does not collect or build these
environment dependencies.

## CMake consumer

After extracting one target archive into BACKEND_PREFIX and provisioning the
matching Kernel package, the separately pinned m-mdy-m/log.c SDK and Lua 5.4 package:

~~~cmake
cmake_minimum_required(VERSION 3.22)
project(my_cdin_host LANGUAGES C)

find_package(cdinBackend 1.2.1 EXACT CONFIG REQUIRED)

add_executable(my_cdin_host main.c)
target_link_libraries(my_cdin_host PRIVATE cdin::backend)
~~~

Configure with the package and environment prefixes:

~~~sh
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="$BACKEND_PREFIX;$KERNEL_PREFIX;$LUA_PREFIX"
cmake --build build --config Release
~~~

The package config resolves Threads, PkgConfig, the Kernel static link
closure, the independent logger SDK and Lua 5.4. Missing external packages fail during configuration.

## pkg-config consumer

For static linking, request the private dependency closure explicitly:

~~~sh
export PKG_CONFIG_PATH="$BACKEND_PREFIX/lib/pkgconfig:$KERNEL_PREFIX/lib/pkgconfig:$LUA_PREFIX/lib/pkgconfig"
cc app.c $(pkg-config --cflags --static cdin-backend) \
  $(pkg-config --libs --static cdin-backend) -o app
~~~

The independent services and codec libraries can also be consumed without
the Kernel-hosting Backend target through cdin-backend-services and
cdin-protocol.

## Release identity and Registry metadata

The existing Backend 1.2.0 source-only metadata and Catalog entry remain
immutable. The 1.2.1 release creates five native assets and a source archive.
Each native build writes a sibling *.artifact.json descriptor with its real
asset URI, digest, byte count, target tuple and build provenance.

Only after every release asset has been published should the exact descriptors,
archives and checksum sidecars be passed to scripts/generate-release-metadata.py.
The generated 1.1.x Component Release Metadata is validated and added as a new
Catalog identity. It removes the Kernel component edge while retaining Kernel
ABI, logger C API and Lua 5.4 as separate environment contracts; Runtime
remains a component dependency. See registry/README.md.

Registry 1.3.0 verifies and extracts the archive without running package
scripts. CDIN owns dependency provisioning, linking, product tests and launch.
Artifact delivery alone does not prove live product integration or production
GUI readiness.
