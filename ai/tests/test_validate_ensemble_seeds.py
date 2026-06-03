# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for ``ai/scripts/validate_ensemble_seeds.py`` (ADR-0309).

Synthesises ``loso_seed{N}.json`` fixtures matching the schema emitted
by ``ai/scripts/train_fr_regressor_v2_ensemble_loso.py`` and verifies
that the validator emits ``PROMOTE.json`` iff the two-part gate
(mean PLCC >= 0.95 AND max-min spread <= 0.005) clears, and
``HOLD.json`` otherwise.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_VALIDATE_PATH = _REPO_ROOT / "ai" / "scripts" / "validate_ensemble_seeds.py"

# Load the script as a module without polluting ``sys.modules`` with a
# package path that doesn't exist (``ai.scripts`` is not a package).
_spec = importlib.util.spec_from_file_location("validate_ensemble_seeds_under_test", _VALIDATE_PATH)
assert _spec is not None and _spec.loader is not None
validate_ensemble_seeds = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = validate_ensemble_seeds
_spec.loader.exec_module(validate_ensemble_seeds)


def _write_seed_json(out_dir: Path, seed: int, mean_plcc: float) -> Path:
    """Write a minimal schema-correct ``loso_seed{N}.json`` fixture."""
    payload = {
        "seed": seed,
        "mean_plcc": mean_plcc,
        "per_fold": [{"source": f"src{i}", "plcc": mean_plcc} for i in range(9)],
    }
    path = out_dir / f"loso_seed{seed}.json"
    with path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh)
    return path


@pytest.fixture
def passing_loso_dir(tmp_path: Path) -> Path:
    """5 seeds, all PLCC=0.96 -> mean=0.96, spread=0.0 (gate PASS)."""
    for seed in range(5):
        _write_seed_json(tmp_path, seed, 0.96)
    return tmp_path


@pytest.fixture
def spread_failing_loso_dir(tmp_path: Path) -> Path:
    """Mean clears but spread = 0.02 > 0.005 (gate FAIL on spread)."""
    plccs = [0.95, 0.96, 0.97, 0.96, 0.95]
    plccs[2] = 0.97  # spread = 0.97 - 0.95 = 0.02
    for seed, plcc in enumerate(plccs):
        _write_seed_json(tmp_path, seed, plcc)
    return tmp_path


@pytest.fixture
def mean_failing_loso_dir(tmp_path: Path) -> Path:
    """All seeds at 0.93 -> mean=0.93 < 0.95 (gate FAIL on mean)."""
    for seed in range(5):
        _write_seed_json(tmp_path, seed, 0.93)
    return tmp_path


def test_promote_emitted_on_gate_pass(passing_loso_dir: Path) -> None:
    verdict, written = validate_ensemble_seeds.run_validation(
        loso_dir=passing_loso_dir,
        seeds=[0, 1, 2, 3, 4],
        corpus_root=passing_loso_dir,  # tmp_path; corpus snapshot tolerant
    )
    assert verdict["verdict"] == "PROMOTE"
    assert written.name == "PROMOTE.json"
    assert written.exists()
    payload = json.loads(written.read_text())
    assert payload["verdict"] == "PROMOTE"
    assert payload["gate"]["passed"] is True
    assert payload["gate"]["mean_plcc"] == pytest.approx(0.96)
    assert payload["gate"]["plcc_spread"] == pytest.approx(0.0)
    assert "flip seeds smoke->false" in payload["recommendation"]
    assert payload["adr"] == "ADR-0309"
    assert payload["parent_adr"] == "ADR-0303"
    assert payload["corpus"]["sha256"] is not None  # tmp_path exists
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/validate_ensemble_seeds.py"
    assert provenance["inputs"]["loso_dir"]["kind"] == "directory"
    assert provenance["outputs"]["verdict"]["path"].endswith("PROMOTE.json")


def test_hold_emitted_on_spread_failure(spread_failing_loso_dir: Path) -> None:
    verdict, written = validate_ensemble_seeds.run_validation(
        loso_dir=spread_failing_loso_dir,
        seeds=[0, 1, 2, 3, 4],
        corpus_root=spread_failing_loso_dir,
    )
    assert verdict["verdict"] == "HOLD"
    assert written.name == "HOLD.json"
    payload = json.loads(written.read_text())
    assert payload["gate"]["passed"] is False
    assert payload["gate"]["plcc_spread_pass"] is False
    assert payload["gate"]["mean_plcc_pass"] is True  # mean alone is fine
    assert "investigate diversity" in payload["recommendation"]


def test_hold_emitted_on_mean_failure(mean_failing_loso_dir: Path) -> None:
    verdict, written = validate_ensemble_seeds.run_validation(
        loso_dir=mean_failing_loso_dir,
        seeds=[0, 1, 2, 3, 4],
        corpus_root=mean_failing_loso_dir,
    )
    assert verdict["verdict"] == "HOLD"
    assert written.name == "HOLD.json"
    payload = json.loads(written.read_text())
    assert payload["gate"]["passed"] is False
    assert payload["gate"]["mean_plcc_pass"] is False


def test_corpus_snapshot_absent_when_root_missing(passing_loso_dir: Path, tmp_path: Path) -> None:
    """``snapshot_corpus`` returns ``present: False`` for missing roots."""
    snapshot = validate_ensemble_seeds.snapshot_corpus(tmp_path / "does_not_exist")
    assert snapshot["present"] is False
    assert snapshot["sha256"] is None


def test_main_exit_code_promote(passing_loso_dir: Path) -> None:
    rc = validate_ensemble_seeds.main([str(passing_loso_dir)])
    assert rc == 0


def test_main_exit_code_hold(mean_failing_loso_dir: Path) -> None:
    rc = validate_ensemble_seeds.main([str(mean_failing_loso_dir)])
    assert rc == 1


def test_main_exit_code_input_error(tmp_path: Path) -> None:
    """Empty dir -> FileNotFoundError on the first seed JSON -> rc 2."""
    rc = validate_ensemble_seeds.main([str(tmp_path)])
    assert rc == 2
