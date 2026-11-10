#!/usr/bin/env bash
# Verify one downloaded native package and link an independent consumer
# against the extracted prefix without any Backend source-tree paths.
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
test_root="${PIPELINE_ARTIFACT_TEST_DIR:-$repo_root/dist}"
kernel_prefix="${KERNEL_PREFIX:-$repo_root/.deps/kernel}"
logger_prefix="${LOGGER_PREFIX:-$repo_root/.deps/logger}"
archive="$(find "$test_root" -maxdepth 2 -type f -name 'cdin-backend-*.tar.gz' ! -name '*-source.tar.gz' -print | sort | head -n 1)"
[[ -n "$archive" ]] || {
  echo "No cdin-backend target archive found under $test_root" >&2
  exit 1
}
checksum="${archive}.sha256"
[[ -f "$checksum" ]] || {
  echo "Missing checksum sidecar: $checksum" >&2
  exit 1
}
if command -v sha256sum >/dev/null 2>&1; then
  (cd "$(dirname "$archive")" && sha256sum -c "$(basename "$checksum")")
else
  (cd "$(dirname "$archive")" && shasum -a 256 -c "$(basename "$checksum")")
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/package"
tar -xzf "$archive" -C "$work/package"

for required in \
  include/cdin/backend.h \
  lib/libcdin_backend.a \
  lib/libcdin_backend_services.a \
  lib/libcdin_protocol.a \
  lib/cmake/cdinBackend/cdinBackendConfig.cmake \
  lib/pkgconfig/cdin-backend.pc; do
  [[ -f "$work/package/$required" ]] || {
    echo "Extracted package is incomplete; missing $required" >&2
    exit 1
  }
done

if command -v cygpath >/dev/null 2>&1; then
  # pkgconf is a native Windows executable in UCRT64; use Windows paths and
  # the Windows semicolon list separator for its search path.
  kernel_pc_dir="$(cygpath -m "$kernel_prefix/lib/pkgconfig")"
  logger_pc_dir="$(cygpath -m "$logger_prefix/lib/pkgconfig")"
  package_pc_dir="$(cygpath -m "$work/package/lib/pkgconfig")"
  cmake_package_prefix="$(cygpath -m "$work/package")"
  cmake_kernel_prefix="$(cygpath -m "$kernel_prefix")"
  cmake_logger_prefix="$(cygpath -m "$logger_prefix")"
  export PKG_CONFIG_PATH="$package_pc_dir;$logger_pc_dir;$kernel_pc_dir"
else
  kernel_pc_dir="$kernel_prefix/lib/pkgconfig"
  logger_pc_dir="$logger_prefix/lib/pkgconfig"
  cmake_package_prefix="$work/package"
  cmake_kernel_prefix="$kernel_prefix"
  cmake_logger_prefix="$logger_prefix"
  export PKG_CONFIG_PATH="$work/package/lib/pkgconfig:$logger_pc_dir:$kernel_pc_dir"
fi
cmake_prefix="$cmake_package_prefix;$cmake_kernel_prefix;$cmake_logger_prefix"
cmake_consumer_build="$work/consumer-build"
if command -v cygpath >/dev/null 2>&1; then
  cmake_consumer_build="$(cygpath -m "$work/consumer-build")"
fi
case "$(uname -s)" in
  Darwin)
    lua_prefix="$(brew --prefix lua@5.4)"
    export PATH="$lua_prefix/bin:$PATH"
    export PKG_CONFIG_PATH="$lua_prefix/lib/pkgconfig:$work/package/lib/pkgconfig:$logger_pc_dir:$kernel_pc_dir"
    cmake_prefix="$cmake_package_prefix;$cmake_kernel_prefix;$cmake_logger_prefix;$lua_prefix"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    # PKG_CONFIG_PATH was already built with native Windows paths above.
    ;;
  *)
    export PKG_CONFIG_PATH="$work/package/lib/pkgconfig:$logger_pc_dir:$kernel_pc_dir"
    ;;
esac
pkg-config --exists cdin-backend
pkg-config --print-requires-private cdin-backend | grep -F "cdin-kernel"
pkg-config --print-requires-private cdin-backend | grep -F "m-mdy-m-log"
pkg-config --print-requires-private cdin-backend | grep -F "lua5.4"
test "$(pkg-config --modversion m-mdy-m-log)" = "0.1.0"

cmake -S "$repo_root/tests/package-consumer" -B "$cmake_consumer_build" \
  -DCMAKE_PREFIX_PATH="$cmake_prefix" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$cmake_consumer_build" --config Release --parallel
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    lua_prefix="$(pkg-config --variable=prefix lua5.4)"
    stage_windows_runtime_dlls "$cmake_consumer_build" "$lua_prefix" "cdin_backend_external_consumer.exe"
    for runtime_dir in "$lua_prefix/bin" "$kernel_prefix/bin" "$kernel_prefix/lib" "$logger_prefix/bin" "$logger_prefix/lib"; do
      if [[ -d "$runtime_dir" ]]; then PATH="$runtime_dir:$PATH"; fi
    done
    export PATH
    consumer_exe="$cmake_consumer_build/cdin_backend_external_consumer.exe"
    if command -v objdump >/dev/null 2>&1; then
      objdump -p "$consumer_exe" | sed -n 's/^[[:space:]]*DLL Name: /consumer dependency: /p'
    fi
    ;;
esac
ctest --test-dir "$cmake_consumer_build" --output-on-failure
echo "Verified extracted Backend package and independent CMake consumer: $(basename "$archive")"
