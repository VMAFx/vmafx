# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exit statuses of the AI scripts come back as return values, not ``sys.exit`` calls (HISS-07)."""

from __future__ import annotations

import builtins
import sys
from pathlib import Path
from typing import Any

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

import collect_gpu_calibration_data as collect  # noqa: E402

from scripts.lib.safe_subprocess import CommandTimedOut, TextCommandResult  # noqa: E402

TIMEOUT_EXIT_CODE = 124


def _block_pandas(monkeypatch: pytest.MonkeyPatch) -> None:
    real_import = builtins.__import__

    def fake_import(name: str, *args: Any, **kwargs: Any) -> Any:
        if name == "pandas":
            raise ImportError("blocked for the test")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_import)


def _row() -> collect.Row:
    return collect.Row(
        arch_id="a",
        feature_name="psnr",
        metric_name="psnr_y",
        frame_idx=0,
        raw_score_cpu=1.0,
        raw_score_gpu=1.0,
    )


def test_write_parquet_without_pandas_raises_support_error(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    _block_pandas(monkeypatch)
    with pytest.raises(collect.ParquetSupportError, match="pandas required"):
        collect.write_parquet([_row()], tmp_path / "out.parquet")


def test_write_parquet_without_rows_returns_zero(tmp_path: Path) -> None:
    assert collect.write_parquet([], tmp_path / "out.parquet") == 0


def test_main_returns_two_when_parquet_support_is_missing(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr(collect, "_validate_inputs", lambda _args: True)
    monkeypatch.setattr(collect, "_resolve_selection", lambda _args: (["psnr"], ["cuda"], None))
    monkeypatch.setattr(collect, "_collect_rows", lambda *_a, **_k: [_row()])
    _block_pandas(monkeypatch)
    rc = collect.main(
        [
            "--vmaf-binary", "vmaf",
            "--reference", "ref.yuv",
            "--distorted", "dist.yuv",
            "--width", "16",
            "--height", "16",
            "--output", str(tmp_path / "out.parquet"),
        ]
    )  # fmt: skip
    assert rc == 2
    assert "pandas required" in capsys.readouterr().err
    assert not (tmp_path / "out.manifest.json").exists()


def test_placeholder_registry_missing_is_a_file_not_found_error(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    for name in ("export_fastdvdnet_pre_placeholder", "export_transnet_v2_placeholder"):
        pytest.importorskip("torch")
        module = __import__(name)
        monkeypatch.setattr(module, "REGISTRY", tmp_path / "missing.json")
        with pytest.raises(FileNotFoundError, match="missing"):
            module._update_registry(tmp_path / "m.onnx")


def test_run_one_positive(monkeypatch: pytest.MonkeyPatch) -> None:
    """A clean ``run_one`` call returns ``(0, "")`` and passes ``VMAF_TIMEOUT_S`` to run_command."""
    recorded: dict[str, Any] = {}

    def fake_run(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        recorded.update(kwargs)
        return TextCommandResult(tuple(cmd), 0, "", "")

    monkeypatch.setattr(collect, "run_command", fake_run)
    assert collect.run_one(["vmaf", "--help"]) == (0, "")
    assert recorded.get("timeout_seconds") == collect.VMAF_TIMEOUT_S


def test_run_one_negative(monkeypatch: pytest.MonkeyPatch) -> None:
    """A failing ``run_one`` call returns its exit code with stderr, else stdout."""

    def fake_run_stderr(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        return TextCommandResult(tuple(cmd), 3, "", "boom")

    monkeypatch.setattr(collect, "run_command", fake_run_stderr)
    assert collect.run_one(["vmaf"]) == (3, "boom")

    def fake_run_stdout(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        return TextCommandResult(tuple(cmd), 3, "out", "")

    monkeypatch.setattr(collect, "run_command", fake_run_stdout)
    assert collect.run_one(["vmaf"]) == (3, "out")


def test_run_one_boundary(monkeypatch: pytest.MonkeyPatch) -> None:
    """A timed-out ``run_one`` call returns 124 and a message naming ``VMAF_TIMEOUT_S``."""

    def fake_run(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        raise CommandTimedOut(tuple(cmd), collect.VMAF_TIMEOUT_S, None, None)

    monkeypatch.setattr(collect, "run_command", fake_run)
    rc, msg = collect.run_one(["vmaf"])
    assert rc == TIMEOUT_EXIT_CODE
    assert str(collect.VMAF_TIMEOUT_S) in msg
    assert "VMAF_TIMEOUT_S" in msg
