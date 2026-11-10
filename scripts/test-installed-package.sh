#!/usr/bin/env bash
# Install Backend into a temporary prefix and link a consumer from a directory
# that is not part of the Backend source tree.
set -euo pipefail

stage_windows_runtime_dlls() {
  local consumer_directory="$1"
  local lua_prefix="$2"
  local executable_name="$3"
  local current dependency candidate destination root
  local index=0
  local -a roots=(
    "$lua_prefix/bin" "$lua_prefix/lib"
    "$kernel_prefix/bin" "$kernel_prefix/lib"
    "$logger_prefix/bin" "$logger_prefix/lib"
  )
  local -a queue=("$consumer_directory/$executable_name")
  local -A visited=()
  command -v objdump >/dev/null 2>&1 || {
    echo "objdump is required to resolve Windows consumer runtime imports." >&2
    return 1
  }
  while (( index < ${#queue[@]} )); do
    current="${queue[$index]}"
    index=$((index + 1))
    [[ -f "$current" ]] || {
      echo "Missing PE image while resolving runtime imports: $current" >&2
      return 1
    }
    while IFS= read -r dependency; do
      dependency="${dependency//$'\r'/}"
      [[ -n "$dependency" ]] || continue
      dependency_key="${dependency,,}"
      [[ -z "${visited[$dependency_key]+x}" ]] || continue
      visited["$dependency_key"]=1

      # Windows API-set contracts are provided by the OS loader and do not
      # correspond to standalone redistributable DLL files.
      if [[ "$dependency_key" == api-ms-win-* || "$dependency_key" == ext-ms-win-* ]]; then
        continue
      fi

      candidate=""
      for root in "${roots[@]}"; do
        [[ -d "$root" ]] || continue
        candidate="$(find "$root" -type f -iname "$dependency" -print -quit 2>/dev/null || true)"
        [[ -z "$candidate" ]] || break
      done
      if [[ -z "$candidate" ]]; then
        # System DLLs are resolved by Windows itself. Any other unresolved
        # import is a packaging/runtime dependency error, not a silent skip.
        if command -v where.exe >/dev/null 2>&1 && where.exe "$dependency" >/dev/null 2>&1; then
          continue
        fi
        echo "Could not resolve imported Windows runtime DLL '$dependency' for $current" >&2
        return 1
      fi

      destination="$consumer_directory/${candidate##*/}"
      if [[ -e "$destination" ]]; then
        if ! sha256sum "$candidate" "$destination" | awk 'NR == 1 { first=$1 } NR == 2 { exit(first != $1) }'; then
          echo "Conflicting Windows runtime DLL basename: ${candidate##*/}" >&2
          return 1
        fi
      else
        cp -p "$candidate" "$destination"
      fi
      queue+=("$destination")
    done < <(objdump -p "$current" | sed -n 's/^[[:space:]]*DLL Name: //p')
  done
  echo "Resolved ${#visited[@]} Windows PE imports for $executable_name."
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
if ! pkg-config --exists lua5.4; then
  echo "SKIP installed Backend package consumer: Lua 5.4 development package is not available on this runner."
  exit 0
fi
kernel_prefix="${KERNEL_PREFIX:-$repo_root/.deps/kernel}"
logger_prefix="${LOGGER_PREFIX:-$repo_root/.deps/logger}"
stage="$repo_root/build/package-consumer-prefix"
source_build="$repo_root/build/package-consumer-source"
consumer_build="$repo_root/build/package-consumer-external"
cmake_source_build="$source_build"
cmake_consumer_build="$consumer_build"

cmake_kernel_prefix="$kernel_prefix"
cmake_logger_prefix="$logger_prefix"
cmake_stage="$stage"
if command -v cygpath >/dev/null 2>&1; then
  kernel_pc_dir="$(cygpath -m "$kernel_prefix/lib/pkgconfig")"
  logger_pc_dir="$(cygpath -m "$logger_prefix/lib/pkgconfig")"
  cmake_kernel_prefix="$(cygpath -m "$kernel_prefix")"
  cmake_logger_prefix="$(cygpath -m "$logger_prefix")"
  cmake_stage="$(cygpath -m "$stage")"
  cmake_source_build="$(cygpath -m "$source_build")"
  cmake_consumer_build="$(cygpath -m "$consumer_build")"
  pkgconfig_separator=';'
else
  kernel_pc_dir="$kernel_prefix/lib/pkgconfig"
  logger_pc_dir="$logger_prefix/lib/pkgconfig"
  pkgconfig_separator=':'
fi
[[ -f "$kernel_prefix/lib/pkgconfig/cdin-kernel.pc" ]] || {
  echo "Expected pinned Kernel pkg-config file is missing under $kernel_prefix" >&2
  exit 1
}
[[ -f "$logger_prefix/lib/pkgconfig/m-mdy-m-log.pc" ]] || {
  echo "Expected independent Logger pkg-config file is missing under $logger_prefix" >&2
  exit 1
}
cmake_prefix="$cmake_kernel_prefix;$cmake_logger_prefix"
export KERNEL_PREFIX="$kernel_prefix" LOGGER_PREFIX="$logger_prefix"
export PKG_CONFIG_PATH="$logger_pc_dir$pkgconfig_separator$kernel_pc_dir"
case "$(uname -s)" in
  Darwin)
    lua_prefix="$(brew --prefix lua@5.4)"
    export PATH="$lua_prefix/bin:$PATH"
    export PKG_CONFIG_PATH="$lua_prefix/lib/pkgconfig$pkgconfig_separator$PKG_CONFIG_PATH"
    cmake_prefix="$cmake_prefix;$lua_prefix"
    ;;
esac
pkg-config --modversion cdin-kernel
test "$(pkg-config --modversion m-mdy-m-log)" = "0.1.0"
pkg-config --modversion lua5.4

rm -rf "$stage" "$source_build" "$consumer_build"
cmake -S "$repo_root" -B "$cmake_source_build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$cmake_stage" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_INCLUDEDIR=include \
  -DCMAKE_PREFIX_PATH="$cmake_prefix" \
  -DCDIN_BUILD_BACKEND=ON \
  -DCDIN_WITH_LUA=ON \
  -DCDIN_BUILD_INTEGRATION_TESTS=OFF \
  -DCDIN_BUILD_EXAMPLES=OFF \
  -DCDIN_BUILD_FUZZERS=OFF \
  -DBUILD_TESTING=OFF
cmake --build "$cmake_source_build" --config Release --parallel
cmake --install "$cmake_source_build" --config Release

if command -v cygpath >/dev/null 2>&1; then
  stage_pc_dir="$(cygpath -m "$stage/lib/pkgconfig")"
  PKG_CONFIG_PATH="$stage_pc_dir;$logger_pc_dir;$kernel_pc_dir" pkg-config --static --libs cdin-backend
else
  PKG_CONFIG_PATH="$stage/lib/pkgconfig:$logger_pc_dir:$kernel_pc_dir" pkg-config --static --libs cdin-backend
fi
cmake -S "$repo_root/tests/package-consumer" -B "$cmake_consumer_build" \
  -DCMAKE_PREFIX_PATH="$cmake_stage;$cmake_prefix" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$cmake_consumer_build" --config Release --parallel
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    lua_prefix="$(pkg-config --variable=prefix lua5.4)"
    stage_windows_runtime_dlls "$consumer_build" "$lua_prefix" "cdin_backend_external_consumer.exe"
    for runtime_dir in "$lua_prefix/bin" "$kernel_prefix/bin" "$kernel_prefix/lib" "$logger_prefix/bin" "$logger_prefix/lib"; do
      if [[ -d "$runtime_dir" ]]; then PATH="$runtime_dir:$PATH"; fi
    done
    export PATH
    consumer_exe="$consumer_build/cdin_backend_external_consumer.exe"
    if command -v objdump >/dev/null 2>&1; then
      objdump -p "$consumer_exe" | sed -n 's/^[[:space:]]*DLL Name: /consumer dependency: /p'
    fi
    ;;
esac
ctest --test-dir "$cmake_consumer_build" --output-on-failure
