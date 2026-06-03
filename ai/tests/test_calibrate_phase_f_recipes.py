# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for Phase F recipe calibration report provenance."""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

# pylint: disable=wrong-import-position
import calibrate_phase_f_recipes


def test_phase_f_calibration_records_run_provenance(tmp_path: Path) -> None:
    corpus = tmp_path / "corpus.jsonl"
    out = tmp_path / "phase_f_recipes.json"
    rows = [
        {"src": "a", "width": 960, "height": 540, "mos": 2.5, "duration_s": 5.0},
        {"src": "b", "width": 960, "height": 540, "mos": 3.0, "duration_s": 5.0},
        {"src": "c", "width": 540, "height": 960, "mos": 4.0, "duration_s": 5.0},
        {"src": "d", "width": 960, "height": 540, "mos": 4.5, "duration_s": 5.0},
    ]
    corpus.write_text(
        "\n".join(json.dumps(row, sort_keys=True) for row in rows) + "\n",
        encoding="utf-8",
    )

    rc = calibrate_phase_f_recipes.main(
        [
            "--corpus",
            str(corpus),
            "--out",
            str(out),
            "--max-rows",
            "4",
        ]
    )

    assert rc == 0
    payload = json.loads(out.read_text(encoding="utf-8"))
    assert payload["recipes"]["ugc"]["_provenance"]["corpus_rows_used"] == 4
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/calibrate_phase_f_recipes.py"
    assert provenance["inputs"]["corpus"]["kind"] == "file"
    assert provenance["outputs"]["recipe_json"]["path"] == str(out)
    assert provenance["args"]["max_rows"] == 4
