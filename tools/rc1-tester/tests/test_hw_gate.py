# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the parity gate's Metal run in the macOS tester bundle (ADR-1496)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester import hw_gate
from vmaf_rc1_tester.safe_process import CommandResult

FIXTURE = {"id": "f1", "ref": "r", "dis": "d", "width": 1920, "height": 1080,
           "pixel_format": "420", "bitdepth": 8}  # fmt: skip
CONFIG = {
    "features": ["adm", "float_ssim"],
    "skip": [{"fixture": "f1", "features": ["float_ssim"], "reason": "scale 1 only"}],
}


def gate_root(tmp_path: Path) -> Path:
    script = tmp_path / hw_gate.GATE_SCRIPT
    script.parent.mkdir(parents=True)
    script.write_text("# stand-in\n")
    return tmp_path


def runner(status: str = "OK", write: bool = True, code: int = 0):
    seen: list[list[str]] = []

    def run(argv, **_kw):
        seen.append(list(argv))
        if write:
            cells = [{"feature": f, "status": status, "tolerance_abs": 0.0,
                      "tolerance_source": "held-exact:ADR-1496", "n_frames": 3,
                      "per_metric_max_abs_diff": {"m": 0.25 if status != "OK" else 0.0},
                      "per_metric_mismatches": {"m": 1 if status != "OK" else 0}, "note": ""}
                     for f in argv[argv.index("--features") + 1: argv.index("--workdir")]]  # fmt: skip
            Path(argv[argv.index("--json-out") + 1]).write_text(json.dumps({"cells": cells}))
        return CommandResult(code, "", "")

    run.seen = seen  # type: ignore[attr-defined]
    return run


def test_gate_runs_metal_held_exact_without_left_out_features(tmp_path: Path) -> None:
    run = runner()
    section = hw_gate.run_metal_gate(gate_root(tmp_path), "vmaf", [FIXTURE], CONFIG,
                                     metal_status="identical", timeout_seconds=1, runner=run)  # fmt: skip
    argv = run.seen[0]
    assert argv[argv.index("--backends") + 1 : argv.index("--backends") + 3] == ["cpu", "metal"]
    assert argv[argv.index("--hold-exact") + 1] == "metal"
    assert "float_ssim" not in argv and "adm" in argv
    assert section["status"] == "pass"
    entry = section["fixtures"][0]
    assert entry["left_out"] == [{"feature": "float_ssim", "reason": "scale 1 only"}]
    assert entry["cells"][0]["tolerance_source"] == "held-exact:ADR-1496"


def test_a_failing_cell_fails_the_gate(tmp_path: Path) -> None:
    section = hw_gate.run_metal_gate(gate_root(tmp_path), "vmaf", [FIXTURE], CONFIG,
                                     metal_status="differing", timeout_seconds=1,
                                     runner=runner("FAIL", code=1))  # fmt: skip
    assert section["status"] == "fail"
    assert section["fixtures"][0]["cells"][0]["max_abs_diff"] == "0.25"


def test_no_summary_is_an_error_and_no_device_skips(tmp_path: Path) -> None:
    root = gate_root(tmp_path)
    assert hw_gate.run_metal_gate(root, "vmaf", [FIXTURE], CONFIG, metal_status="identical",
                                  timeout_seconds=1, runner=runner(write=False))["status"] == "error"  # fmt: skip
    assert hw_gate.run_metal_gate(root, "vmaf", [FIXTURE], CONFIG, metal_status="no_device",
                                  timeout_seconds=1, runner=runner())["status"] == "no_device"  # fmt: skip


def test_bundle_without_the_gate_is_an_error(tmp_path: Path) -> None:
    section = hw_gate.run_metal_gate(tmp_path, "vmaf", [FIXTURE], CONFIG,
                                     metal_status="identical", timeout_seconds=1, runner=runner())  # fmt: skip
    assert section["status"] == "error"
