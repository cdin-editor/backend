#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

runtime_root="${CDIN_RUNTIME_ROOT:-$repo_root/runtime/lua}"
if [[ ! -f "$runtime_root/cdin/runtime.lua" && -f "$repo_root/../runtime/lua/cdin/runtime.lua" ]]; then
  runtime_root="$repo_root/../runtime/lua"
fi
if [[ ! -f "$runtime_root/cdin/runtime.lua" ]]; then
  echo "Runtime source not found: $runtime_root/cdin/runtime.lua" >&2
  echo "Check out cdin-editor/runtime into ./runtime or set CDIN_RUNTIME_ROOT." >&2
  exit 2
fi

kernel_pc_dir="${KERNEL_PREFIX:-$repo_root/.deps/kernel}/lib/pkgconfig"
if command -v cygpath >/dev/null 2>&1; then
  kernel_pc_dir="$(cygpath -m "$kernel_pc_dir")"
fi
export PKG_CONFIG_PATH="$kernel_pc_dir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

for package in cdin-kernel lua5.4; do
  if ! pkg-config --exists "$package"; then
    echo "Missing pkg-config dependency: $package" >&2
    exit 2
  fi
done

make WITH_LUA=1 all
mkdir -p build/integration
read -r -a cflags <<< "$(pkg-config --cflags cdin-kernel lua5.4)"
read -r -a kernel_libs <<< "$(pkg-config --static --libs cdin-kernel)"
read -r -a lua_libs <<< "$(pkg-config --libs lua5.4)"
read -r -a thread_libs <<< "${THREAD_LIBS:--pthread}"

"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion \
  -Wstrict-prototypes -Wformat=2 -Wpedantic -Werror \
  -Iinclude -Isrc "${cflags[@]}" \
  tests/integration/runtime_host_smoke.c \
  build/libcdin_backend.a build/libcdin_backend_services.a build/libcdin_protocol.a \
  "${kernel_libs[@]}" "${lua_libs[@]}" "${thread_libs[@]}" \
  -o build/integration/runtime_host_smoke

export CDIN_RUNTIME_ROOT="$runtime_root"
bash scripts/run-with-kernel-libs.sh build/integration/runtime_host_smoke
