# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.data.scores` — teacher VMAF score loader.

Covers ``TeacherScores`` round-trip, ``resolve_teacher_model`` precedence,
``_run_vmaf_score`` argv composition (mocked subprocess), and
``teacher_scores`` happy + sad paths (missing binary, missing model,
absent pooled metrics, NaN handling).
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from unittest.mock import MagicMock, patch

import numpy as np
import pytest

from ai.data import scores as scores_mod

# ---------------------------------------------------------------------------
# TeacherScores dataclass
# ---------------------------------------------------------------------------


def test_teacher_scores_to_jsonable_round_trip() -> None:
    arr = np.array([1.0, 2.5, 99.7], dtype=np.float32)
    ts = scores_mod.TeacherScores(per_frame=arr, pooled=50.4)

    payload = ts.to_jsonable()
    assert payload["pooled"] == pytest.approx(50.4)
    assert payload["per_frame"] == [1.0, 2.5, pytest.approx(99.7)]


def test_teacher_scores_from_jsonable_yields_float32() -> None:
    payload = {"per_frame": [10.0, 20.0, 30.0], "pooled": 20.0}
    ts = scores_mod.TeacherScores.from_jsonable(payload)

    assert ts.per_frame.dtype == np.float32
    assert ts.pooled == pytest.approx(20.0)
    np.testing.assert_allclose(ts.per_frame, [10.0, 20.0, 30.0])


def test_teacher_scores_round_trip_preserves_values() -> None:
    original = scores_mod.TeacherScores(
        per_frame=np.array([0.0, 12.34, 56.78], dtype=np.float32),
        pooled=23.45,
    )
    payload = original.to_jsonable()
    restored = scores_mod.TeacherScores.from_jsonable(payload)
    np.testing.assert_allclose(restored.per_frame, original.per_frame)
    assert restored.teacher_model == original.teacher_model


# ---------------------------------------------------------------------------
# Teacher model resolution & single-source mirror
# ---------------------------------------------------------------------------


