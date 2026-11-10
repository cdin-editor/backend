#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=../deps.lock
source "$repo_root/deps.lock"

kernel_prefix="$repo_root/.deps/kernel"
logger_prefix="$repo_root/.deps/logger"
force=0
while (($#)); do
  case "$1" in
    --prefix)
      [[ $# -ge 2 && -n "$2" ]] || { echo "--prefix requires a path" >&2; exit 2; }
      kernel_prefix="$2"
      shift 2
      ;;
    --logger-prefix)
      [[ $# -ge 2 && -n "$2" ]] || { echo "--logger-prefix requires a path" >&2; exit 2; }
      logger_prefix="$2"
      shift 2
      ;;
    --force)
      force=1
      shift
      ;;
    --help|-h)
      cat <<'USAGE'
Usage: bash scripts/install-deps.sh [--prefix KERNEL_PATH] [--logger-prefix LOGGER_PATH] [--force]

Downloads and verifies the pinned cdin-kernel and m-mdy-m/log.c SDK releases.
The two SDKs are installed into independent prefixes (.deps/kernel and
.deps/logger by default). No component source checkout or build is required.
USAGE
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

for tool in curl pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "missing required tool: $tool" >&2
    exit 2
  }
done

host="$(uname -s 2>/dev/null || printf unknown)"
host_arch="$(uname -m 2>/dev/null || printf unknown)"
case "$host" in
  Linux*) platform=linux ;;
  Darwin*) platform=macos ;;
  MINGW*|MSYS*|CYGWIN*) platform=windows ;;
  *) echo "unsupported operating system: $host" >&2; exit 2 ;;
esac
case "$host_arch" in
  x86_64|amd64|x64) arch=x86_64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) echo "unsupported architecture: $host_arch" >&2; exit 2 ;;
esac
if [[ "$platform" == windows && "$arch" != x86_64 ]]; then
  echo "Windows SDK packages currently target x86_64 UCRT64 only" >&2
  exit 2
fi

if command -v cygpath >/dev/null 2>&1; then
  path_separator=';'
  to_native_path() { cygpath -m "$1"; }
  to_windows_path() { cygpath -w "$1"; }
  if command -v pwsh >/dev/null 2>&1; then powershell_bin=pwsh
  elif command -v powershell.exe >/dev/null 2>&1; then powershell_bin=powershell.exe
  else echo "PowerShell is required in the MSYS2 environment" >&2; exit 2
  fi
else
  path_separator=':'
  to_native_path() { printf '%s' "$1"; }
fi

verify_checksum() {
  local archive="$1" sidecar="$2" expected actual
  expected="$(awk 'NR == 1 { print $1; exit }' "$sidecar" | tr '[:upper:]' '[:lower:]')"
  [[ "$expected" =~ ^[0-9a-f]{64}$ ]] || {
    echo "invalid SHA-256 sidecar: $sidecar" >&2
    exit 1
  }
  if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$archive" | awk '{print tolower($1)}')"
  else
    actual="$(shasum -a 256 "$archive" | awk '{print tolower($1)}')"
  fi
  [[ "$actual" == "$expected" ]] || {
    echo "SHA-256 mismatch for $(basename "$archive")" >&2
    echo "expected: $expected" >&2
    echo "actual:   $actual" >&2
    exit 1
  }
  printf 'Verified SHA-256: %s\n' "$actual" >&2
}

download_release_asset() {
  local repository="$1" tag="$2" filename="$3" cache="$4" archive sidecar extract_root
  mkdir -p "$cache"
  archive="$cache/$filename"
  sidecar="$archive.sha256"
  curl --fail --location --retry 3 --retry-delay 1 \
    "$repository/releases/download/$tag/$filename" --output "$archive"
  curl --fail --location --retry 3 --retry-delay 1 \
    "$repository/releases/download/$tag/$filename.sha256" --output "$sidecar"
  verify_checksum "$archive" "$sidecar"
  extract_root="$cache/extracted"
  rm -rf "$extract_root"
  mkdir -p "$extract_root"
  if [[ "$filename" == *.zip ]]; then
    command -v unzip >/dev/null 2>&1 || { echo "unzip is required to extract $filename" >&2; exit 2; }
    unzip -q "$archive" -d "$extract_root"
  else
    tar -xzf "$archive" -C "$extract_root"
  fi
  printf '%s\n' "$extract_root"
}

