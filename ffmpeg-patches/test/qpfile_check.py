#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Checks of the ``-qpfile`` option of the patched encoders (shared fix series patch 0043).

Exits 0 on success, 1 on a failure and 77 when the FFmpeg lacks libx264 or a
fixture is missing. Needs an FFmpeg built with the series and ``--enable-libx264``
(``--enable-libaom`` and ``--enable-libsvtav1`` add their checks).

By default a missing libaom-av1 / libsvtav1 encoder degrades to a
"(skipped: encoder missing)" row counted as passing, and a missing fixture or
libx264 degrades to exit 77. ``--require-encoders`` switches to the strict mode
the FFmpeg integration CI workflow uses (issue #3351: an integration build must
really exercise the encoders and must not go green by skipping them): a missing
fixture, an ``--ffmpeg`` binary that cannot be launched, or any of libx264,
libaom-av1 and libsvtav1 being absent is a hard ``FAIL`` that exits 1 before any
case runs, and the per-encoder "skipped: encoder missing" rows are never emitted.

- ``x264 applies the offsets``: a qpfile of +12 on every macroblock makes the
  libx264 encode smaller than the same encode without one, and -12 makes it
  larger (libx264 has no ``qpfile`` parameter: the option feeds x264's
  ``quant_offsets``).
- ``x264 refuses a grid that does not fit the video`` and ``x264 refuses
  aq-mode=0`` (the offsets travel through adaptive quantization): both fail at
  encoder open and say why.
- ``libsvtav1`` accepts the same file and encodes.
- ``libaom`` accepts the file only with ``-usage realtime`` and ``-cpu-used``
  7 or 8; other usage modes and lower ``-cpu-used`` levels are refused at
  encoder open (libaom ignores ROI maps otherwise), and both refusals are
  checked.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKIP = 77
WIDTH, HEIGHT, FRAMES = 576, 324, 6


def run(args: argparse.Namespace, cmd: list[str]) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    if args.libdir:
        env["LD_LIBRARY_PATH"] = args.libdir + ":" + env.get("LD_LIBRARY_PATH", "")
    return subprocess.run(  # noqa: S603 -- binary under test, argument list, no shell
        cmd, env=env, capture_output=True, text=True, timeout=600, check=False
    )


def has_encoder(args: argparse.Namespace, name: str) -> bool:
    out = run(args, [args.ffmpeg, "-hide_banner", "-encoders"]).stdout
    return any(line.split()[1:2] == [name] for line in out.splitlines() if line.startswith(" V"))


def write_qpfile(path: Path, cols: int, rows: int, offset: int) -> None:
    """One record per frame: `<frame> <type> <qp>` then `rows` lines of `cols` offsets."""
    lines = []
    for frame in range(FRAMES):
        lines.append(f"{frame} {'I' if frame == 0 else 'P'} 30")
        lines.extend(" ".join([str(offset)] * cols) for _ in range(rows))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def encode(
    args: argparse.Namespace, tmp: Path, name: str, codec: str, extra: list[str], source: Path
) -> tuple[subprocess.CompletedProcess[str], int]:
    out = tmp / f"{name}.mkv"
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p"]
    cmd += ["-s", f"{WIDTH}x{HEIGHT}", "-r", "25", "-i", str(source), "-c:v", codec, *extra]
    cmd += [str(out)]
    result = run(args, cmd)
    return result, out.stat().st_size if out.is_file() else 0


def x264_checks(args: argparse.Namespace, tmp: Path, source: Path) -> list[tuple[str, bool, str]]:
    cols, rows = (WIDTH + 15) // 16, (HEIGHT + 15) // 16
    base = ["-preset", "veryfast", "-crf", "30", "-bf", "0", "-g", "6"]
    results: list[tuple[str, bool, str]] = []
    sizes = {}
    for label, offset in (("plus", 12), ("minus", -12)):
        qp = tmp / f"{label}.qp"
        write_qpfile(qp, cols, rows, offset)
        run_, size = encode(args, tmp, label, "libx264", [*base, "-qpfile", str(qp)], source)
        sizes[label] = size
        loaded = "libx264: qpfile=" in run_.stderr
        results.append(
            (
                f"x264 accepts a qpfile ({offset:+d})",
                run_.returncode == 0 and loaded,
                run_.stderr[-300:],
            )
        )
    plain, sizes["none"] = encode(args, tmp, "none", "libx264", base, source)
    ordered = 0 < sizes["plus"] < sizes["none"] < sizes["minus"]
    results.append(
        (
            "x264 applies the offsets (+12 smaller, none, -12 larger)",
            plain.returncode == 0 and ordered,
            f"bytes: +12 {sizes['plus']}, none {sizes['none']}, -12 {sizes['minus']}",
        )
    )
    grid = tmp / "grid.qp"
    write_qpfile(grid, cols + 1, rows, 0)
    bad, _ = encode(args, tmp, "grid", "libx264", [*base, "-qpfile", str(grid)], source)
    results.append(
        (
            "x264 refuses a grid that does not fit the video",
            bad.returncode != 0 and "macroblocks" in bad.stderr,
            bad.stderr[-300:],
        )
    )
    noaq, _ = encode(
        args,
        tmp,
        "noaq",
        "libx264",
        [*base, "-x264-params", "aq-mode=0", "-qpfile", str(tmp / "plus.qp")],
        source,
    )
    results.append(
        (
            "x264 refuses aq-mode=0",
            noaq.returncode != 0 and "aq-mode" in noaq.stderr,
            noaq.stderr[-300:],
        )
    )
    return results


