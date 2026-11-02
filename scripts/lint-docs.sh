#!/usr/bin/env bash
set -euo pipefail

if ! command -v markdownlint-cli2 >/dev/null 2>&1; then
  echo "markdownlint-cli2 is required; install it with: npm install --global markdownlint-cli2" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"
markdownlint-cli2 "**/*.md"