def test_resolve_teacher_model_default_matches_mirror(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("VMAF_MODEL_PATH", raising=False)
    resolved = scores_mod.resolve_teacher_model()
    assert resolved.name == scores_mod.DEFAULT_MODEL
    assert resolved.arg == f"version={scores_mod.DEFAULT_MODEL}"
    assert not resolved.is_path


def test_resolve_teacher_model_path_override(tmp_path: Path) -> None:
    custom = tmp_path / "custom" / "model.json"
    resolved = scores_mod.resolve_teacher_model(custom)
    assert resolved.arg == f"path={custom}"
    assert resolved.name == "model"
    assert resolved.is_path


def test_resolve_teacher_model_env_override(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    custom = tmp_path / "env_override.json"
    monkeypatch.setenv("VMAF_MODEL_PATH", str(custom))
    resolved = scores_mod.resolve_teacher_model()
    assert resolved.arg == f"path={custom}"
    assert resolved.name == "env_override"
    assert resolved.is_path


def test_resolve_teacher_model_version_string() -> None:
    resolved = scores_mod.resolve_teacher_model("version=vmaf_v0.6.1")
    assert resolved.arg == "version=vmaf_v0.6.1"
    assert resolved.name == "vmaf_v0.6.1"
    assert not resolved.is_path


def test_teacher_scores_from_jsonable_legacy_payload_is_unknown_teacher() -> None:
    # A cache payload written before ADR-1173 carries no teacher_model; it
    # must surface as "unknown", never be relabelled as the current default.
    payload = {"per_frame": [1.0, 2.0], "pooled": 1.5}
    ts = scores_mod.TeacherScores.from_jsonable(payload)
    assert ts.teacher_model == scores_mod.TEACHER_UNKNOWN
    assert ts.teacher_model != scores_mod.DEFAULT_MODEL


def test_scores_module_carries_no_hardcoded_default_model_path() -> None:
    # ADR-1168 / ADR-1173: the default teacher has exactly one source
    # (vmaftune.defaultmodel.DEFAULT_MODEL); ai/ must not duplicate it.
    assert not hasattr(scores_mod, "DEFAULT_MODEL_PATH")
    assert not hasattr(scores_mod, "_model_path")


def test_resolve_teacher_model_directory_ignored(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    model_dir = tmp_path / "models"
    model_dir.mkdir()
    monkeypatch.setenv("VMAF_MODEL_PATH", str(model_dir))
    resolved = scores_mod.resolve_teacher_model()
    assert resolved.name == scores_mod.DEFAULT_MODEL
    assert resolved.arg == f"version={scores_mod.DEFAULT_MODEL}"
    assert not resolved.is_path


def test_run_vmaf_score_default_passes_version_arg(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.delenv("VMAF_MODEL_PATH", raising=False)
    fake_binary = tmp_path / "vmaf"
    fake_binary.write_text("")
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 64)
    dis.write_bytes(b"\x00" * 64)

    captured_argv: list[list[str]] = []

    def fake_run(cmd, **kw):
        captured_argv.append(list(cmd))
        out_path = Path(cmd[cmd.index("-o") + 1])
        out_path.write_text(json.dumps({"frames": [], "pooled_metrics": {}}))
        return subprocess.CompletedProcess(cmd, returncode=0, stdout="", stderr="")

    with patch.object(scores_mod.subprocess, "run", side_effect=fake_run):
        scores_mod._run_vmaf_score(
            fake_binary,
            ref,
            dis,
            320,
            240,
            pix_fmt="420",
            bitdepth=8,
        )

    assert captured_argv, "subprocess.run not invoked"
    argv = captured_argv[0]
    assert argv[argv.index("-m") + 1] == f"version={scores_mod.DEFAULT_MODEL}"


# ---------------------------------------------------------------------------
# _run_vmaf_score subprocess argv
# ---------------------------------------------------------------------------


def test_run_vmaf_score_composes_argv(tmp_path: Path) -> None:
    fake_binary = tmp_path / "vmaf"
    fake_binary.write_text("")  # contents irrelevant under mock
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 64)
    dis.write_bytes(b"\x00" * 64)
    model = tmp_path / "model.json"
    model.write_text("{}")

    captured_argv: list[list[str]] = []

    def fake_run(cmd, **kw):
        captured_argv.append(list(cmd))
        # The function reads <out_path>.json — write a stub.
        out_path = Path(cmd[cmd.index("-o") + 1])
        out_path.write_text(json.dumps({"frames": [], "pooled_metrics": {}}))
        return subprocess.CompletedProcess(cmd, returncode=0, stdout="", stderr="")

    with patch.object(scores_mod.subprocess, "run", side_effect=fake_run):
        doc = scores_mod._run_vmaf_score(
            fake_binary,
            ref,
            dis,
            320,
            240,
            pix_fmt="420",
            bitdepth=8,
            model=model,
        )

    assert captured_argv, "subprocess.run not invoked"
    argv = captured_argv[0]
    assert argv[0] == str(fake_binary)
    assert "-r" in argv and argv[argv.index("-r") + 1] == str(ref)
    assert "-d" in argv and argv[argv.index("-d") + 1] == str(dis)
    assert argv[argv.index("-w") + 1] == "320"
    assert argv[argv.index("-h") + 1] == "240"
    assert argv[argv.index("-p") + 1] == "420"
    assert argv[argv.index("-b") + 1] == "8"
    assert argv[argv.index("-m") + 1] == f"path={model}"
    assert doc == {"frames": [], "pooled_metrics": {}}


def test_run_vmaf_score_cleans_up_output_on_subprocess_failure(tmp_path: Path) -> None:
    fake_binary = tmp_path / "vmaf"
    fake_binary.write_text("")
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00")
    dis.write_bytes(b"\x00")
    model = tmp_path / "model.json"
    model.write_text("{}")

    leftover: list[Path] = []

    def fake_run(cmd, **kw):
        leftover.append(Path(cmd[cmd.index("-o") + 1]))
        raise subprocess.CalledProcessError(returncode=2, cmd=cmd)

    with (
        patch.object(scores_mod.subprocess, "run", side_effect=fake_run),
        pytest.raises(subprocess.CalledProcessError),
    ):
        scores_mod._run_vmaf_score(
            fake_binary,
            ref,
            dis,
            16,
            16,
            pix_fmt="420",
            bitdepth=8,
            model=model,
        )
    # The temp output file must have been removed in the finally clause.
    assert leftover and not leftover[0].exists()


# ---------------------------------------------------------------------------
# teacher_scores high-level entry point
# ---------------------------------------------------------------------------


def _stub_vmaf_doc(per_frame_vmaf: list[float | None], pooled_mean: float | None) -> dict:
    frames = []
    for i, v in enumerate(per_frame_vmaf):
        m = {} if v is None else {"vmaf": v}
        frames.append({"frameNum": i, "metrics": m})
    doc: dict = {"frames": frames}
    if pooled_mean is not None:
        doc["pooled_metrics"] = {"vmaf": {"mean": pooled_mean}}
    else:
        doc["pooled_metrics"] = {}
    return doc


def test_teacher_scores_raises_when_model_missing(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    binary.chmod(0o755)
    model_missing = tmp_path / "nope.json"

    with (
        patch.object(scores_mod, "default_vmaf_binary", return_value=binary),
        patch.object(scores_mod, "_ensure_binary", lambda b: None),
        pytest.raises(FileNotFoundError, match="VMAF model JSON not found"),
    ):
        scores_mod.teacher_scores(
            tmp_path / "r.yuv",
            tmp_path / "d.yuv",
            16,
            16,
            model=model_missing,
        )


def test_teacher_scores_happy_path(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    model = tmp_path / "model.json"
    model.write_text("{}")

    doc = _stub_vmaf_doc([90.0, 91.0, 92.0], pooled_mean=91.0)
    with (
        patch.object(scores_mod, "_ensure_binary", lambda b: None),
        patch.object(scores_mod, "_run_vmaf_score", return_value=doc),
    ):
        ts = scores_mod.teacher_scores(
            tmp_path / "r.yuv",
            tmp_path / "d.yuv",
            16,
            16,
            vmaf_binary=binary,
            model=model,
        )
    assert ts.pooled == pytest.approx(91.0)
    np.testing.assert_allclose(ts.per_frame, [90.0, 91.0, 92.0])


def test_teacher_scores_falls_back_to_nanmean_when_pooling_missing(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    model = tmp_path / "model.json"
    model.write_text("{}")

    doc = _stub_vmaf_doc([80.0, 82.0, 84.0], pooled_mean=None)
    with (
        patch.object(scores_mod, "_ensure_binary", lambda b: None),
        patch.object(scores_mod, "_run_vmaf_score", return_value=doc),
    ):
        ts = scores_mod.teacher_scores(
            tmp_path / "r.yuv",
            tmp_path / "d.yuv",
            16,
            16,
            vmaf_binary=binary,
            model=model,
        )
    assert ts.pooled == pytest.approx(82.0)


def test_teacher_scores_handles_none_vmaf_as_nan(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    model = tmp_path / "model.json"
    model.write_text("{}")

    doc = _stub_vmaf_doc([90.0, None, 92.0], pooled_mean=91.0)
    with (
        patch.object(scores_mod, "_ensure_binary", lambda b: None),
        patch.object(scores_mod, "_run_vmaf_score", return_value=doc),
    ):
        ts = scores_mod.teacher_scores(
            tmp_path / "r.yuv",
            tmp_path / "d.yuv",
            16,
            16,
            vmaf_binary=binary,
            model=model,
        )
    assert np.isnan(ts.per_frame[1])
    assert ts.per_frame[0] == pytest.approx(90.0)
    assert ts.per_frame[2] == pytest.approx(92.0)


def test_teacher_scores_uses_default_binary_when_unspecified(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    model = tmp_path / "model.json"
    model.write_text("{}")

    doc = _stub_vmaf_doc([100.0], pooled_mean=100.0)
    default_called = MagicMock(return_value=binary)
    with (
        patch.object(scores_mod, "default_vmaf_binary", default_called),
        patch.object(scores_mod, "_ensure_binary", lambda b: None),
        patch.object(scores_mod, "_run_vmaf_score", return_value=doc),
    ):
        scores_mod.teacher_scores(
            tmp_path / "r.yuv",
            tmp_path / "d.yuv",
            16,
            16,
            model=model,
        )
    default_called.assert_called_once()
