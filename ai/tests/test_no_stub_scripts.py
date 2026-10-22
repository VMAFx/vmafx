# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Stub scripts do not come back, and the placeholder generators are real.

``gen_calibration.py`` and ``quantize_int8.py`` printed "not yet implemented"
and exited 1 while ``vmaf-train quantize-int8`` already did the work (static
PTQ calibrated from a parquet cache). A stub that looks like an entry point
sends a reader to a command that cannot run, so they are removed and this
test keeps them removed. The same holds for the eight further stubs that had
no implementation behind them (a LOSO evaluator, a benchmark, corpus
converters, trainers). The two placeholder-model generators
(``gen_dists_sq_placeholder_onnx.py``, ``gen_mobilesal_placeholder_onnx.py``)
are implemented and must rebuild the committed ONNX files byte for byte,
and they exist once: the older copies under ``scripts/`` are removed.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
from types import ModuleType

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "ai" / "scripts"
REMOVED = (
    "gen_calibration.py",
    "quantize_int8.py",
    "build_calibration_set.py",
    "eval_loso_fr_regressor_v2.py",
    "external_benchmark_pvmaf.py",
    "fetch_lsvq.py",
    "gen_ssimulacra2_eotf_lut.py",
    "hdrsdr_vqa_to_corpus_jsonl.py",
    "my_corpus_to_corpus_jsonl.py",
    "train_fr_regressor_v4.py",
    "train_video_saliency_student.py",
)
GENERATORS = {
    "gen_dists_sq_placeholder_onnx.py": "dists_sq.onnx",
    "gen_mobilesal_placeholder_onnx.py": "mobilesal.onnx",
}
STUB_MARKER = "not yet implemented"


def stub_scripts(directory: Path) -> list[str]:
    """Names of the ``*.py`` files under ``directory`` that announce themselves as stubs."""
    return sorted(p.name for p in directory.glob("*.py") if STUB_MARKER in p.read_text().lower())


def load_generator(name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name[:-3], SCRIPTS / name)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_scripts_directory_is_populated() -> None:
    assert len(list(SCRIPTS.glob("*.py"))) > 20


def test_removed_stubs_stay_removed() -> None:
    assert [n for n in REMOVED if (SCRIPTS / n).exists()] == []


def test_placeholder_generators_have_one_copy() -> None:
    """The generators live in ``ai/scripts/`` only; a second copy under
    ``scripts/`` rewrote ``model/tiny/registry.json`` with fields dropped."""
    top = REPO_ROOT / "scripts"
    assert [n for n in GENERATORS if (top / n).exists()] == []


def test_quantize_entry_point_is_the_cli_command() -> None:
    cli = (REPO_ROOT / "ai" / "src" / "vmaf_train" / "cli.py").read_text()
    assert "def quantize_int8_cmd" in cli


def test_no_script_announces_itself_as_a_stub() -> None:
    assert stub_scripts(SCRIPTS) == []


def test_stub_scan_finds_a_planted_stub(tmp_path: Path) -> None:
    (tmp_path / "ok.py").write_text("print('fine')\n")
    (tmp_path / "bad.py").write_text('print("bad.py: Not Yet Implemented.")\n')
    assert stub_scripts(tmp_path) == ["bad.py"]


def test_placeholder_generators_rebuild_the_committed_models() -> None:
    pytest.importorskip("onnx")
    for script, model in sorted(GENERATORS.items()):
        module = load_generator(script)
        committed = REPO_ROOT / "model" / "tiny" / model
        assert module.build_model_bytes() == committed.read_bytes(), script
        assert module.main(["--check", "--output", str(committed)]) == 0, script


def test_placeholder_generators_check_refuses_a_different_file(tmp_path: Path) -> None:
    pytest.importorskip("onnx")
    for script in sorted(GENERATORS):
        module = load_generator(script)
        other = tmp_path / (script + ".onnx")
        assert module.main(["--check", "--output", str(other)]) == 1, script  # missing
        other.write_bytes(module.build_model_bytes() + b"\x00")
        assert module.main(["--check", "--output", str(other)]) == 1, script  # differs
        assert module.main(["--output", str(other)]) == 0, script  # writes
        assert module.main(["--check", "--output", str(other)]) == 0, script
