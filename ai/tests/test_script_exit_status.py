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
import export_transnet_v2 as transnet  # noqa: E402

from scripts.lib.safe_subprocess import (  # noqa: E402
    CommandFailed,
    CommandTimedOut,
    TextCommandResult,
)

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
        """Fake successful command execution by recording options and returning empty output."""
        recorded.update(kwargs)
        return TextCommandResult(tuple(cmd), 0, "", "")

    monkeypatch.setattr(collect, "run_command", fake_run)
    assert collect.run_one(["vmaf", "--help"]) == (0, "")
    assert recorded.get("timeout_seconds") == collect.VMAF_TIMEOUT_S


def test_run_one_negative(monkeypatch: pytest.MonkeyPatch) -> None:
    """A failing ``run_one`` call returns its exit code with stderr, else stdout."""

    def fake_run_stderr(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        """Fake failed command execution by returning status three and a standard error message."""
        return TextCommandResult(tuple(cmd), 3, "", "boom")

    monkeypatch.setattr(collect, "run_command", fake_run_stderr)
    assert collect.run_one(["vmaf"]) == (3, "boom")

    def fake_run_stdout(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        """Fake failed command execution by returning status three and standard output text."""
        return TextCommandResult(tuple(cmd), 3, "out", "")

    monkeypatch.setattr(collect, "run_command", fake_run_stdout)
    assert collect.run_one(["vmaf"]) == (3, "out")


def test_run_one_boundary(monkeypatch: pytest.MonkeyPatch) -> None:
    """A timed-out ``run_one`` call returns 124 and a message naming ``VMAF_TIMEOUT_S``."""

    def fake_run(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        """Fake command execution by raising CommandTimedOut with the collection timeout."""
        raise CommandTimedOut(tuple(cmd), collect.VMAF_TIMEOUT_S, None, None)

    monkeypatch.setattr(collect, "run_command", fake_run)
    rc, msg = collect.run_one(["vmaf"])
    assert rc == TIMEOUT_EXIT_CODE
    assert str(collect.VMAF_TIMEOUT_S) in msg
    assert "VMAF_TIMEOUT_S" in msg


def test_export_transnet_v2_convert_to_onnx_positive(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """_convert_to_onnx passes sys.executable allowlist, SCRIPT_TIMEOUT_S, and expected argv."""
    captured_cmd: list[str] = []
    captured_kw: dict[str, Any] = {}

    def fake_run(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        """Fake successful conversion by recording its arguments and returning empty output."""
        captured_cmd.extend(cmd)
        captured_kw.update(kwargs)
        return TextCommandResult(tuple(cmd), 0, "", "")

    monkeypatch.setattr(transnet, "run_command", fake_run)
    wrapped_dir = tmp_path / "w"
    onnx_path = tmp_path / "out.onnx"
    transnet._convert_to_onnx(wrapped_dir, onnx_path, 17)

    assert captured_kw.get("allowed_executables") == (sys.executable,)
    assert captured_kw.get("timeout_seconds") == transnet.SCRIPT_TIMEOUT_S
    assert captured_kw.get("check") is True
    assert captured_cmd[:3] == [sys.executable, "-m", "tf2onnx.convert"]
    assert "--saved-model" in captured_cmd
    assert str(wrapped_dir) in captured_cmd
    assert "--output" in captured_cmd
    assert str(onnx_path) in captured_cmd
    assert "--opset" in captured_cmd
    assert "17" in captured_cmd


def test_export_transnet_v2_main_command_failed_negative(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """CommandFailed in export_transnet_v2 main exits cleanly with a string message."""
    monkeypatch.setattr(transnet, "_verify_upstream", lambda _path: None)
    monkeypatch.setattr(transnet, "_wrap_to_savedmodel", lambda _up, _wr: None)

    def fake_run_failed(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        """Fake conversion by raising CommandFailed with a conversion error message."""
        raise CommandFailed(TextCommandResult(tuple(cmd), 1, "", "convert failed"))

    monkeypatch.setattr(transnet, "run_command", fake_run_failed)

    cli_args = [
        "--upstream-dir",
        str(tmp_path / "upstream"),
        "--output",
        str(tmp_path / "out.onnx"),
        "--wrapped-savedmodel",
        str(tmp_path / "wrapped"),
        "--skip-verify",
        "--no-registry",
    ]
    with pytest.raises(SystemExit) as exc_info:
        transnet.main(cli_args)

    assert isinstance(exc_info.value.code, str)
    assert "command exited 1" in exc_info.value.code


def test_export_transnet_v2_main_command_timed_out_boundary(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """CommandTimedOut in export_transnet_v2 main exits with message containing timeout."""
    monkeypatch.setattr(transnet, "_verify_upstream", lambda _path: None)
    monkeypatch.setattr(transnet, "_wrap_to_savedmodel", lambda _up, _wr: None)

    def fake_run_timeout(cmd: list[str], **kwargs: Any) -> TextCommandResult:
        """Fake conversion by raising CommandTimedOut with the script timeout."""
        raise CommandTimedOut(tuple(cmd), transnet.SCRIPT_TIMEOUT_S, None, None)

    monkeypatch.setattr(transnet, "run_command", fake_run_timeout)

    cli_args = [
        "--upstream-dir",
        str(tmp_path / "upstream"),
        "--output",
        str(tmp_path / "out.onnx"),
        "--wrapped-savedmodel",
        str(tmp_path / "wrapped"),
        "--skip-verify",
        "--no-registry",
    ]
    with pytest.raises(SystemExit) as exc_info:
        transnet.main(cli_args)

    assert isinstance(exc_info.value.code, str)
    assert "timed out after 1800s" in str(exc_info.value.code)
