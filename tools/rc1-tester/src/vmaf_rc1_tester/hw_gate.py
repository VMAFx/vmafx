# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The cross-backend parity gate's Metal cells, on a Mac (the macOS tester bundle).

`scripts/ci/cross_backend_parity_gate.py` has a `metal` backend since ADR-1496,
and an Apple device exists only on the tester's machine, so the bundle carries
the gate (`tester/gate/`) and runs it here on every fixture:
`--backends cpu metal --hold-exact metal`. Every cell is compared exactly, or at
the gate's LIBM_TWINS bound for a math-library feature, at `--precision max`:
the measurement a `scripts/ci/exact_twins.d/<feature>.metal` fragment would
cite. A feature the Metal twin cannot run on a fixture for a recorded reason is
left out of that fixture's run and listed with the reason.
"""

from __future__ import annotations

import json
import sys
import tempfile
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from .hw_equiv import Runner
from .safe_process import run_bounded

GATE_SCRIPT = Path("tester") / "gate" / "scripts" / "ci" / "cross_backend_parity_gate.py"
MAX_NOTE = 200
GATE_OUTPUT_BYTES = 4 * 1_048_576


def gate_features(config: Mapping[str, Any], fixture_id: str) -> tuple[list[str], list[dict]]:
    """(features the gate runs on this fixture, features left out with their reason)."""
    left_out: list[dict[str, str]] = []
    for entry in config.get("skip", []):
        if entry["fixture"] == fixture_id:
            left_out += [{"feature": f, "reason": entry["reason"]} for f in entry["features"]]
    skipped = {item["feature"] for item in left_out}
    return [f for f in config["features"] if f not in skipped], left_out


def gate_argv(
    script: Path,
    vmaf: str,
    fixture: Mapping[str, Any],
    features: Sequence[str],
    workdir: Path,
    json_out: Path,
) -> list[str]:
    """One gate run: every listed feature, CPU against Metal, Metal held exact."""
    return [
        sys.executable, "-I", "-B", str(script),
        "--vmaf-binary", vmaf, "--reference", str(fixture["ref"]),
        "--distorted", str(fixture["dis"]), "--width", str(fixture["width"]),
        "--height", str(fixture["height"]), "--pixel-format", str(fixture["pixel_format"]),
        "--bitdepth", str(fixture["bitdepth"]), "--backends", "cpu", "metal",
        "--hold-exact", "metal", "--features", *features,
        "--workdir", str(workdir), "--json-out", str(json_out),
    ]  # fmt: skip


def gate_cell(raw: Mapping[str, Any]) -> dict[str, Any]:
    """The report's form of one gate cell."""
    per_max = raw.get("per_metric_max_abs_diff", {})
    largest = max(per_max.values(), default=0.0)
    return {
        "feature": str(raw["feature"]),
        "status": str(raw["status"]),
        "tolerance": f"{float(raw['tolerance_abs']):.17g}",
        "tolerance_source": str(raw["tolerance_source"]),
        "frames": int(raw["n_frames"]),
        "max_abs_diff": f"{float(largest):.17g}",
        "mismatches": int(sum(raw.get("per_metric_mismatches", {}).values())),
        "note": str(raw.get("note", ""))[:MAX_NOTE],
    }


def run_gate_fixture(
    root: Path,
    vmaf: str,
    fixture: Mapping[str, Any],
    config: Mapping[str, Any],
    *,
    timeout_seconds: float,
    runner: Runner = run_bounded,
) -> dict[str, Any]:
    """The gate on one fixture; `error` when it left no JSON summary."""
    features, left_out = gate_features(config, str(fixture["id"]))
    entry: dict[str, Any] = {"fixture": str(fixture["id"]), "left_out": left_out}
    with tempfile.TemporaryDirectory(prefix="vmaf-gate-") as work:
        json_out = Path(work) / "gate.json"
        argv = gate_argv(root / GATE_SCRIPT, vmaf, fixture, features, Path(work), json_out)
        try:
            result = runner(
                argv, timeout_seconds=timeout_seconds, max_output_bytes=GATE_OUTPUT_BYTES
            )
            payload = json.loads(json_out.read_text(encoding="utf-8"))
        except (TimeoutError, RuntimeError, ValueError, OSError) as error:
            entry["error"] = str(error)[:MAX_NOTE]
            return entry
    entry["exit_code"] = result.returncode
    entry["cells"] = [gate_cell(cell) for cell in payload.get("cells", [])]
    return entry


def status_of_gate(entries: Sequence[Mapping[str, Any]]) -> str:
    """`pass` when every cell is OK or SKIP (a cell the gate itself does not run on
    the fixture, with the reason in its note), `fail` on a FAIL or ERROR cell,
    `error` without cells."""
    if not entries or any("error" in entry or not entry.get("cells") for entry in entries):
        return "error"
    bad = any(cell["status"] not in ("OK", "SKIP") for entry in entries for cell in entry["cells"])
    return "fail" if bad else "pass"


def run_metal_gate(
    root: Path,
    vmaf: str,
    fixtures: Sequence[Mapping[str, Any]],
    config: Mapping[str, Any] | None,
    *,
    metal_status: str,
    timeout_seconds: float,
    runner: Runner = run_bounded,
) -> dict[str, Any]:
    """The gate on every fixture; `no_device` when the Metal equivalence found none."""
    if metal_status in ("no_device", "not_applicable", "not_run"):
        return {"status": metal_status}
    if config is None or not (root / GATE_SCRIPT).is_file():
        return {"status": "error", "reason": "the parity gate is not part of this bundle"}
    entries = [
        run_gate_fixture(root, vmaf, fixture, config, timeout_seconds=timeout_seconds,
                         runner=runner)
        for fixture in fixtures
    ]  # fmt: skip
    return {"status": status_of_gate(entries), "fixtures": entries}
