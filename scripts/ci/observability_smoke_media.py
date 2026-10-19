#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Write the media of the Compose observability smoke test.

Reads a raw 576x324 8-bit 4:2:0 reference and distorted pair and writes into
the output directory ref.yuv and dis.yuv (raw, for ScoreStream) and ref.y4m
and dis.y4m (the same frames with a YUV4MPEG2 header, for the file-based
Score requests and controller jobs, which take no geometry). Files are
world-readable: the containers run as an unprivileged user.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

WIDTH, HEIGHT = 576, 324
FRAME = WIDTH * HEIGHT * 3 // 2
HEADER = f"YUV4MPEG2 W{WIDTH} H{HEIGHT} F25:1 Ip A1:1 C420jpeg\n".encode()
ARGC = 4  # program, reference, distorted, output directory


def write_y4m(raw: bytes, out: Path) -> None:
    """Write raw 4:2:0 frames as a Y4M file."""
    if len(raw) % FRAME:
        raise SystemExit(f"{out.name}: {len(raw)} bytes is no whole number of frames")
    with out.open("wb") as fh:
        fh.write(HEADER)
        for start in range(0, len(raw), FRAME):
            fh.write(b"FRAME\n")
            fh.write(raw[start : start + FRAME])
    out.chmod(0o644)


def main(argv: list[str]) -> int:
    if len(argv) != ARGC:
        print(f"usage: {argv[0]} REF.yuv DIS.yuv OUTDIR", file=sys.stderr)
        return 2
    out = Path(argv[3])
    for src, name in ((Path(argv[1]), "ref"), (Path(argv[2]), "dis")):
        raw_out = out / f"{name}.yuv"
        shutil.copyfile(src, raw_out)
        raw_out.chmod(0o644)
        write_y4m(src.read_bytes(), out / f"{name}.y4m")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
