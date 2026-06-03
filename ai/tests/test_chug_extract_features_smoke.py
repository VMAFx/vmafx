# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""End-to-end VMAF-floor smoke for ``ai/scripts/chug_extract_features.py``.

ADR-0510 regression guard.  The 2026-05-18 CHUG re-extract produced 5992
rows with VMAF ~99 across every bitrate-ladder rung (including 360p @
0.2 Mbps which should physically score in the 30-60 band).  The root cause
was the operator routing through ``extract_k150k_features.py`` whose
FR-from-NR adapter (ref == distorted) collapses every difference-based
metric to its identity-pair floor.

This test runs ``chug_extract_features.py`` end-to-end against a
synthetic ref/dis pair where the distorted side is deliberately
destroyed (uniform mid-grey overwriting all pixel data).  It asserts the
emitted ``adm2_mean`` is well below the identity-pair value (1.0) and
the per-frame ADM is below 0.95 — a sentinel for "VMAF would be < 90".

We assert on ``adm2`` rather than on a composite VMAF score because the
script intentionally skips the ``--model`` arg (only feature scores are
materialised; VMAF aggregation happens downstream in the trainer).  The
ADM detail-loss aggregate is the canonical sentinel for "VMAF will not
be near 100" in libvmaf's own integration tests.

Skipped if the ``vmaf`` / ``ffmpeg`` binaries are not on PATH.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "chug_extract_features.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("chug_extract_features", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _resolve_vmaf_binary() -> Path | None:
    env = os.environ.get("VMAF_BIN_FOR_TESTS")
    if env and Path(env).is_file():
        return Path(env)
    for candidate in (
        _REPO_ROOT / "build" / "tools" / "vmaf",
        _REPO_ROOT / "core" / "build-cpu" / "tools" / "vmaf",
    ):
        if candidate.is_file():
            return candidate
    onpath = shutil.which("vmaf")
    if onpath:
        return Path(onpath)
    return None


def _write_solid_grey_mp4(out: Path, *, width: int, height: int, frames: int) -> None:
    """Encode a deliberately-degraded mid-grey clip via ffmpeg lavfi."""
    cmd = [
        "ffmpeg",
        "-y",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"color=c=gray:s={width}x{height}:r=30:d={frames / 30:.3f}",
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-pix_fmt",
        "yuv420p",
        str(out),
    ]
    subprocess.run(cmd, check=True)


def _write_textured_mp4(out: Path, *, width: int, height: int, frames: int) -> None:
    """Encode a textured (mandelbrot) clip as the reference."""
    cmd = [
        "ffmpeg",
        "-y",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"mandelbrot=size={width}x{height}:rate=30",
        "-frames:v",
        str(frames),
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-pix_fmt",
        "yuv420p",
        str(out),
    ]
    subprocess.run(cmd, check=True)


def test_chug_extract_features_floor_below_identity_on_degraded_pair(
    tmp_path: Path,
) -> None:
    """ADR-0509: a deliberately-degraded distorted clip must score well below 1.0.

    Pipes a synthetic ref/dis pair through the real
    ``chug_extract_features.py`` pipeline (with the actual vmaf binary)
    and asserts the resulting ``adm2_mean`` is < 0.95, i.e. far enough
    from the identity-pair value (1.0) that the FR-from-NR regression
    cannot pass.  In the 2026-05-18 bad parquet ``adm2_mean`` was 1.0
    for every row.
    """
    vmaf_bin = _resolve_vmaf_binary()
    if vmaf_bin is None or shutil.which("ffmpeg") is None:
        pytest.skip("requires vmaf + ffmpeg binaries on PATH or in build/")

    module = _load_module()

    width, height, frames = 320, 240, 6
    clips_dir = tmp_path / "clips"
    clips_dir.mkdir()
    ref_mp4 = clips_dir / "ref.mp4"
    dis_mp4 = clips_dir / "dist.mp4"

    _write_textured_mp4(ref_mp4, width=width, height=height, frames=frames)
    _write_solid_grey_mp4(dis_mp4, width=width, height=height, frames=frames)

    rows = [
        {
            "src": "ref.mp4",
            "src_sha256": "r" * 64,
            "width": width,
            "height": height,
            "mos": 5.0,
            "chug_content_name": "synthetic.mp4",
            "chug_ref": 1,
            "chug_bitrate_label": "ref",
            "chug_bitladder": "1080p_ref_",
            "chug_video_id": "ref",
        },
        {
            "src": "dist.mp4",
            "src_sha256": "d" * 64,
            "width": width,
            "height": height,
            "mos": 1.5,
            "chug_content_name": "synthetic.mp4",
            "chug_ref": 0,
            "chug_bitrate_label": "low",
            "chug_bitladder": "low_0.1M_",
            "chug_video_id": "dis",
        },
    ]
    chug_jsonl = tmp_path / "chug.jsonl"
    chug_jsonl.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
    output = tmp_path / "features.jsonl"

    written = module.run(
        input_jsonl=chug_jsonl,
        output_jsonl=output,
        clips_dir=clips_dir,
        cache_dir=tmp_path / "cache",
        max_rows=None,
        vmaf_bin=vmaf_bin,
    )

    assert written == 1, "expected one distorted row (reference identity is opt-in)"
    row = json.loads(output.read_text(encoding="utf-8"))
    assert row["feature_alignment"] == "distorted_scaled_to_reference"
    assert row["feature_ref_src"] == "ref.mp4"
    adm2 = row.get("adm2_mean")
    assert adm2 is not None, "adm2_mean must be populated"
    assert adm2 < 0.95, (
        f"adm2_mean={adm2} is too close to the identity-pair value (1.0); "
        "ADR-0510 FR-from-NR regression suspected. Compare ref vs dis paths."
    )
