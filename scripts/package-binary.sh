#!/usr/bin/env bash
# Build one native, install-ready Backend CMake prefix for the explicit target.
# Registry consumes the archive as data; this script is run only by component CI.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"
# Share the exact component dependency pins with install-deps.sh.
# That helper runs in a child process, so its sourced variables do not
# propagate back to this package process.
# shellcheck source=../deps.lock
source "$repo_root/deps.lock"

tag="${PIPELINE_RELEASE_TAG:-${GITHUB_REF_NAME:-}}"
[[ -n "$tag" ]] || {
  echo "PIPELINE_RELEASE_TAG is required (for example v1.2.2)." >&2
  exit 2
}
version="${tag#v}"
project_version="$(sed -n 's/^project(cdin_backend VERSION \([^ ]*\).*/\1/p' CMakeLists.txt | head -n 1)"
[[ "$version" == "$project_version" ]] || {
  echo "release tag $tag does not match CMake package version $project_version" >&2
  exit 1
}

target_os="${BACKEND_TARGET_OS:-}"
target_arch="${BACKEND_TARGET_ARCH:-}"
target_abi="${BACKEND_TARGET_ABI:-}"
target_runner="${BACKEND_TARGET_RUNNER:-${RUNNER_OS:-local}-${RUNNER_ARCH:-$(uname -m)}}"
[[ -n "$target_os" && -n "$target_arch" && -n "$target_abi" ]] || {
  echo "Set BACKEND_TARGET_OS, BACKEND_TARGET_ARCH and BACKEND_TARGET_ABI explicitly." >&2
  exit 2
}

host_arch="$(uname -m)"
case "$host_arch" in
  x86_64|amd64|AMD64) host_arch=x86_64 ;;
  aarch64|arm64) host_arch=arm64 ;;
  *) echo "Unsupported native build architecture: $host_arch" >&2; exit 2 ;;
esac
[[ "$host_arch" == "$target_arch" ]] || {
  echo "Refusing cross-build: runner architecture $host_arch != requested $target_arch" >&2
  exit 1
}

cmake_extra=()
lua_prefix=""
case "$(uname -s)" in
  Linux)
    [[ "$target_os" == linux ]] || { echo "Linux runner cannot build target $target_os" >&2; exit 1; }
    glibc_version="$(getconf GNU_LIBC_VERSION | awk '{print $2}')"
    [[ "$glibc_version" == 2.39 && "$target_abi" == gnu-glibc-2.39 ]] || {
      echo "Expected Ubuntu 24.04/glibc 2.39, found glibc $glibc_version (ABI $target_abi)." >&2
      exit 1
    }
    ;;
  Darwin)
    [[ "$target_os" == darwin && "$target_abi" == macos-15 ]] || {
      echo "macOS package target must declare darwin/macos-15." >&2
      exit 1
    }
    macos_major="$(sw_vers -productVersion | cut -d. -f1)"
    [[ "$macos_major" == 15 ]] || {
      echo "Expected macOS 15 runner, found $macos_major." >&2
      exit 1
    }
    lua_prefix="$(brew --prefix lua@5.4)"
    export PATH="$lua_prefix/bin:$PATH"
    cmake_extra+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0")
    ;;
  MINGW*|MSYS*|CYGWIN*)
    [[ "$target_os" == windows && "$target_abi" == windows-ucrt64 && "${MSYSTEM:-}" == UCRT64 ]] || {
      echo "Windows packages require the MSYS2 UCRT64 ABI." >&2
      exit 1
    }
    ;;
  *)
    echo "Unsupported native build host: $(uname -s)" >&2
    exit 2
    ;;
esac

source_revision="$(git rev-parse --verify HEAD)"
git diff --quiet && git diff --cached --quiet || {
  echo "Refusing to package a dirty source checkout." >&2
  exit 1
}

