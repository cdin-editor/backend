#!/usr/bin/env bash
# Native CI gate. Reusable services are exercised without Kernel; the Backend
# integration scenario then links the pinned Kernel package and its logger.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

# Keep the package location identical for the installer and every later build.
export KERNEL_PREFIX="${KERNEL_PREFIX:-$repo_root/.deps/kernel}"

make e2e
if [[ "${CDIN_SKIP_DEP_INSTALL:-0}" == "1" ]]; then
  echo "Using the already installed Kernel package at $KERNEL_PREFIX"
else
  bash scripts/install-deps.sh --prefix "$KERNEL_PREFIX"
fi

# Make computes pkg-config flags while parsing its Makefile. Use a new Make
# process after installation so no pre-install lookup can be cached. The
# Makefile also normalizes this path for native MSYS2 pkgconf on Windows.
make all
make integration
bash scripts/check-boundaries.sh
