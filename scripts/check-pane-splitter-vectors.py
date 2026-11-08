#!/usr/bin/env python3
"""Verify the pinned Spec PaneTree conformance fixture and Backend coverage map."""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VECTOR = ROOT / "tests" / "protocol" / "vectors" / "pane-splitter-v1.json"
CODEC_TEST = ROOT / "tests" / "protocol" / "test_codec.c"
SERVER_TEST = ROOT / "tests" / "protocol" / "test_server.c"

REQUIRED_BACKEND_CASES = {
    "horizontal-split-commit",
    "minimum-and-maximum-ratios",
    "ratio-below-minimum-rejected",
    "ratio-above-maximum-rejected",
    "stale-layout-revision-rejected",
    "observer-cannot-resize",
    "capability-absent-does-not-send-extension",
    "success-result-precedes-atomic-snapshot",
}

def main() -> int:
    document = json.loads(VECTOR.read_text(encoding="utf-8"))
    if document.get("schema") != "cdin.frontend-protocol.conformance.v1":
        raise SystemExit("unexpected Spec vector schema")
    if document.get("family") != "proto.pane-splitter" or document.get("status") != "ACCEPTED":
        raise SystemExit("pane splitter fixture is not the accepted Spec family")
    if document.get("capability") != "pane-splitter-resize/1" or document.get("optional_namespace") != "pane":
        raise SystemExit("capability or namespace differs from ADR-0024")
    limits = document.get("ratio_bp", {})
    if limits != {"min": 1000, "max": 8999}:
        raise SystemExit(f"ratio bounds mismatch: {limits!r}")
    cases = document.get("cases")
    if not isinstance(cases, list) or not cases:
        raise SystemExit("Spec vector cases are missing")
    by_id = {case.get("id"): case for case in cases}
    if None in by_id or len(by_id) != len(cases):
        raise SystemExit("Spec vector IDs are missing or duplicated")
    missing = REQUIRED_BACKEND_CASES - by_id.keys()
    if missing:
        raise SystemExit(f"required Backend vectors missing from Spec: {sorted(missing)}")

    codec_source = CODEC_TEST.read_text(encoding="utf-8")
    server_source = SERVER_TEST.read_text(encoding="utf-8")
    coverage_map = {
        "ratio-below-minimum-rejected": codec_source,
        "ratio-above-maximum-rejected": codec_source,
        "horizontal-split-commit": server_source,
        "minimum-and-maximum-ratios": codec_source,
        "stale-layout-revision-rejected": server_source,
        "observer-cannot-resize": server_source,
        "capability-absent-does-not-send-extension": server_source,
        "success-result-precedes-atomic-snapshot": server_source,
    }
    uncovered = [case_id for case_id, source in coverage_map.items()
                 if f"Spec vector: {case_id}" not in source]
    if uncovered:
        raise SystemExit(f"Backend test mapping missing Spec vector markers: {uncovered}")
    print(f"PASS Spec pane-splitter vectors: {len(cases)} cases; Backend maps {len(coverage_map)} cases")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
