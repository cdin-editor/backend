#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=../deps.lock
source "$repo_root/deps.lock"

prefix="$repo_root/.deps/kernel"
force=0
while (($#)); do
  case "$1" in
    --prefix)
      [[ $# -ge 2 && -n "$2" ]] || { echo "--prefix requires a path" >&2; exit 2; }
      prefix="$2"
      shift 2
      ;;
    --force)
      force=1
      shift
      ;;
    --help|-h)
      cat <<'USAGE'
Usage: bash scripts/install-deps.sh [--prefix PATH] [--force]

Downloads the pinned cdin-kernel binary release, verifies its SHA-256 sidecar,
and installs its headers and libraries into .deps/kernel (or --prefix).
No Kernel source checkout is required.
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
arch="$(uname -m 2>/dev/null || printf unknown)"
case "$host" in
  Linux*) platform=linux ;;
  Darwin*) platform=macos ;;
  MINGW*|MSYS*|CYGWIN*) platform=windows ;;
  *) echo "unsupported operating system: $host" >&2; exit 2 ;;
esac
case "$arch" in
  x86_64|amd64|x64) arch=x86_64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) echo "unsupported architecture: $arch" >&2; exit 2 ;;
esac
if [[ "$platform" == windows && "$arch" != x86_64 ]]; then
  echo "cdin-kernel ${CDIN_KERNEL_VERSION} currently publishes Windows x86_64 packages only" >&2
  exit 2
fi

asset_base="cdin-kernel-${CDIN_KERNEL_VERSION}-${platform}-${arch}"
if [[ "$platform" == windows ]]; then
  archive_name="${asset_base}.zip"
else
  archive_name="${asset_base}.tar.gz"
fi
release_url="${CDIN_KERNEL_REPOSITORY}/releases/download/v${CDIN_KERNEL_VERSION}"
archive_url="${release_url}/${archive_name}"
checksum_url="${archive_url}.sha256"
cache_dir="$repo_root/build/deps/kernel-download"
mkdir -p "$cache_dir" "$(dirname "$prefix")"
archive_path="$cache_dir/$archive_name"
checksum_path="$cache_dir/${archive_name}.sha256"

installed_version=""
if [[ -f "$prefix/lib/pkgconfig/cdin-kernel.pc" ]]; then
  pc_dir="$prefix/lib/pkgconfig"
  if command -v cygpath >/dev/null 2>&1; then pc_dir="$(cygpath -m "$pc_dir")"; fi
  installed_version="$(PKG_CONFIG_PATH="$pc_dir" pkg-config --modversion cdin-kernel 2>/dev/null || true)"
fi
if [[ "$force" -eq 0 && "$installed_version" == "$CDIN_KERNEL_VERSION" && -f "$prefix/include/cdin/kernel.h" ]]; then
  echo "cdin-kernel ${installed_version} is already installed at ${prefix}"
  exit 0
fi

printf 'Downloading %s\n' "$archive_url"
curl --fail --location --retry 3 --retry-delay 1 "$archive_url" --output "$archive_path"
curl --fail --location --retry 3 --retry-delay 1 "$checksum_url" --output "$checksum_path"

expected_sha="$(awk 'NR == 1 { print $1; exit }' "$checksum_path" | tr '[:upper:]' '[:lower:]')"
if [[ ! "$expected_sha" =~ ^[0-9a-f]{64}$ ]]; then
  echo "invalid SHA-256 sidecar: $checksum_path" >&2
  exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
  actual_sha="$(sha256sum "$archive_path" | awk '{print tolower($1)}')"
else
  actual_sha="$(shasum -a 256 "$archive_path" | awk '{print tolower($1)}')"
fi
if [[ "$actual_sha" != "$expected_sha" ]]; then
  echo "SHA-256 mismatch for $archive_name" >&2
  echo "expected: $expected_sha" >&2
  echo "actual:   $actual_sha" >&2
  rm -f "$archive_path"
  exit 1
fi
printf 'Verified SHA-256: %s\n' "$actual_sha"

extract_root="$cache_dir/extracted"
rm -rf "$extract_root"
mkdir -p "$extract_root"
if [[ "$platform" == windows ]]; then
  command -v unzip >/dev/null 2>&1 || { echo "unzip is required to extract the Windows release" >&2; exit 2; }
  unzip -q "$archive_path" -d "$extract_root"
else
  tar -xzf "$archive_path" -C "$extract_root"
fi
package_root="$extract_root/$asset_base"
if [[ ! -f "$package_root/BUILD-INFO.txt" || ! -f "$package_root/include/cdin/kernel.h" ]]; then
  echo "release archive has an unexpected or incomplete package layout" >&2
  exit 1
fi

if [[ "$platform" == windows ]]; then
  if command -v pwsh >/dev/null 2>&1; then
    powershell_bin=pwsh
  elif command -v powershell.exe >/dev/null 2>&1; then
    powershell_bin=powershell.exe
  else
    echo "PowerShell is required to install the Windows Kernel package" >&2
    exit 2
  fi
  command -v cygpath >/dev/null 2>&1 || { echo "cygpath is required in the MSYS2 environment" >&2; exit 2; }
  "$powershell_bin" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
    -File "$(cygpath -w "$package_root/install.ps1")" \
    -Prefix "$(cygpath -w "$prefix")"
else
  bash "$package_root/install.sh" --prefix "$prefix"
fi

pkgconfig_dir="$prefix/lib/pkgconfig"
if command -v cygpath >/dev/null 2>&1; then
  # MSYS2's pkgconf is a native Windows program and expects a Windows path.
  pkgconfig_dir="$(cygpath -m "$pkgconfig_dir")"
fi
installed_version="$(PKG_CONFIG_PATH="$pkgconfig_dir" pkg-config --modversion cdin-kernel)"
if [[ "$installed_version" != "$CDIN_KERNEL_VERSION" ]]; then
  echo "Kernel package version mismatch: expected ${CDIN_KERNEL_VERSION}, found ${installed_version}" >&2
  exit 1
fi
abi_major="$(sed -n 's/^#define CDIN_KERNEL_ABI_MAJOR \([0-9][0-9]*\)u/\1/p' "$prefix/include/cdin/kernel.h")"
abi_minor="$(sed -n 's/^#define CDIN_KERNEL_ABI_MINOR \([0-9][0-9]*\)u/\1/p' "$prefix/include/cdin/kernel.h")"
abi_patch="$(sed -n 's/^#define CDIN_KERNEL_ABI_PATCH \([0-9][0-9]*\)u/\1/p' "$prefix/include/cdin/kernel.h")"
actual_abi="${abi_major}.${abi_minor}.${abi_patch}"
expected_abi="${CDIN_KERNEL_ABI_MAJOR}.${CDIN_KERNEL_ABI_MINOR}.${CDIN_KERNEL_ABI_PATCH}"
if [[ "$actual_abi" != "$expected_abi" ]]; then
  echo "Kernel ABI mismatch: expected ${expected_abi}, found ${actual_abi}" >&2
  exit 1
fi
for library in libcdin_kernel.a libm-mdy-m-log.a; do
  if [[ ! -f "$prefix/lib/$library" ]]; then
    echo "Kernel release is missing required static library: $prefix/lib/$library" >&2
    exit 1
  fi
done

printf '\nVerified cdin-kernel package %s (C ABI %s).\n' "$installed_version" "$actual_abi"
printf 'Set PKG_CONFIG_PATH to: %s\n' "$pkgconfig_dir"
printf 'The Kernel logger is resolved transitively by cdin-kernel.pc (Requires.private).\n'
