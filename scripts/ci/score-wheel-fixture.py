#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Score repo fixture pair via installed vmafx Python wheel."""

from __future__ import annotations

import sys
from pathlib import Path

import vmafx

MAX_VMAF = 100.0
FIXTURE_FRAMES = 5


def find_repo_root() -> Path:
    """Find repository root containing testdata fixtures."""
    candidate = Path(__file__).resolve().parents[2]
    if (candidate / "testdata" / "ref_576x324_48f.yuv").is_file():
        return candidate
    cwd = Path.cwd()
    if (cwd / "testdata" / "ref_576x324_48f.yuv").is_file():
        return cwd
    raise FileNotFoundError("Could not find repository root containing testdata fixtures")


def score_fixture_frames(
    lib: vmafx.Library,
    ref_path: Path,
    dis_path: Path,
    n_frames: int = FIXTURE_FRAMES,
) -> float:
    """Score n_frames from ref and dis YUV fixtures."""
    model = lib.model_load("vmaf_v0.6.1")
    ctx = lib.context()
    ctx.use_model(model)

    w, h = 576, 324
    y_sz = w * h
    uv_sz = (w // 2) * (h // 2)
    frame_sz = y_sz + 2 * uv_sz
    desc = vmafx.FrameDesc(
        int(vmafx.PixelFormat.YUV420P),
        8,
        w,
        h,
        vmafx.Color(0, 0, 0, 0),
    )

    with ref_path.open("rb") as f_ref, dis_path.open("rb") as f_dis:
        for idx in range(n_frames):
            rb = f_ref.read(frame_sz)
            db = f_dis.read(frame_sz)
            if len(rb) < frame_sz or len(db) < frame_sz:
                break
            ref_f = lib.frame_wrap_host(
                desc,
                (rb[:y_sz], rb[y_sz : y_sz + uv_sz], rb[y_sz + uv_sz :]),
                (w, w // 2, w // 2),
            )
            dis_f = lib.frame_wrap_host(
                desc,
                (db[:y_sz], db[y_sz : y_sz + uv_sz], db[y_sz + uv_sz :]),
                (w, w // 2, w // 2),
            )
            ctx.submit(ref_f, dis_f, idx)

    ctx.flush()
    pooled = ctx.score_pooled(model, int(vmafx.Pool.MEAN), 0, n_frames - 1)
    return float(pooled.value)


def main() -> int:
    """Run wheel verification fixture scoring."""
    root = find_repo_root()
    ref_path = root / "testdata" / "ref_576x324_48f.yuv"
    dis_path = root / "testdata" / "dis_576x324_48f.yuv"

    lib = vmafx.Library()
    version = lib.version_string()
    print(f"Loaded vmafx library version: {version}")

    score = score_fixture_frames(lib, ref_path, dis_path, n_frames=FIXTURE_FRAMES)
    print(f"Pooled VMAF score (5 frames): {score:.4f}")
    if not (0.0 < score <= MAX_VMAF):
        print(f"ERROR: VMAF score {score} out of expected range (0, 100]", file=sys.stderr)
        return 1
    print("Verification passed successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
