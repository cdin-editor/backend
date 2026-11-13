#!/usr/bin/env python3
"""Generate immutable Backend Release Metadata from published binary assets.

Run this from the exact release-tag checkout after downloading every
*.artifact.json descriptor and its matching archive/checksum sidecar.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlparse

EXPECTED_TARGETS = {
    ("linux", "x86_64", "gnu-glibc-2.39"),
    ("linux", "arm64", "gnu-glibc-2.39"),
    ("darwin", "x86_64", "macos-15"),
    ("darwin", "arm64", "macos-15"),
    ("windows", "x86_64", "windows-ucrt64"),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", default="")
    parser.add_argument("--artifact-dir", type=Path, default=Path("dist"))
    parser.add_argument("--source-revision", default="")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    tag = args.tag or os.environ.get("GITHUB_REF_NAME", "")
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", tag):
        parser.error("--tag must be a v-prefixed SemVer version")
    tag_version = tag[1:]
    dry_run = (
        os.environ.get("PIPELINE_DRY_RUN", "false").lower() == "true"
        and os.environ.get("GITHUB_REF_TYPE") != "tag"
    )
    head_revision = subprocess.check_output(
        ["git", "rev-parse", "--verify", "HEAD"], text=True
    ).strip()
    if dry_run:
        # Branch preflights use a synthetic candidate tag. Bind package
        # provenance to the checked-out commit without requiring a Git tag.
        tag_revision = head_revision
        if args.source_revision and args.source_revision.lower() != head_revision.lower():
            raise SystemExit("provided source revision does not match the checked-out dry-run commit")
    else:
        try:
            tag_revision = subprocess.check_output(
                ["git", "rev-parse", "--verify", f"{tag}^{{commit}}"],
                text=True,
                stderr=subprocess.PIPE,
            ).strip()
        except subprocess.CalledProcessError as error:
            raise SystemExit(f"unable to resolve immutable release tag {tag!r}") from error
        if not re.fullmatch(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})", tag_revision):
            raise SystemExit("tag-resolved source revision must be a full Git object ID")
        if args.source_revision and args.source_revision.lower() != tag_revision.lower():
            raise SystemExit("provided source revision does not match the exact tag-resolved commit")
        if head_revision.lower() != tag_revision.lower():
            raise SystemExit(
                f"refusing to publish metadata from HEAD {head_revision}; "
                f"exact tag {tag} resolves to {tag_revision}"
            )
    revision = tag_revision
    version = None

    artifact_root = args.artifact_dir.expanduser().resolve()
    if not artifact_root.is_dir():
        raise SystemExit(f"artifact directory does not exist: {artifact_root}")
    descriptors = sorted(
        path for path in artifact_root.rglob("*.tar.gz.artifact.json")
        if path.is_file() and not path.is_symlink()
    )
    if len(descriptors) != len(EXPECTED_TARGETS):
        raise SystemExit(
            f"expected {len(EXPECTED_TARGETS)} artifact descriptors; found {len(descriptors)}"
        )

    artifacts = []
    selected_targets = set()
    source_uri = "https://github.com/cdin-editor/backend"
    release_prefix = f"https://github.com/cdin-editor/backend/releases/download/{tag}/"

    for descriptor_path in descriptors:
        record = json.loads(descriptor_path.read_text(encoding="utf-8"))
        if record.get("source_revision") != revision:
            raise SystemExit(f"{descriptor_path}: source revision does not match {revision}")
        artifact = record.get("artifact")
        if not isinstance(artifact, dict):
            raise SystemExit(f"{descriptor_path}: missing artifact object")
        target = artifact.get("target", {})
        key = (target.get("os"), target.get("architecture"), target.get("abi"))
        if key not in EXPECTED_TARGETS or key in selected_targets:
            raise SystemExit(f"{descriptor_path}: unexpected or duplicate target {key}")
        selected_targets.add(key)

        expected_artifact_id = f"backend-{key[0]}-{key[1].replace('_', '-')}-{key[2]}"
        target_suffix = f"-{key[0]}-{key[1].replace('_', '-')}-{key[2]}.tar.gz"
        if artifact.get("artifact_id") != expected_artifact_id:
            raise SystemExit(f"{descriptor_path}: unexpected artifact ID")
        if artifact.get("kind") != "native-development-package":
            raise SystemExit(f"{descriptor_path}: artifact kind must be native-development-package")
        if artifact.get("format") != "tar.gz" or artifact.get("layout") != "cmake-prefix-v1":
            raise SystemExit(f"{descriptor_path}: unsupported package format or layout")
        if artifact.get("source_unit_ids") != ["backend-source"]:
            raise SystemExit(f"{descriptor_path}: artifact must reference backend-source")
        build = artifact.get("build")
        if not isinstance(build, dict) or build.get("recipe") != {
            "source_unit_id": "backend-source",
            "path": "scripts/package-binary.sh",
        } or not build.get("toolchain") or not build.get("runner"):
            raise SystemExit(f"{descriptor_path}: incomplete build provenance")

        filename = artifact.get("file_name", "")
        if (
            not isinstance(filename, str)
            or not filename.startswith("cdin-backend-")
            or not filename.endswith(target_suffix)
            or Path(filename).name != filename
        ):
            raise SystemExit(f"{descriptor_path}: unexpected filename {filename!r}")
        artifact_version = filename[len("cdin-backend-"):-len(target_suffix)]
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", artifact_version):
            raise SystemExit(f"{descriptor_path}: invalid package version in filename {filename!r}")
        if version is None:
            version = artifact_version
        elif version != artifact_version:
            raise SystemExit(
                f"{descriptor_path}: package version {artifact_version} differs from other targets ({version})"
            )
        if not dry_run and artifact_version != tag_version:
            raise SystemExit(
                f"{descriptor_path}: package version {artifact_version} does not match release tag {tag_version}"
            )
        if descriptor_path.name != filename + ".artifact.json":
            raise SystemExit(f"{descriptor_path}: descriptor filename does not match the asset")
        if not re.fullmatch(r"[a-fA-F0-9]{64}", str(artifact.get("sha256", ""))):
            raise SystemExit(f"{descriptor_path}: invalid SHA-256 digest")
        if not isinstance(artifact.get("size_bytes"), int) or artifact["size_bytes"] <= 0:
            raise SystemExit(f"{descriptor_path}: invalid artifact byte count")
        asset_path = descriptor_path.parent / filename
        if asset_path.is_symlink() or not asset_path.is_file():
            raise SystemExit(f"{descriptor_path}: missing archive {asset_path}")
        actual_digest = sha256(asset_path)
        actual_size = asset_path.stat().st_size
        if actual_digest != artifact.get("sha256"):
            raise SystemExit(f"{descriptor_path}: digest mismatch for {filename}")
        if actual_size != artifact.get("size_bytes"):
            raise SystemExit(f"{descriptor_path}: size mismatch for {filename}")
        sidecar = Path(str(asset_path) + ".sha256")
        if sidecar.is_symlink() or not sidecar.is_file():
            raise SystemExit(f"{descriptor_path}: missing checksum sidecar")
        sidecar_fields = sidecar.read_text(encoding="ascii").split()
        if len(sidecar_fields) < 2 or sidecar_fields[0] != actual_digest:
            raise SystemExit(f"{descriptor_path}: checksum sidecar mismatch")
        if sidecar_fields[1].lstrip("*") != filename:
            raise SystemExit(f"{descriptor_path}: checksum sidecar filename mismatch")
        if artifact.get("uri") != release_prefix + filename:
            raise SystemExit(f"{descriptor_path}: URI is not the exact tag asset URL")
        parsed_uri = urlparse(artifact["uri"])
        if parsed_uri.scheme != "https" or parsed_uri.netloc != "github.com":
            raise SystemExit(f"{descriptor_path}: artifact URI must use github.com over HTTPS")

        artifacts.append(artifact)

    if selected_targets != EXPECTED_TARGETS:
        missing = sorted(EXPECTED_TARGETS - selected_targets)
        raise SystemExit(f"missing required native target artifacts: {missing}")
    if version is None:
        raise SystemExit("could not resolve a common package version from the target artifacts")

    metadata = {
        "schema_version": "1.2.0",
        "component_id": "backend",
        "variant": "default",
        "version": version,
        "status": "active",
        "source_units": [{
            "source_unit_id": "backend-source",
            "repository_uri": source_uri,
            "revision": revision,
        }],
        "publication": {
            "provider": "github-releases-v1",
            "repository_uri": source_uri,
            "tag": tag,
            "tag_commit": revision,
            "metadata_asset_name": f"backend-{version}.json",
        },
        "artifacts": sorted(
            artifacts,
            key=lambda item: (
                item["target"]["os"],
                item["target"]["architecture"],
                item["target"]["abi"],
            ),
        ),
        "dependencies": [{
            "component_id": "runtime",
            "variant": "default",
            "version_constraint": ">=1.0.0-rc.2 <2.0.0",
        }],
        "contracts": {
            "provides": [
                {"contract_id": "org.cdin.runtime-host-api", "version": "1.0.0"},
                {"contract_id": "org.cdin.frontend-protocol", "version": "1.0.0"},
            ],
            "requires": [
                {
                    "contract_id": "org.cdin.kernel-abi",
                    "version_constraint": ">=1.1.0 <2.0.0",
                },
                {
                    "contract_id": "org.m-mdy-m.log-c-api",
                    "version_constraint": ">=0.1.0 <1.0.0",
                },
                {
                    "contract_id": "org.lua.puc-lua-c-api",
                    "version_constraint": ">=5.4.0 <5.5.0",
                },
            ],
        },
    }
    if args.output is None:
        parser.error("--output is required; release metadata must be an untracked Release Asset")
    output = args.output.expanduser().resolve()
    try:
        output_relative = output.relative_to(artifact_root)
    except ValueError as error:
        raise SystemExit("metadata output must be written beneath the downloaded artifact directory") from error
    if (
        output.name != f"backend-{version}.json"
        or len(output_relative.parts) != 2
        or not output_relative.parts[0].startswith("release-build-")
    ):
        raise SystemExit(
            "metadata output must use the canonical basename inside a top-level release-build-* directory"
        )
    if output.exists():
        raise SystemExit(f"refusing to overwrite immutable release metadata: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {output} for {len(artifacts)} verified target packages at source {revision}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"metadata generation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
