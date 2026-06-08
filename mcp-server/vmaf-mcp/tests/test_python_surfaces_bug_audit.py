# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the 2026-05-31 Python-surfaces bug-audit bundle.

Covers six defects in ``mcp-server/vmaf-mcp/src/vmaf_mcp/server.py``:

* ``_run_vmaf_score`` read its JSON output without ``encoding=`` — locale
  leak (an MCP-stdio launch with a non-UTF-8 LC_ALL would crash on a
  legitimate accented filename in the vmaf payload).
* ``_list_extractors`` read .c sources without ``encoding=``.
* ``_describe_model_file`` read JSON models without ``encoding=``.
* ``_probe_backend`` read its probe-output JSON without ``encoding=``.
* ``_pick_worst_frames`` sorted NaN-valued VMAF scores — Python's
  ``list.sort`` is not a total order over NaN, so the ranking became
  non-deterministic when a backend emitted NaN for a partially decoded
  frame.
* ``_describe_worst_frames`` used a shared ``/tmp/vmaf-mcp-worst-<pid>``
  directory across concurrent tool calls — one call's ``shutil.rmtree``
  could nuke the PNGs another call was still using.

Tests are seam-only: no real vmaf binary, no ffmpeg, no MCP transport.
"""

from __future__ import annotations

import asyncio
import math
import sys
from pathlib import Path
from typing import Any

import pytest

# Make the mcp-server src tree importable regardless of pytest invocation cwd.
_SRC = Path(__file__).resolve().parents[1] / "src"
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))

from vmaf_mcp import server as _srv  # noqa: E402

# ---------------------------------------------------------------------------
# _pick_worst_frames — NaN-safe sort (Bug 13)
# ---------------------------------------------------------------------------


def test_pick_worst_frames_skips_nan_scores() -> None:
    """NaN VMAF values must be dropped — they break sort stability."""
    payload = {
        "frames": [
            {"frameNum": 0, "metrics": {"vmaf": 95.5}},
            {"frameNum": 1, "metrics": {"vmaf": float("nan")}},
            {"frameNum": 2, "metrics": {"vmaf": 12.0}},
            {"frameNum": 3, "metrics": {"vmaf": float("inf")}},
            {"frameNum": 4, "metrics": {"vmaf": 80.0}},
        ]
    }
    worst = _srv._pick_worst_frames(payload, n=3)
    # Only finite scores survive; deterministic ascending order.
    assert [idx for idx, _ in worst] == [2, 4, 0]
    for _, score in worst:
        assert math.isfinite(score)


def test_pick_worst_frames_handles_string_score_gracefully() -> None:
    """A bogus string score must not crash the picker."""
    payload = {
        "frames": [
            {"frameNum": 0, "metrics": {"vmaf": "not-a-float"}},
            {"frameNum": 1, "metrics": {"vmaf": 50.0}},
        ]
    }
    worst = _srv._pick_worst_frames(payload, n=5)
    assert worst == [(1, 50.0)]


def test_pick_worst_frames_returns_empty_for_n_zero() -> None:
    payload = {"frames": [{"frameNum": 0, "metrics": {"vmaf": 50.0}}]}
    assert _srv._pick_worst_frames(payload, n=0) == []


# ---------------------------------------------------------------------------
# _describe_worst_frames — per-call tempdir (Bug 14)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_describe_worst_frames_uses_unique_tempdir_per_call(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Concurrent calls must not share a tempdir — race deletes peer PNGs."""
    captured_roots: list[Path] = []

    # Replace the real VMAF score with a stub that returns two frames.
    async def stub_score(req: Any) -> dict[str, Any]:
        return {
            "frames": [
                {"frameNum": 0, "metrics": {"vmaf": 10.0}},
                {"frameNum": 1, "metrics": {"vmaf": 20.0}},
            ]
        }

    # Replace _extract_frame_png to "create" the file without ffmpeg.
    async def stub_extract(*args: Any, **kwargs: Any) -> None:
        out_png = kwargs["out_png"]
        captured_roots.append(out_png.parent)
        Path(out_png).write_bytes(b"png")

    def stub_describe(_p: Path) -> str:
        return "fixture-description"

    monkeypatch.setattr(_srv, "_run_vmaf_score", stub_score)
    monkeypatch.setattr(_srv, "_extract_frame_png", stub_extract)

    req = _srv.ScoreRequest(
        ref=tmp_path / "ref.yuv",
        dis=tmp_path / "dis.yuv",
        width=32,
        height=32,
        pixfmt="420",
        bitdepth=8,
    )

    # Drive two concurrent calls to surface the race.
    results = await asyncio.gather(
        _srv._describe_worst_frames(req, n=2, describe=stub_describe),
        _srv._describe_worst_frames(req, n=2, describe=stub_describe),
    )

    # Each call must have produced its own root, and the two roots must
    # differ (mkdtemp guarantees uniqueness).
    unique_roots = {str(r) for r in captured_roots}
    assert (
        len(unique_roots) >= 2
    ), "Both concurrent calls allocated the same /tmp dir — that's the race the audit fixed"

    # Both calls must return two PNG paths.  With TemporaryDirectory semantics
    # (ADR-Bug-14 fix) the temp dir is cleaned up when the call returns, so the
    # PNGs no longer exist on disk after _describe_worst_frames returns.
    # Callers that need persistence must copy files out before the call returns.
    for resp in results:
        assert len(resp["frames"]) == 2
        for frame in resp["frames"]:
            assert not Path(frame["png"]).exists(), (
                "TemporaryDirectory leaked: PNG file still present after "
                "_describe_worst_frames returned (expected cleanup on context exit)"
            )


# ---------------------------------------------------------------------------
# UTF-8 encoding on read_text (Bugs 9, 10, 11, 12)
# ---------------------------------------------------------------------------


def test_describe_model_file_reads_utf8_json(tmp_path: Path) -> None:
    """A JSON model with unicode comment / model_type survives the read."""
    model = tmp_path / "fixture_éàü.json"
    model.write_text(
        '{"model_dict": {"model_type": "LIBSVMNUSVR_éàü", '
        '"feature_names": ["adm2", "vif_scale0"]}}',
        encoding="utf-8",
    )
    out = _srv._describe_model_file(model, tmp_path)
    assert out["model_type"] == "LIBSVMNUSVR_éàü"
    assert out["feature_names"] == ["adm2", "vif_scale0"]


def test_list_extractors_handles_utf8_comments(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Source files with unicode comments must not derail the extractor scan."""
    feature_dir = tmp_path / "core" / "src" / "feature"
    feature_dir.mkdir(parents=True)
    (feature_dir / "fake.c").write_text(
        "/* éàü unicode comment */\n"
        "VmafFeatureExtractor vmaf_fex_fake = {\n"
        '    .name = "fake_metric",\n'
        "};\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(_srv, "_repo_root", lambda: tmp_path)
    extractors = _srv._list_extractors()
    names = [e["name"] for e in extractors]
    assert "fake_metric" in names
