#!/usr/bin/env python3
"""Regression tests for immutable Backend artifact metadata generation."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts" / "generate-release-metadata.py"
TAG = "v1.2.2"
TARGETS = (
    ("linux", "x86_64", "gnu-glibc-2.39"),
    ("linux", "arm64", "gnu-glibc-2.39"),
    ("darwin", "x86_64", "macos-15"),
    ("darwin", "arm64", "macos-15"),
    ("windows", "x86_64", "windows-ucrt64"),
)


def run_generator(
    artifact_dir: Path,
    output: Path,
    source_repo: Path,
    *,
    source_revision: str = "",
) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable,
        str(GENERATOR),
        "--tag",
        TAG,
        "--artifact-dir",
        str(artifact_dir),
        "--output",
        str(output),
    ]
    if source_revision:
        command.extend(["--source-revision", source_revision])
    return subprocess.run(
        command,
        cwd=source_repo,
        capture_output=True,
        text=True,
        check=False,
    )


def prepare_source_repo(source_repo: Path) -> str:
    source_repo.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "init", "-q"], cwd=source_repo, check=True)
    subprocess.run(["git", "config", "user.name", "CDIN Test"], cwd=source_repo, check=True)
    subprocess.run(["git", "config", "user.email", "cdin-test@example.invalid"], cwd=source_repo, check=True)
    (source_repo / "README.md").write_text("Backend metadata fixture\\n", encoding="utf-8")
    subprocess.run(["git", "add", "README.md"], cwd=source_repo, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "test: prepare source identity"], cwd=source_repo, check=True)
    subprocess.run(["git", "tag", TAG], cwd=source_repo, check=True)
    return subprocess.check_output(
        ["git", "rev-parse", "--verify", f"{TAG}^{{commit}}"],
        cwd=source_repo,
        text=True,
    ).strip()


def prepare_descriptors(artifact_dir: Path, revision: str) -> None:
    artifact_dir.mkdir(parents=True, exist_ok=True)
    for target_os, architecture, abi in TARGETS:
        artifact_id = f"backend-{target_os}-{architecture.replace('_', '-')}-{abi}"
        filename = (
            f"cdin-backend-1.2.2-{target_os}-{architecture.replace('_', '-')}-{abi}.tar.gz"
        )
        uri = f"https://github.com/cdin-editor/backend/releases/download/{TAG}/{filename}"
        target_dir = artifact_dir / f"release-build-{target_os}-{architecture}" / "dist"
        target_dir.mkdir(parents=True, exist_ok=True)
        archive = target_dir / filename
        payload = f"native-artifact:{target_os}:{architecture}:{abi}".encode("ascii")
        archive.write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        (target_dir / (filename + ".sha256")).write_text(
            f"{digest}  {filename}\n", encoding="ascii"
        )
        descriptor = {
            "source_revision": revision,
            "artifact": {
                "artifact_id": artifact_id,
                "kind": "native-development-package",
                "target": {
                    "os": target_os,
                    "architecture": architecture,
                    "abi": abi,
                },
                "format": "tar.gz",
                "file_name": filename,
                "uri": uri,
                "sha256": digest,
                "size_bytes": len(payload),
                "layout": "cmake-prefix-v1",
                "source_unit_ids": ["backend-source"],
                "build": {
                    "recipe": {
                        "source_unit_id": "backend-source",
                        "path": "scripts/package-binary.sh",
                    },
                    "toolchain": [{"name": "compiler", "version": "test-compiler"}],
                    "runner": f"test-{target_os}-{architecture}",
                },
            },
        }
        (target_dir / (filename + ".artifact.json")).write_text(
            json.dumps(descriptor, indent=2) + "\n", encoding="utf-8"
        )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="backend-release-metadata-") as temporary:
        root = Path(temporary)
        source_repo = root / "source"
        revision = prepare_source_repo(source_repo)
        artifact_dir = root / "assets"
        output = artifact_dir / "release-build-metadata" / "backend-1.2.2.json"
        prepare_descriptors(artifact_dir, revision)

        success = run_generator(artifact_dir, output, source_repo, source_revision=revision)
        if success.returncode != 0:
            raise AssertionError(
                "valid five-target descriptors were rejected:\n"
                + success.stdout
                + success.stderr
            )

        metadata = json.loads(output.read_text(encoding="utf-8"))
        assert metadata["schema_version"] == "1.2.0"
        assert metadata["version"] == "1.2.2"
        assert metadata["source_units"][0]["revision"] == revision
        assert metadata["publication"] == {
            "provider": "github-releases-v1",
            "repository_uri": "https://github.com/cdin-editor/backend",
            "tag": TAG,
            "tag_commit": revision,
            "metadata_asset_name": "backend-1.2.2.json",
        }
        assert output.name == metadata["publication"]["metadata_asset_name"]
        assert len(metadata["artifacts"]) == len(TARGETS)
        assert {item["target"]["os"] for item in metadata["artifacts"]} == {
            "linux",
            "darwin",
            "windows",
        }
        assert [item["component_id"] for item in metadata["dependencies"]] == [
            "runtime"
        ]
        required = {
            item["contract_id"] for item in metadata["contracts"]["requires"]
        }
        assert required == {
            "org.cdin.kernel-abi",
            "org.m-mdy-m.log-c-api",
            "org.lua.puc-lua-c-api",
        }

        overwrite = run_generator(artifact_dir, output, source_repo, source_revision=revision)
        assert overwrite.returncode != 0
        assert "refusing to overwrite immutable release metadata" in (
            overwrite.stdout + overwrite.stderr
        )

        corrupted = next(artifact_dir.rglob("cdin-backend-*.tar.gz"))
        corrupted.write_bytes(corrupted.read_bytes() + b"tampered")
        mismatch = run_generator(artifact_dir, artifact_dir / "release-build-metadata" / "backend-1.2.2.json.tmp", source_repo, source_revision=revision)
        assert mismatch.returncode != 0
        assert "digest mismatch" in mismatch.stdout + mismatch.stderr

        wrong_revision = run_generator(
            artifact_dir,
            artifact_dir / "release-build-metadata" / "backend-1.2.2.json",
            source_repo,
            source_revision="b" * len(revision),
        )
        assert wrong_revision.returncode != 0
        assert "does not match the exact tag-resolved commit" in (
            wrong_revision.stdout + wrong_revision.stderr
        )

        (source_repo / "after-tag.txt").write_text("unreleased change\\n", encoding="utf-8")
        subprocess.run(["git", "add", "after-tag.txt"], cwd=source_repo, check=True)
        subprocess.run(
            ["git", "commit", "-q", "-m", "test: advance checkout after immutable tag"],
            cwd=source_repo,
            check=True,
        )
        wrong_head = run_generator(
            artifact_dir,
            artifact_dir / "release-build-metadata" / "backend-1.2.2-head-mismatch.json",
            source_repo,
            source_revision=revision,
        )
        assert wrong_head.returncode != 0
        assert "refusing to publish metadata from HEAD" in (
            wrong_head.stdout + wrong_head.stderr
        )

    print("Backend release metadata generator: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
