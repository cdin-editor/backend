#!/usr/bin/env bash
set -euo pipefail

if (($# == 0)); then
  echo "usage: run-with-kernel-libs.sh PROGRAM [ARG ...]" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
kernel_prefix="${KERNEL_PREFIX:-$repo_root/.deps/kernel}"
if [[ ! -d "$kernel_prefix/lib" ]]; then
  echo "Kernel library directory does not exist: $kernel_prefix/lib" >&2
  echo "Run 'bash scripts/install-deps.sh' before executing integration tests." >&2
  exit 2
fi

case "$(uname -s 2>/dev/null || printf unknown)" in
  Linux*)
    export LD_LIBRARY_PATH="$kernel_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    ;;
  Darwin*)
    export DYLD_LIBRARY_PATH="$kernel_prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    # The MSYS runtime converts POSIX path entries for native Windows child
    # processes, allowing the Windows loader to locate cdin_kernel.dll.
    export PATH="$kernel_prefix/bin:$PATH"
    ;;
  *)
    echo "unsupported host for Kernel runtime loading: $(uname -s 2>/dev/null || echo unknown)" >&2
    exit 2
    ;;
esac

exec "$@"
