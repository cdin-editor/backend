# CI failure analysis: run 41406922640

This report records the five failures from the GitHub Actions run created for
commit `c150d690fa179dbaa2cab96be41ec04ca8db746c` on November 2, 2026. The
failure logs were inspected per job, including step summaries and raw logs.

## Summary

| Job | Failed step | Root cause | Correction |
| --- | --- | --- | --- |
| `125150950695` — quality | Run the Pipeline `check` task | The integration executable linked to the Kernel shared library, but the operating-system loader could not find `libcdin_kernel.so.1` at runtime. | Run integration executables with the installed Kernel library directory on the platform loader path. Add CMake build RPATH on Unix and copy the Kernel DLL beside Windows integration executables. |
| `125150950608` — docs | Lint Markdown | `docs/architecture.md` contained a 931-character prose line; MD013 allows at most 500 characters in this repository. | Rewrite the long paragraph as short prose and a focused list. Keep long table rows exempt from prose line wrapping. |
| `125150950095` — Linux | Strict build and tests | The `require-kernel` check could not consistently resolve the package installed by the preceding step. The installer and nested Make process relied on implicit prefix and `PKG_CONFIG_PATH` defaults rather than one explicitly exported dependency prefix. | Make `check` an orchestration recipe. Install/verify Kernel first, then start a fresh Make process for Backend build and integration. |
| `125150950455` — macOS | Strict build and tests | Same package-prefix visibility problem as Linux. The four Kernel-independent E2E scenarios passed before the nested Make process failed to resolve the installed package. | Same ordered dependency installation and fresh Make process as Linux. The runtime helper also supplies `DYLD_LIBRARY_PATH` for integration executables. |
| `125150950453` — Windows UCRT64 | Strict build and tests | The workflow installed `mingw-w64-ucrt-x86_64-make`, which provides `mingw32-make`; the shell command `make` was still unavailable in the MSYS environment. | Install the MSYS `make` package as well as the UCRT64 toolchain package. Keep Make/CMake commands inside the UCRT64 MSYS shell. |

## Details and design notes

### Runtime library discovery

`pkg-config --static --libs cdin-kernel` supplies link flags. It does not
force the final executable to use only static archives. In the failing Linux
job, the integration executable therefore needed `libcdin_kernel.so.1` when it
started. The linker succeeded, but the dynamic loader failed before the test
could run.

`scripts/run-with-kernel-libs.sh` now supplies the correct runtime search path
for Linux, macOS and MSYS/Windows. CMake integration targets receive a build
RPATH on Unix-like systems. On Windows, the Kernel DLL is copied beside each
integration executable, which is the normal Windows loader search location.

### Dependency installation order

Make expands `$(shell pkg-config ...)` while parsing `Makefile`. Installing a
package later in the same Make invocation cannot change that earlier result.
`make check` therefore delegates to `scripts/ci-check.sh`; that script runs the
Kernel-independent E2E scenarios, installs and verifies the pinned Kernel
package, and only then starts a new Make process for the Backend and
integration targets. `KERNEL_PREFIX` is exported so the installer, Make and
runtime loader all refer to the same installation.

### Windows tool names

The UCRT64 package `mingw-w64-ucrt-x86_64-make` supplies the MinGW-prefixed
make executable. The workflow also installs the MSYS `make` package so the
repository's portable `make` command works inside the MSYS2 shell. Both
packages are intentional: one is part of the UCRT toolchain and the other
provides the shell-level command used by the build scripts.

## Validation performed for the corrected source

- GCC strict build and all four E2E plus both Kernel integration workflows
  passed against a local package fixture built from the Kernel source and
  exposing the expected `pkg-config` metadata and shared-library layout.
- Clang 17 strict build and the same E2E and integration workflows passed.
- Clang 17 CMake build completed, and CTest passed all 6 scenarios.
- The CMake integration tests passed with `LD_LIBRARY_PATH` explicitly unset,
  confirming that their build RPATH locates the Kernel shared library.
- The corrected architecture prose has no non-table line longer than the
  configured 500-character MD013 limit.
- The full `markdownlint-cli2` executable was not available in the local
  editing environment. The actual Markdownlint action remains enabled in the
  repository's documentation workflow and must pass on GitHub Actions.
- Windows was not available as a local execution host. Its workflow now
  explicitly installs the missing MSYS `make` package and retains the native
  UCRT64 build and CTest steps for remote verification.

The original CI run is evidence of the failures above; it is not a validation
of the corrected source. A new GitHub Actions run is required after this
source archive is pushed to the repository.