def libaom_checks(args: argparse.Namespace, tmp: Path, source: Path) -> list[tuple[str, bool, str]]:
    """Run the libaom-av1 qpfile cases and return (label, passed, stderr tail) rows."""
    if not has_encoder(args, "libaom-av1"):
        return [
            ("libaom-av1 accepts a qpfile (skipped: encoder missing)", True, ""),
            ("libaom-av1 accepts cpu-used 7 (skipped: encoder missing)", True, ""),
            ("libaom-av1 refuses cpu-used 6 (skipped: encoder missing)", True, ""),
            ("libaom-av1 refuses without -usage realtime (skipped: encoder missing)", True, ""),
        ]
    qp = tmp / "plus.qp"
    results: list[tuple[str, bool, str]] = []
    cases = (
        (
            "libaom-av1 accepts a qpfile",
            "rt8",
            ["-usage", "realtime", "-cpu-used", "8", "-crf", "40"],
            True,
            "",
        ),
        (
            "libaom-av1 accepts cpu-used 7",
            "rt7",
            ["-usage", "realtime", "-cpu-used", "7", "-crf", "40"],
            True,
            "",
        ),
        (
            "libaom-av1 refuses cpu-used 6",
            "rt6",
            ["-usage", "realtime", "-cpu-used", "6", "-crf", "40"],
            False,
            "needs -usage realtime",
        ),
        (
            "libaom-av1 refuses without -usage realtime",
            "good8",
            ["-cpu-used", "8", "-crf", "40"],
            False,
            "needs -usage realtime",
        ),
    )
    for label, name, extra, ok_pred, needle in cases:
        run_, size = encode(
            args, tmp, f"libaom-{name}", "libaom-av1", [*extra, "-qpfile", str(qp)], source
        )
        if ok_pred:
            passed = run_.returncode == 0 and "qpfile=" in run_.stderr and size > 0
        else:
            passed = run_.returncode != 0 and needle in run_.stderr
        results.append((label, passed, run_.stderr[-300:]))
    return results


def other_checks(args: argparse.Namespace, tmp: Path, source: Path) -> list[tuple[str, bool, str]]:
    """Run the libsvtav1 qpfile case, then the libaom-av1 cases; return the result rows."""
    results: list[tuple[str, bool, str]] = []
    qp = tmp / "plus.qp"
    if not has_encoder(args, "libsvtav1"):
        results.append(("libsvtav1 accepts a qpfile (skipped: encoder missing)", True, ""))
    else:
        run_, size = encode(
            args, tmp, "libsvtav1", "libsvtav1", ["-preset", "12", "-qpfile", str(qp)], source
        )
        loaded = "qpfile=" in run_.stderr
        results.append(
            (
                "libsvtav1 accepts a qpfile",
                run_.returncode == 0 and loaded and size > 0,
                run_.stderr[-300:],
            )
        )
    results.extend(libaom_checks(args, tmp, source))
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--libdir", default="", help="directory of the libraries FFmpeg loads")
    parser.add_argument("--yuv", default=str(ROOT / "python" / "test" / "resource" / "yuv"))
    parser.add_argument(
        "--require-encoders",
        action="store_true",
        help="strict CI mode (issue #3351): a missing fixture, ffmpeg or encoder "
        "is a FAIL exit 1 instead of a skip",
    )
    args = parser.parse_args()
    clip = Path(args.yuv) / "src01_hrc01_576x324.yuv"
    if args.require_encoders:
        if not clip.is_file():
            print(f"FAIL required fixture missing: {clip}")
            return 1
        try:
            encoders = run(args, [args.ffmpeg, "-hide_banner", "-encoders"]).stdout
        except OSError as exc:
            print(f"FAIL ffmpeg is not executable: {args.ffmpeg} ({exc.strerror or exc})")
            return 1
        present = {
            line.split()[1]
            for line in encoders.splitlines()
            if line.startswith(" V") and len(line.split()) > 1
        }
        missing = [n for n in ("libx264", "libaom-av1", "libsvtav1") if n not in present]
        for name in missing:
            print(f"FAIL required encoder missing: {name}")
        if missing:
            return 1
    if not clip.is_file() or not has_encoder(args, "libx264"):
        print("skip: the fixture or libx264 is missing")
        return SKIP
    failed = False
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        source = tmp / "clip.yuv"
        source.write_bytes(clip.read_bytes()[: WIDTH * HEIGHT * 3 // 2 * FRAMES])
        for name, passed, detail in [
            *x264_checks(args, tmp, source),
            *other_checks(args, tmp, source),
        ]:
            print(f"{'ok  ' if passed else 'FAIL'} {name}")
            failed |= not passed
            if not passed or detail.startswith("bytes"):
                print(f"     {detail}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
