#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

cc_bin="${CC:-cc}"
build_dir="${BUILD_DIR:-$repo_root/build/stress}"
mkdir -p "$build_dir"

# Build the command incrementally. macOS ships Bash 3.2, where expanding an
# empty array under `set -u` can fail with "unbound variable". Never expand
# optional arrays unless their source string contains at least one word.
cmd=("$cc_bin")
case "${OSTYPE:-unknown}" in
  msys*|cygwin*|win32*) ;;
  *) cmd+=("-D_POSIX_C_SOURCE=200809L") ;;
esac

cmd+=(
  -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes
  -Wformat=2 -Wpedantic -Werror
)

append_words() {
  local value="$1"
  local -a parsed

  # Bash 3.2-compatible guard: read and expand only non-empty word lists.
  if [[ "$value" =~ [^[:space:]] ]]; then
    read -r -a parsed <<< "$value"
    cmd+=("${parsed[@]}")
  fi
}

append_words "${EXTRA_CFLAGS:-}"
cmd+=(
  -Iinclude -Isrc
  tests/stress/resource_stress.c
  src/jobs/pool.c src/jobs/public.c src/platform/thread.c
  -o "$build_dir/resource_stress"
)
append_words "${EXTRA_LDFLAGS:-}"
append_words "${THREAD_LIBS:--pthread}"
append_words "${MEMORY_LIBS:-}"

"${cmd[@]}"
"$build_dir/resource_stress" "$@"
