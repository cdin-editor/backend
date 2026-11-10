#!/usr/bin/env bash
# Install the target-native toolchain required by the tagged binary package
# build, then run the same full Backend gate used by the normal CI workflow.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

case "${RUNNER_OS:-$(uname -s)}" in
  Linux)
    sudo apt-get update
    sudo apt-get install -y build-essential cmake pkg-config curl tar gzip zip liblua5.4-dev
    ;;
  macOS|Darwin)
    command -v brew >/dev/null 2>&1 || {
      echo "Homebrew is required on the macOS release runners." >&2
      exit 2
    }
    brew install cmake pkg-config lua@5.4
    export PATH="$(brew --prefix lua@5.4)/bin:$PATH"
    export PKG_CONFIG_PATH="$(brew --prefix lua@5.4)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    ;;
  Windows|Windows_NT|MINGW*|MSYS*)
    [[ "${MSYSTEM:-}" == UCRT64 ]] || {
      echo "Windows release packages must be built in the MSYS2 UCRT64 environment." >&2
      exit 2
    }
    ;;
esac

# The reusable release workflow starts from a clean checkout. Provision the
# exact independent Kernel and logger SDK pins before running Backend tests.
kernel_prefix="$repo_root/.deps/kernel"
logger_prefix="$repo_root/.deps/logger"
bash scripts/install-deps.sh --prefix "$kernel_prefix" --logger-prefix "$logger_prefix"
export KERNEL_PREFIX="$kernel_prefix" LOGGER_PREFIX="$logger_prefix"
if command -v cygpath >/dev/null 2>&1; then
  pkgconfig_separator=';'
  kernel_pc_dir="$(cygpath -m "$kernel_prefix/lib/pkgconfig")"
  logger_pc_dir="$(cygpath -m "$logger_prefix/lib/pkgconfig")"
else
  pkgconfig_separator=':'
  kernel_pc_dir="$kernel_prefix/lib/pkgconfig"
  logger_pc_dir="$logger_prefix/lib/pkgconfig"
fi
export PKG_CONFIG_PATH="$logger_pc_dir$pkgconfig_separator$kernel_pc_dir${PKG_CONFIG_PATH:+$pkgconfig_separator$PKG_CONFIG_PATH}"
cmake --version
pkg-config --modversion cdin-kernel
pkg-config --modversion m-mdy-m-log
pkg-config --modversion lua5.4
bash scripts/ci-check.sh
