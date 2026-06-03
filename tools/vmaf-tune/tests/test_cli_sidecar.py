# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""CLI surface tests for ``vmaf-tune sidecar``.

The sidecar model already has unit tests in ``test_sidecar.py``. This
file pins the operator-facing argparse wiring: status, prediction, and
recording captures from JSON / JSONL without touching the user's real
cache directory.
"""

from __future__ import annotations

import io
import json
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import cli as cli_module  # noqa: E402
from vmaftune.cli import main  # noqa: E402

_FEATURES = {
    "probe_bitrate_kbps": 3000.0,
    "probe_i_frame_avg_bytes": 10000.0,
    "probe_p_frame_avg_bytes": 2000.0,
    "probe_b_frame_avg_bytes": 1000.0,
    "saliency_mean": 0.3,
    "saliency_var": 0.05,
    "frame_diff_mean": 2.5,
    "y_avg": 128.0,
    "y_var": 400.0,
    "shot_length_frames": 120,
    "fps": 24.0,
    "width": 1920,
    "height": 1080,
}


def _capture_help(argv: list[str]) -> str:
    buf = io.StringIO()
    with patch("sys.stdout", buf), pytest.raises(SystemExit) as exc:
        main(argv)
    assert exc.value.code == 0
    return buf.getvalue()


def test_sidecar_subparser_is_registered() -> None:
    parser = cli_module._build_parser()
    sub_actions = [a for a in parser._actions if hasattr(a, "choices") and a.choices]
    choices = {name for action in sub_actions for name in (action.choices or {})}
    assert "sidecar" in choices


def test_sidecar_help_lists_operator_commands() -> None:
    help_text = _capture_help(["sidecar", "--help"])
    for command in ("status", "predict", "record", "batch-record"):
        assert command in help_text


def test_sidecar_status_json_uses_requested_cache(
    tmp_path: Path, capsys: pytest.CaptureFixture
) -> None:
    cache_dir = tmp_path / "cache"
    rc = main(
        [
            "sidecar",
            "status",
            "--codec",
            "libx264",
            "--cache-dir",
            str(cache_dir),
            "--json",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["schema"] == "vmaf-tune-sidecar-status/v1"
    assert payload["codec"] == "libx264"
    assert payload["n_updates"] == 0
    assert payload["state_path"].startswith(str(cache_dir))
    assert (cache_dir / "host-uuid").is_file()


def test_sidecar_record_then_predict_applies_correction(
    tmp_path: Path, capsys: pytest.CaptureFixture
) -> None:
    cache_dir = tmp_path / "cache"
    features_path = tmp_path / "features.json"
    features_path.write_text(json.dumps(_FEATURES), encoding="utf-8")

    record_rc = main(
        [
            "sidecar",
            "record",
            "--codec",
            "libx264",
            "--cache-dir",
            str(cache_dir),
            "--features-json",
            str(features_path),
            "--crf",
            "28",
            "--observed-vmaf",
            "99.0",
            "--json",
        ]
    )
    assert record_rc == 0
    record_payload = json.loads(capsys.readouterr().out)
    assert record_payload["schema"] == "vmaf-tune-sidecar-record/v1"
    assert record_payload["n_updates"] == 1
    assert Path(record_payload["state_path"]).is_file()

    predict_rc = main(
        [
            "sidecar",
            "predict",
            "--codec",
            "libx264",
            "--cache-dir",
            str(cache_dir),
            "--features-json",
            str(features_path),
            "--crf",
            "28",
            "--json",
        ]
    )
    assert predict_rc == 0
    predict_payload = json.loads(capsys.readouterr().out)
    assert predict_payload["schema"] == "vmaf-tune-sidecar-predict/v1"
    assert predict_payload["n_updates"] == 1
    assert predict_payload["correction"] != pytest.approx(0.0)


def test_sidecar_batch_record_accepts_features_wrapper(
    tmp_path: Path, capsys: pytest.CaptureFixture
) -> None:
    captures = tmp_path / "captures.jsonl"
    rows = [
        {"features": _FEATURES, "crf": 26, "observed_vmaf": 96.0},
        {
            "features": {**_FEATURES, "probe_bitrate_kbps": 3600.0},
            "crf": 28,
            "observed_vmaf": 94.0,
        },
    ]
    captures.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")

    rc = main(
        [
            "sidecar",
            "batch-record",
            "--codec",
            "libx264",
            "--cache-dir",
            str(tmp_path / "cache"),
            "--captures-jsonl",
            str(captures),
            "--json",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["schema"] == "vmaf-tune-sidecar-batch-record/v1"
    assert payload["rows_recorded"] == 2
    assert payload["rows_skipped"] == 0
    assert payload["n_updates"] == 2