kernel_prefix="${KERNEL_PREFIX:-$repo_root/.deps/kernel}"
logger_prefix="${LOGGER_PREFIX:-$repo_root/.deps/logger}"
bash scripts/install-deps.sh --prefix "$kernel_prefix" --logger-prefix "$logger_prefix"
export KERNEL_PREFIX="$kernel_prefix" LOGGER_PREFIX="$logger_prefix"
if command -v cygpath >/dev/null 2>&1; then
  kernel_pc_dir="$(cygpath -m "$kernel_prefix/lib/pkgconfig")"
  logger_pc_dir="$(cygpath -m "$logger_prefix/lib/pkgconfig")"
  cmake_kernel_prefix="$(cygpath -m "$kernel_prefix")"
  cmake_logger_prefix="$(cygpath -m "$logger_prefix")"
  pkgconfig_separator=';'
else
  kernel_pc_dir="$kernel_prefix/lib/pkgconfig"
  logger_pc_dir="$logger_prefix/lib/pkgconfig"
  cmake_kernel_prefix="$kernel_prefix"
  cmake_logger_prefix="$logger_prefix"
  pkgconfig_separator=':'
fi
export PKG_CONFIG_PATH="$logger_pc_dir$pkgconfig_separator$kernel_pc_dir${PKG_CONFIG_PATH:+$pkgconfig_separator$PKG_CONFIG_PATH}"
cmake_prefix="$cmake_kernel_prefix;$cmake_logger_prefix"
if [[ -n "$lua_prefix" ]]; then
  export PKG_CONFIG_PATH="$lua_prefix/lib/pkgconfig$pkgconfig_separator$PKG_CONFIG_PATH"
  cmake_prefix="$cmake_prefix;$lua_prefix"
fi

pkg-config --atleast-version=1.2.1 cdin-kernel
pkg-config --exact-version="${M_MDY_M_LOG_VERSION:?missing logger version pin in deps.lock}" m-mdy-m-log
pkg-config --modversion lua5.4

build_dir="$repo_root/build/binary-package"
stage="$repo_root/build/package-prefix"
cmake_stage="$stage"
cmake_build_dir="$build_dir"
if command -v cygpath >/dev/null 2>&1; then
  cmake_stage="$(cygpath -m "$stage")"
  cmake_build_dir="$(cygpath -m "$build_dir")"
fi
rm -rf "$build_dir" "$stage"
mkdir -p "$stage"

cmake -S . -B "$cmake_build_dir" \
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
  -DBUILD_TESTING=OFF \
  "${cmake_extra[@]}"
cmake --build "$cmake_build_dir" --config Release --parallel
cmake --install "$cmake_build_dir" --config Release

for required in \
  include/cdin/backend.h \
  lib/libcdin_backend.a \
  lib/libcdin_backend_services.a \
  lib/libcdin_protocol.a \
  lib/cmake/cdinBackend/cdinBackendConfig.cmake \
  lib/pkgconfig/cdin-backend.pc \
  lib/pkgconfig/cdin-backend-services.pc \
  lib/pkgconfig/cdin-protocol.pc; do
  [[ -f "$stage/$required" ]] || {
    echo "Install prefix is incomplete; missing $required" >&2
    exit 1
  }
done

build_info="$stage/share/cdin-backend"
mkdir -p "$build_info"
{
  printf 'package_version=%s\n' "$version"
  printf 'source_revision=%s\n' "$source_revision"
  printf 'target_os=%s\n' "$target_os"
  printf 'target_architecture=%s\n' "$target_arch"
  printf 'target_abi=%s\n' "$target_abi"
  printf 'runner=%s\n' "$target_runner"
  printf 'compiler=%s\n' "$(cc --version | head -n 1)"
  printf 'cmake=%s\n' "$(cmake --version | head -n 1)"
  printf 'kernel_package=%s\n' "$(pkg-config --modversion cdin-kernel)"
  printf 'logger_package=%s\n' "$(pkg-config --modversion m-mdy-m-log)"
  printf 'lua_package=%s\n' "$(pkg-config --modversion lua5.4)"
  printf 'build_recipe=scripts/package-binary.sh\n'
} > "$build_info/BUILD-INFO.txt"