install_release_package() {
  local package_root="$1" prefix="$2" installer="$3"
  if [[ "$platform" == windows ]]; then
    "$powershell_bin" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
      -File "$(to_windows_path "$package_root/$installer")" \
      -Prefix "$(to_windows_path "$prefix")"
  else
    bash "$package_root/$installer" --prefix "$prefix"
  fi
}

# Kernel SDK: reuse a verified installation unless --force was requested.
kernel_pc_dir="$(to_native_path "$kernel_prefix/lib/pkgconfig")"
kernel_version=""
if [[ -f "$kernel_prefix/lib/pkgconfig/cdin-kernel.pc" ]]; then
  kernel_version="$(PKG_CONFIG_PATH="$kernel_pc_dir" pkg-config --modversion cdin-kernel 2>/dev/null || true)"
fi
if [[ "$force" -eq 0 && "$kernel_version" == "$CDIN_KERNEL_VERSION" &&
      -f "$kernel_prefix/include/cdin/kernel.h" && -f "$kernel_prefix/lib/libcdin_kernel.a" ]]; then
  printf 'cdin-kernel %s already installed at %s\n' "$kernel_version" "$kernel_prefix"
else
  kernel_asset="cdin-kernel-$CDIN_KERNEL_VERSION-$platform-$arch"
  kernel_format=tar.gz
  [[ "$platform" != windows ]] || kernel_format=zip
  kernel_filename="$kernel_asset.$kernel_format"
  kernel_cache="$repo_root/build/deps/kernel-download"
  kernel_extract="$(download_release_asset "$CDIN_KERNEL_REPOSITORY" "v$CDIN_KERNEL_VERSION" "$kernel_filename" "$kernel_cache")"
  kernel_package="$kernel_extract/$kernel_asset"
  [[ -f "$kernel_package/BUILD-INFO.txt" && -f "$kernel_package/include/cdin/kernel.h" ]] || {
    echo "Kernel release archive has an unexpected package layout: $kernel_package" >&2
    exit 1
  }
  mkdir -p "$(dirname "$kernel_prefix")"
  kernel_installer=install.sh
  [[ "$platform" != windows ]] || kernel_installer=install.ps1
  install_release_package "$kernel_package" "$kernel_prefix" "$kernel_installer"
fi

# Logger SDK: it has its own release identity, target asset and install prefix.
case "$platform:$arch" in
  linux:x86_64) logger_target=linux-x86_64 ;;
  linux:arm64) logger_target=linux-aarch64 ;;
  macos:x86_64) logger_target=macos-x86_64 ;;
  macos:arm64) logger_target=macos-arm64 ;;
  windows:x86_64) logger_target=windows-x86_64-ucrt64 ;;
  *) echo "unsupported Logger SDK target: $platform/$arch" >&2; exit 2 ;;
esac
logger_asset="m-mdy-m-log-v$M_MDY_M_LOG_VERSION-$logger_target"
logger_format=tar.gz
[[ "$platform" != windows ]] || logger_format=zip
logger_filename="$logger_asset.$logger_format"
logger_pc_dir="$(to_native_path "$logger_prefix/lib/pkgconfig")"
logger_version=""
if [[ -f "$logger_prefix/lib/pkgconfig/m-mdy-m-log.pc" ]]; then
  logger_version="$(PKG_CONFIG_PATH="$logger_pc_dir" pkg-config --modversion m-mdy-m-log 2>/dev/null || true)"
fi
if [[ "$force" -eq 0 && "$logger_version" == "$M_MDY_M_LOG_VERSION" &&
      -f "$logger_prefix/include/m-mdy-m/log.h" && -f "$logger_prefix/lib/libm-mdy-m-log.a" ]]; then
  printf 'm-mdy-m/log.c %s already installed at %s\n' "$logger_version" "$logger_prefix"
