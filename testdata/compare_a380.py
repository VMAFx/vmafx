#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Compare SYCL A380 scores frame-by-frame against CPU golden.
Shows per-frame diffs for frames with diff > 0.0001.
"""

import json
from pathlib import Path

BASEDIR = Path(__file__).resolve().parent
FRAME_DIFF_THRESHOLD = 0.0001

resolutions = [
    ("576x324", "576"),
    ("640x480", "640"),
    ("1280x720", "720"),
    ("1920x1080", "1080"),
    ("3840x2160", "4k"),
]

for dims, tag in resolutions:
    cpu_f = BASEDIR / f"scores_cpu_{tag}.json"
    sycl_f = BASEDIR / f"scores_sycl_a380_{tag}.json"
    if not (cpu_f.exists() and sycl_f.exists()):
        continue

    with cpu_f.open(encoding="utf-8") as f:
        cpu = json.load(f)
    with sycl_f.open(encoding="utf-8") as f:
        sycl = json.load(f)

    cpu_s = [fr["metrics"]["vmaf"] for fr in cpu["frames"]]
    sycl_s = [fr["metrics"]["vmaf"] for fr in sycl["frames"]]
    diffs = [abs(a - b) for a, b in zip(cpu_s, sycl_s, strict=True)]
    max_diff = max(diffs)
    avg_diff = sum(diffs) / len(diffs)
    cpu_fps = cpu.get("fps", 0)
    sycl_fps = sycl.get("fps", 0)

    print(f"\n=== {dims} ===")
    print(f'  CPU:  pooled={cpu["pooled_metrics"]["vmaf"]["mean"]:.6f}  fps={cpu_fps:.1f}')
    print(f'  SYCL: pooled={sycl["pooled_metrics"]["vmaf"]["mean"]:.6f}  fps={sycl_fps:.1f}')
    print(f"  Max diff: {max_diff:.9f}  Avg diff: {avg_diff:.9f}")
    print(f"  Speedup: {sycl_fps/cpu_fps:.1f}x" if cpu_fps > 0 else "")

    # Show worst frames
    worst = sorted(range(len(diffs)), key=lambda i: -diffs[i])[:5]
    for i in worst:
        if diffs[i] > FRAME_DIFF_THRESHOLD:
            print(f"    frame {i}: CPU={cpu_s[i]:.6f} SYCL={sycl_s[i]:.6f} diff={diffs[i]:.9f}")
