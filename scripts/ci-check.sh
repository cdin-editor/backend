#!/usr/bin/env bash
# Native CI gate. Reusable services are exercised without Kernel; the Backend
# integration scenario then links the pinned Kernel package and its logger.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

make e2e
bash scripts/install-deps.sh
make all integration
python3 scripts/check-pane-splitter-vectors.py
bash scripts/check-boundaries.sh
