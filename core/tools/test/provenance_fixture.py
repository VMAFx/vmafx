# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared fixture of the provenance report tests (#2142, ADR-2073, RC4 WP5).

A synthetic 176x144 4:2:0 pair of three frames, scored by the vmaf binary under
test into a report; no external fixture is needed, so the tests run in every
build that has the CLI.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

WIDTH = 176
HEIGHT = 144
FRAMES = 3
# A model whose features accept 176x144 frames (the default reads larger ones).
MODEL = "vmaf_v0.6.1"  # vmaf-model-pin: features fit 176x144 frames


def _plane(width: int, height: int, seed: int) -> bytes:
    return bytes((x * 3 + y * 5 + seed * 7) & 0xFF for y in range(height) for x in range(width))


def write_pair(directory: Path) -> tuple[Path, Path]:
    """Reference and distorted .yuv files of FRAMES frames."""
    ref = directory / "ref.yuv"
    dist = directory / "dist.yuv"
    chroma_w, chroma_h = (WIDTH + 1) // 2, (HEIGHT + 1) // 2
    with ref.open("wb") as r, dist.open("wb") as d:
        for i in range(FRAMES):
            for out, seed in ((r, i), (d, i + 50)):
                out.write(_plane(WIDTH, HEIGHT, seed))
                out.write(_plane(chroma_w, chroma_h, seed + 1))
                out.write(_plane(chroma_w, chroma_h, seed + 2))
    return ref, dist


def score(
    vmaf: str, directory: Path, output: Path, *extra: str
) -> subprocess.CompletedProcess[str]:
    """Run the vmaf binary on the pair; `extra` selects the format and options."""
    ref, dist = write_pair(directory)
    argv = [
        vmaf,
        "-r",
        str(ref),
        "-d",
        str(dist),
        "-w",
        str(WIDTH),
        "-h",
        str(HEIGHT),
        "-p",
        "420",
        "-b",
        "8",
        "-m",
        f"version={MODEL}",
        "--quiet",
        # The CPU record: a GPU build would score on its device otherwise.
        "--backend",
        "cpu",
        "-o",
        str(output),
        *extra,
    ]
    return subprocess.run(  # noqa: S603 -- the binary under test, argv built here
        argv, capture_output=True, text=True, check=False, timeout=300
    )
