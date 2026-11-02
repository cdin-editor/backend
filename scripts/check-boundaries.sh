#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"
failed=0

report_match() {
  local title="$1"
  shift
  local matches
  matches="$(grep -R -n -E "$@" src include 2>/dev/null || true)"
  if [[ -n "$matches" ]]; then
    printf '\n%s\n%s\n' "$title" "$matches" >&2
    failed=1
  fi
}

# Kernel ABI details may be shared by implementation modules inside the
# private adapter directory, but must never escape into reusable services or
# public headers. This keeps the vendor ABI out of unrelated responsibilities.
if grep -R -n -E '#[[:space:]]*include[[:space:]]*[<"]cdin/kernel\.h[>"]' src include \
    --exclude-dir=kernel --exclude='adapter.c' >/tmp/cdin-backend-kernel-includes.txt 2>/dev/null; then
  echo "Kernel header included outside src/kernel/:" >&2
  cat /tmp/cdin-backend-kernel-includes.txt >&2
  failed=1
fi

service_matches="$(grep -R -n -E '#[[:space:]]*include[[:space:]]*[<"](cdin/kernel\.h|backend_internal\.h|lua\.h)[>"]' src/jobs src/os src/platform 2>/dev/null || true)"
if [[ -n "$service_matches" ]]; then
  echo "Reusable service modules must not depend on Kernel, Backend internals or Lua:" >&2
  printf '%s\n' "$service_matches" >&2
  failed=1
fi

section_glyph="$(printf '\u00a7')"
if grep -R -n --binary-files=without-match "$section_glyph" \
    README.md CHANGELOG.md docs examples src include tests .github scripts \
    Makefile CMakeLists.txt pipeline.toml psx.yml .editorconfig .gitattributes \
    deps.lock .gix 2>/dev/null; then
  echo "Unwanted section glyph found; use descriptive headings and file links." >&2
  failed=1
fi

report_match "Display toolkit dependency found in native source:" \
  '#[[:space:]]*include[[:space:]]*[<"](SDL|SDL2|ncurses|curses|X11|raylib|GLFW)'
report_match "A second logger dependency is forbidden; use the Kernel log API:" \
  '#[[:space:]]*include[[:space:]]*[<"]m-mdy-m/log\.h[>"]'
report_match "Shell-mediated process APIs are forbidden:" \
  '\b(system|popen|_popen)\s*\('

rm -f /tmp/cdin-backend-kernel-includes.txt
if [[ "$failed" -ne 0 ]]; then
  exit 1
fi

echo "Backend dependency-boundary checks passed."
