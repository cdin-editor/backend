#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

cc_bin="${CC:-cc}"
build_dir="${BUILD_DIR:-$repo_root/build/stress}"
mkdir -p "$build_dir"

read -r -a extra_cflags <<< "${EXTRA_CFLAGS:-}"
read -r -a extra_ldflags <<< "${EXTRA_LDFLAGS:-}"
platform_cflags=()
case "${OSTYPE:-unknown}" in
  msys*|cygwin*|win32*) ;;
  *) platform_cflags+=("-D_POSIX_C_SOURCE=200809L") ;;
esac

"$cc_bin" \
  "${platform_cflags[@]}" \
  -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes \
  -Wformat=2 -Wpedantic -Werror \
  "${extra_cflags[@]}" \
  -Iinclude -Isrc \
  tests/stress/resource_stress.c \
  src/jobs/pool.c src/jobs/public.c src/platform/thread.c \
  -o "$build_dir/resource_stress" \
  "${extra_ldflags[@]}" \
  ${THREAD_LIBS:--pthread} \
  ${MEMORY_LIBS:-}

"$build_dir/resource_stress" "$@"