else
  logger_cache="$repo_root/build/deps/logger-download"
  logger_extract="$(download_release_asset "$M_MDY_M_LOG_REPOSITORY" "v$M_MDY_M_LOG_VERSION" "$logger_filename" "$logger_cache")"
  logger_package="$logger_extract/$logger_asset"
  [[ -f "$logger_package/BUILD-INFO.txt" &&
     -f "$logger_package/include/m-mdy-m/log.h" &&
     -f "$logger_package/lib/libm-mdy-m-log.a" &&
     -f "$logger_package/lib/pkgconfig/m-mdy-m-log.pc" ]] || {
    echo "Logger SDK release archive has an unexpected package layout: $logger_package" >&2
    exit 1
  }
  mkdir -p "$(dirname "$logger_prefix")"
  installer=install.sh
  [[ "$platform" != windows ]] || installer=install.ps1
  install_release_package "$logger_package" "$logger_prefix" "$installer"
fi

# Validate each identity in isolation, then validate Kernel's static closure
# with the independently pinned Logger metadata taking precedence.
kernel_pc_dir="$(to_native_path "$kernel_prefix/lib/pkgconfig")"
logger_pc_dir="$(to_native_path "$logger_prefix/lib/pkgconfig")"
installed_kernel="$(PKG_CONFIG_PATH="$kernel_pc_dir" pkg-config --modversion cdin-kernel)"
installed_logger="$(PKG_CONFIG_PATH="$logger_pc_dir" pkg-config --modversion m-mdy-m-log)"
[[ "$installed_kernel" == "$CDIN_KERNEL_VERSION" ]] || {
  echo "Kernel version mismatch: expected $CDIN_KERNEL_VERSION, found $installed_kernel" >&2
  exit 1
}
[[ "$installed_logger" == "$M_MDY_M_LOG_VERSION" ]] || {
  echo "Logger version mismatch: expected $M_MDY_M_LOG_VERSION, found $installed_logger" >&2
  exit 1
}
abi_major="$(sed -n 's/^#define CDIN_KERNEL_ABI_MAJOR \([0-9][0-9]*\)u/\1/p' "$kernel_prefix/include/cdin/kernel.h")"
abi_minor="$(sed -n 's/^#define CDIN_KERNEL_ABI_MINOR \([0-9][0-9]*\)u/\1/p' "$kernel_prefix/include/cdin/kernel.h")"
abi_patch="$(sed -n 's/^#define CDIN_KERNEL_ABI_PATCH \([0-9][0-9]*\)u/\1/p' "$kernel_prefix/include/cdin/kernel.h")"
actual_abi="$abi_major.$abi_minor.$abi_patch"
expected_abi="$CDIN_KERNEL_ABI_MAJOR.$CDIN_KERNEL_ABI_MINOR.$CDIN_KERNEL_ABI_PATCH"
[[ "$actual_abi" == "$expected_abi" ]] || {
  echo "Kernel ABI mismatch: expected $expected_abi, found $actual_abi" >&2
  exit 1
}
for library in "$kernel_prefix/lib/libcdin_kernel.a" "$logger_prefix/lib/libm-mdy-m-log.a"; do
  [[ -f "$library" ]] || { echo "Missing required SDK library: $library" >&2; exit 1; }
done
combined_pkgconfig="$logger_pc_dir$path_separator$kernel_pc_dir"
PKG_CONFIG_PATH="$combined_pkgconfig" pkg-config --validate cdin-kernel
PKG_CONFIG_PATH="$combined_pkgconfig" pkg-config --validate m-mdy-m-log
PKG_CONFIG_PATH="$combined_pkgconfig" pkg-config --static --libs cdin-kernel >/dev/null
export KERNEL_PREFIX="$kernel_prefix"
export LOGGER_PREFIX="$logger_prefix"
export PKG_CONFIG_PATH="$combined_pkgconfig${PKG_CONFIG_PATH:+$path_separator$PKG_CONFIG_PATH}"
printf '\nVerified cdin-kernel %s (ABI %s) and m-mdy-m/log.c %s.\n' \
  "$installed_kernel" "$actual_abi" "$installed_logger"
printf 'KERNEL_PREFIX=%s\nLOGGER_PREFIX=%s\n' "$kernel_prefix" "$logger_prefix"
printf 'PKG_CONFIG_PATH=%s\n' "$PKG_CONFIG_PATH"
