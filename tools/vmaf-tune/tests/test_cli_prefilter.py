# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""``vmaf-tune prefilter`` subcommand tests (ADR-1116 / pelorus ADR-0110).

Unit-level only — no real ffmpeg / vmaf binary is invoked. The smoke
path is driven directly; the live path is exercised with the
filter-availability probe, scoring-backend selector, and the
encode/score seams all monkey-patched, so the subcommand wiring and the
deband ``-vf`` injection are covered without a pelorus-enabled ffmpeg.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

pytest.importorskip("optuna")  # gate: the loop needs the TPE sampler

from vmaftune import cli as cli_module  # noqa: E402
from vmaftune.cli import main  # noqa: E402
from vmaftune.encode import EncodeRequest, EncodeResult  # noqa: E402
from vmaftune.score import ScoreRequest, ScoreResult  # noqa: E402

# ---------------------------------------------------------------------------
# Argument validation / smoke path.
# ---------------------------------------------------------------------------


def test_smoke_subcommand_emits_json(capsys: pytest.CaptureFixture[str]) -> None:
    rc = main(
        ["prefilter", "--target-vmaf", "92", "--smoke", "--n-trials", "25", "--time-budget-s", "30"]
    )
    assert rc == 0
    out = capsys.readouterr().out
    payload = json.loads(out)
    assert payload["smoke"] is True
    assert payload["filter_name"] == "pelorus_deband_vulkan"
    assert 18 <= payload["recommended_crf"] <= 40
    assert payload["recommended_vf"].startswith("pelorus_deband_vulkan=")
    assert len(payload["probes"]) == payload["n_trials"]


def test_smoke_writes_output_file(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    out_path = tmp_path / "rec.json"
    rc = main(
        [
            "prefilter",
            "--target-vmaf",
            "90",
            "--smoke",
            "--n-trials",
            "15",
            "--output",
            str(out_path),
        ]
    )
    assert rc == 0
    payload = json.loads(out_path.read_text())
    assert payload["recommended_vf"].startswith("pelorus_deband_vulkan")


def test_bad_crf_range_rejected(capsys: pytest.CaptureFixture[str]) -> None:
    rc = main(["prefilter", "--target-vmaf", "90", "--smoke", "--crf-min", "40", "--crf-max", "18"])
    assert rc == 2
    assert "invalid CRF range" in capsys.readouterr().err


def test_subset_sweep_flag(capsys: pytest.CaptureFixture[str]) -> None:
    rc = main(
        [
            "prefilter",
            "--target-vmaf",
            "90",
            "--smoke",
            "--n-trials",
            "12",
            "--sweep-knob",
            "range",
            "--sweep-knob",
            "grainy",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out)
    assert set(payload["recommended_deband"]) == {"range", "grainy"}


# ---------------------------------------------------------------------------
# Live path — filter-availability gate.
# ---------------------------------------------------------------------------


def test_live_missing_src_rejected(capsys: pytest.CaptureFixture[str]) -> None:
    rc = main(["prefilter", "--target-vmaf", "90", "--width", "320", "--height", "240"])
    assert rc == 2
    assert "--src is required" in capsys.readouterr().err


def test_live_missing_geometry_rejected(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    src = tmp_path / "src.yuv"
    src.write_bytes(b"\x00" * 16)
    rc = main(["prefilter", "--target-vmaf", "90", "--src", str(src)])
    assert rc == 2
    assert "--width / --height are required" in capsys.readouterr().err


def test_live_gated_when_filter_unavailable(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    src = tmp_path / "src.yuv"
    src.write_bytes(b"\x00" * 16)
    monkeypatch.setattr(cli_module, "pelorus_filter_available", lambda *a, **k: False)
    rc = main(
        ["prefilter", "--target-vmaf", "90", "--src", str(src), "--width", "320", "--height", "240"]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert "not available in this ffmpeg build" in err


# ---------------------------------------------------------------------------
# Live path — full mocked encode/score loop.
# ---------------------------------------------------------------------------


def _fake_encode_result(req: EncodeRequest) -> EncodeResult:
    return EncodeResult(
        request=req,
        encode_size_bytes=500_000,
        encode_time_ms=10.0,
        encoder_version="x264-test",
        ffmpeg_version="n8.1-test",
        exit_status=0,
        stderr_tail="",
    )


def _fake_score_result(req: ScoreRequest) -> ScoreResult:
    return ScoreResult(
        request=req,
        vmaf_score=91.0,
        score_time_ms=5.0,
        vmaf_binary_version="vmaf-test",
        exit_status=0,
        stderr_tail="",
    )


def test_live_loop_runs_with_mocked_encode_and_score(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """The live loop wires deband -vf injection through encode + score.

    All external binaries are mocked: ``pelorus_filter_available`` says
    yes, ``select_backend`` returns cpu, and ``run_encode`` /
    ``run_score`` are fakes. We assert the deband ``-vf`` fragment is
    injected into every EncodeRequest's extra_params.
    """
    src = tmp_path / "src.yuv"
    src.write_bytes(b"\x00" * 16)

    captured_vf: list[str] = []

    def _fake_run_encode(req, *, ffmpeg_bin="ffmpeg"):  # noqa: ANN001
        # The deband fragment must be injected as "-vf <fragment>".
        ep = list(req.extra_params)
        assert "-vf" in ep, "deband -vf fragment must be injected into the encode"
        vf = ep[ep.index("-vf") + 1]
        assert vf.startswith("pelorus_deband_vulkan")
        captured_vf.append(vf)
        # Materialise the output so the slot.exists() guard passes.
        Path(req.output).parent.mkdir(parents=True, exist_ok=True)
        Path(req.output).write_bytes(b"\x00" * 32)
        return _fake_encode_result(req)

    def _fake_run_score(req, *, vmaf_bin="vmaf", backend=None):  # noqa: ANN001
        return _fake_score_result(req)

    monkeypatch.setattr(cli_module, "pelorus_filter_available", lambda *a, **k: True)
    monkeypatch.setattr(cli_module, "select_backend", lambda **k: "cpu")
    # Patch the seams the probe imports lazily from the package modules.
    import vmaftune.encode as encode_mod
    import vmaftune.score as score_mod

    monkeypatch.setattr(encode_mod, "run_encode", _fake_run_encode)
    monkeypatch.setattr(score_mod, "run_score", _fake_run_score)

    rc = main(
        [
            "prefilter",
            "--target-vmaf",
            "91",
            "--src",
            str(src),
            "--width",
            "320",
            "--height",
            "240",
            "--duration",
            "4.0",
            "--n-trials",
            "6",
            "--encode-dir",
            str(tmp_path / "enc"),
            "--time-budget-s",
            "30",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["smoke"] is False
    assert payload["achieved_vmaf"] == pytest.approx(91.0)
    # achieved_kbps must be kbits-PER-SECOND, not kbits: with the fake encode
    # reporting 500_000 bytes over --duration 4.0 s, the bitrate is
    # 500000*8/1000/4 = 1000.0 kbps. The pre-fix code emitted bare kbits
    # (500000*8/1000 = 4000.0), so this assertion pins the kbps unit.
    assert payload["achieved_kbps"] == pytest.approx(1000.0)
    # At least one probe ran, and every probe injected a deband fragment.
    assert len(captured_vf) >= 1
    assert all(v.startswith("pelorus_deband_vulkan") for v in captured_vf)
    # The recommended deband round-trips into the recommended -vf.
    assert payload["recommended_vf"].startswith("pelorus_deband_vulkan")