asset_arch="${target_arch//_/-}"
asset="cdin-backend-$version-$target_os-$asset_arch-$target_abi.tar.gz"
mkdir -p dist
rm -f "dist/$asset" "dist/$asset.sha256" "dist/$asset.artifact.json"
python3 - "$stage" "$asset" <<'PY'
import gzip
import hashlib
import tarfile
from pathlib import Path
import sys

stage = Path(sys.argv[1]).resolve()
asset_name = sys.argv[2]
tar_path = Path("dist") / asset_name.removesuffix(".gz")
archive_path = Path("dist") / asset_name
paths = sorted(stage.rglob("*"), key=lambda item: item.relative_to(stage).as_posix())

with tarfile.open(tar_path, mode="w", format=tarfile.PAX_FORMAT) as archive:
    for path in paths:
        relative = path.relative_to(stage).as_posix()
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise SystemExit(f"unsupported package entry: {relative}")
        info = archive.gettarinfo(str(path), arcname=relative)
        info.uid = 0
        info.gid = 0
        info.uname = ""
        info.gname = ""
        info.mtime = 0
        info.mode &= 0o777
        if path.is_file():
            with path.open("rb") as source:
                archive.addfile(info, source)
        else:
            archive.addfile(info)

with tar_path.open("rb") as source, archive_path.open("wb") as raw_output:
    with gzip.GzipFile(filename="", mode="wb", fileobj=raw_output, compresslevel=9, mtime=0) as compressed:
        while chunk := source.read(1024 * 1024):
            compressed.write(chunk)
tar_path.unlink()
digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
Path(str(archive_path) + ".sha256").write_text(
    f"{digest}  {archive_path.name}\n", encoding="ascii"
)
print(f"Created {archive_path} ({archive_path.stat().st_size} bytes), sha256={digest}")
PY

artifact_uri="https://github.com/cdin-editor/backend/releases/download/$tag/$asset"
python3 - "$asset" "$artifact_uri" "$target_os" "$target_arch" "$target_abi" "$source_revision" "$target_runner" <<'PY'
import hashlib
import json
import os
import sys
from pathlib import Path

asset_name, uri, target_os, architecture, abi, revision, runner = sys.argv[1:]
asset_path = Path("dist") / asset_name
digest = hashlib.sha256(asset_path.read_bytes()).hexdigest()
size = asset_path.stat().st_size
version = os.environ.get("PIPELINE_RELEASE_TAG", os.environ.get("GITHUB_REF_NAME", "")).removeprefix("v")
compiler = os.popen("cc --version 2>/dev/null").read().splitlines()
cmake = os.popen("cmake --version 2>/dev/null").read().splitlines()
kernel = os.popen("pkg-config --modversion cdin-kernel 2>/dev/null").read().strip()
lua = os.popen("pkg-config --modversion lua5.4 2>/dev/null").read().strip()
logger = os.popen("pkg-config --modversion m-mdy-m-log 2>/dev/null").read().strip()
record = {
    "source_revision": revision,
    "artifact": {
        "artifact_id": f"backend-{target_os}-{architecture.replace('_', '-')}-{abi}",
        "kind": "native-development-package",
        "target": {"os": target_os, "architecture": architecture, "abi": abi},
        "format": "tar.gz",
        "file_name": asset_name,
        "uri": uri,
        "sha256": digest,
        "size_bytes": size,
        "layout": "cmake-prefix-v1",
        "source_unit_ids": ["backend-source"],
        "build": {
            "recipe": {"source_unit_id": "backend-source", "path": "scripts/package-binary.sh"},
            "toolchain": [
                {"name": "compiler", "version": compiler[0] if compiler else "unknown"},
                {"name": "cmake", "version": cmake[0] if cmake else "unknown"},
                {"name": "cdin-kernel", "version": kernel or "unknown"},
                {"name": "lua", "version": lua or "unknown"},
                {"name": "m-mdy-m-log", "version": logger or "unknown"},
            ],
            "runner": runner,
        },
    },
}
output = Path("dist") / (asset_name + ".artifact.json")
output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
print(f"Prepared {asset_name}: {size} bytes sha256={digest}")
PY
