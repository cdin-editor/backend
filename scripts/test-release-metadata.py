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
REVISION = "a" * 40
TAG = "v1.2.1"
TARGETS = (
    ("linux", "x86_64", "gnu-glibc-2.39"),
    ("linux", "arm64", "gnu-glibc-2.39"),
    ("darwin", "x86_64", "macos-15"),
    ("darwin", "arm64", "macos-15"),
    ("windows", "x86_64", "windows-ucrt64"),
)


def run_generator(artifact_dir: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(GENERATOR),
            "--tag",
            TAG,
            "--source-revision",
            REVISION,
            "--artifact-dir",
            str(artifact_dir),
            "--output",
            str(output),
        ],
        capture_output=True,
        text=True,
        check=False,
    )


def prepare_descriptors(artifact_dir: Path) -> None:
    artifact_dir.mkdir(parents=True, exist_ok=True)
    for target_os, architecture, abi in TARGETS:
        artifact_id = f"backend-{target_os}-{architecture.replace('_', '-')}-{abi}"
        filename = (
            f"cdin-backend-1.2.1-{target_os}-{architecture.replace('_', '-')}-{abi}.tar.gz"
        )
        uri = f"https://github.com/cdin-editor/backend/releases/download/{TAG}/{filename}"
        archive = artifact_dir / filename
        payload = f"native-artifact:{target_os}:{architecture}:{abi}".encode("ascii")
        archive.write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        (artifact_dir / (filename + ".sha256")).write_text(
            f"{digest}  {filename}\n", encoding="ascii"
        )
        descriptor = {
            "source_revision": REVISION,
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
        (artifact_dir / (filename + ".artifact.json")).write_text(
            json.dumps(descriptor, indent=2) + "\n", encoding="utf-8"
        )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="backend-release-metadata-") as temporary:
        root = Path(temporary)
        artifact_dir = root / "assets"
        output = root / "backend-1.2.1.json"
        prepare_descriptors(artifact_dir)

        success = run_generator(artifact_dir, output)
        if success.returncode != 0:
            raise AssertionError(
                "valid five-target descriptors were rejected:\n"
                + success.stdout
                + success.stderr
            )

        metadata = json.loads(output.read_text(encoding="utf-8"))
        assert metadata["schema_version"] == "1.1.0"
        assert metadata["version"] == "1.2.1"
        assert metadata["source_units"][0]["revision"] == REVISION
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

        overwrite = run_generator(artifact_dir, output)
        assert overwrite.returncode != 0
        assert "refusing to overwrite immutable release metadata" in (
            overwrite.stdout + overwrite.stderr
        )

        corrupted = next(artifact_dir.glob("cdin-backend-*.tar.gz"))
        corrupted.write_bytes(corrupted.read_bytes() + b"tampered")
        mismatch = run_generator(artifact_dir, root / "should-not-exist.json")
        assert mismatch.returncode != 0
        assert "digest mismatch" in mismatch.stdout + mismatch.stderr

    print("Backend release metadata generator: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
