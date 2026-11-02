#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

command -v git >/dev/null 2>&1 || { echo "git is required to create a source release" >&2; exit 2; }
command -v tar >/dev/null 2>&1 || { echo "tar is required to create a source release" >&2; exit 2; }
command -v gzip >/dev/null 2>&1 || { echo "gzip is required to create a source release" >&2; exit 2; }

if ! git rev-parse --verify HEAD >/dev/null 2>&1; then
  echo "run this script from a Git checkout with at least one commit" >&2
  exit 2
fi

version="${PACKAGE_VERSION:-}"
if [[ -z "$version" && -n "${GITHUB_REF_NAME:-}" ]]; then
  version="${GITHUB_REF_NAME#v}"
fi
if [[ -z "$version" ]]; then
  version="$(sed -n 's/^## \[\([^]]*\)\].*/\1/p' CHANGELOG.md | head -n 1)"
fi
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$ ]]; then
  echo "invalid or missing release version: ${version:-<empty>}" >&2
  exit 2
fi

name="cdin-backend-${version}"
mkdir -p dist
tmp_tar="dist/${name}.tar"
archive="dist/${name}-source.tar.gz"
rm -f "$tmp_tar" "$archive" "${archive}.sha256"
# git archive includes only committed source files. This prevents local build
# output, downloaded .deps packages and generated gix state from entering a tag.
git archive --format=tar --prefix="${name}/" HEAD > "$tmp_tar"
gzip -n -9 "$tmp_tar"
mv "${tmp_tar}.gz" "$archive"
if command -v sha256sum >/dev/null 2>&1; then
  (cd dist && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")
else
  (cd dist && shasum -a 256 "$(basename "$archive")" > "$(basename "$archive").sha256")
fi
printf 'Created %s and its SHA-256 checksum.\n' "$archive"
