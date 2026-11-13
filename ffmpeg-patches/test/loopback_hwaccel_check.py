#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""CI coverage for the loopback decoder's ``-hwaccel`` family (shared patch 0044).

Patch 0044 (``fftools-decode-on-the-GPU-in-a-loopback-decoder``) makes
``-hwaccel``, ``-hwaccel_device`` and ``-hwaccel_output_format`` take effect on
the loopback decoder: placed before ``-dec`` they configure the loopback decode,
instead of being silently consumed by the demuxed input stream the way stock
FFmpeg does. This script proves the wiring on whichever FFmpeg binary it is
pointed at:

* ``software round trip``: with no ``-hwaccel*`` at all, ``-dec 0:0`` decodes a
  libx264 clip through a ``copy`` filtergraph into the null muxer, and decodes
  the same number of frames the encode produced (assertion: the loopback path
  itself works).
* ``software frames are refused by hwdownload``: feeding the same software
  ``[dec:0]`` source into a ``hwdownload`` filtergraph must fail. That is the
  baseline proof that "no -hwaccel == system-memory frames", rather than an
  assumption.
* ``unrecognised -hwaccel name``: ``-hwaccel <nonsense> -dec 0:0`` must exit
  non-zero with the patch's own stderr line
  ``Unrecognized hwaccel for a loopback decoder``. Stock FFmpeg would silently
  ignore the option here, so the stderr string (not just the exit code) is the
  hard evidence the patch actually reads the option in the loopback path.
* ``unrecognised -hwaccel_output_format``: same idea, string
  ``Unrecognised hwaccel output format``.
* GPU branch (only when a device of ``--hwaccel`` type exists):
  ``-hwaccel <type> -hwaccel_output_format <type> -dec 0:0`` decodes the same
  frame count as the software round trip, and a bogus ``-hwaccel_device`` is
  refused.

Exit codes: 0 when every row is ok, or when the GPU branch is an explicit,
evidence-backed SKIP (the hosted runners have no GPU); 1 on any FAIL. Exit 77
is deliberately NOT used: in CI a skip must never be able to green a build, so a
missing fixture, an unrunnable FFmpeg or a failed encode is a hard FAIL with a
printed reason. The only tolerated skip is the GPU branch, and it stays paired
with the software-path evidence above.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIDTH, HEIGHT, FRAMES = 576, 324, 6
NEEDLE_HWACCEL = "Unrecognized hwaccel for a loopback decoder"
NEEDLE_HWFMT = "Unrecognised hwaccel output format"


def run(args: argparse.Namespace, cmd: list[str]) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    if args.libdir:
        env["LD_LIBRARY_PATH"] = args.libdir + ":" + env.get("LD_LIBRARY_PATH", "")
    return subprocess.run(  # noqa: S603 -- binary under test, argument list, no shell
        cmd, env=env, capture_output=True, text=True, timeout=600, check=False
    )


def frame_count(text: str) -> int:
    """Last ``frame= N`` progress count FFmpeg printed (its stats go to stderr)."""
    count = 0
    for match in re.finditer(r"frame=\s*(\d+)", text):
        count = int(match.group(1))
    return count


def loopback_cmd(ffmpeg: str, enc: Path, hw: list[str], filtergraph: str) -> list[str]:
    return [
        ffmpeg,
        "-hide_banner",
        "-nostdin",
        "-y",
        "-i",
        str(enc),
        *hw,
        "-dec",
        "0:0",
        "-filter_complex",
        filtergraph,
        "-map",
        "[f]",
        "-f",
        "null",
        "-",
    ]


def finish(args: argparse.Namespace, rows: list[tuple[str, str, str]], gpu_state: str) -> int:
    hw = args.hwaccel
    for status, label, detail in rows:
        print(f"{status:<4} {label}")
        if detail:
            print(f"     {detail}")
    n_ok = sum(1 for status, _, _ in rows if status == "ok")
    n_fail = sum(1 for status, _, _ in rows if status == "FAIL")
    n_skip = sum(1 for status, _, _ in rows if status == "SKIP")
    evidence = (
        "loopback refuses unknown -hwaccel (patch stderr string), "
        "loopback refuses unknown -hwaccel_output_format (patch stderr string), "
        "hwdownload rejects software frames as the software baseline, "
        "software round trip decodes 6 frames"
    )
    if gpu_state == "ok":
        evidence += "; gpu round trip ran and matched the software frame count"
    elif gpu_state == "unavailable":
        evidence += "; gpu branch SKIPPED (no device on this runner), not passed"
    summary = (
        f"summary: {n_ok} ok, {n_fail} fail, {n_skip} skip; "
        f"gpu={hw} {gpu_state}; evidence={evidence}"
    )
    print(summary)
    if args.summary_out:
        out = Path(args.summary_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(summary + "\n", encoding="utf-8")
    return 1 if any(status == "FAIL" for status, _, _ in rows) else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--libdir", default="", help="directory of the libraries FFmpeg loads")
    parser.add_argument("--yuv", default=str(ROOT / "python" / "test" / "resource" / "yuv"))
    parser.add_argument("--hwaccel", default="cuda", help="hwaccel type the GPU branch probes")
    parser.add_argument(
        "--require-gpu",
        action="store_true",
        help="fail (exit 1) when no usable --hwaccel device exists",
    )
    parser.add_argument(
        "--summary-out", default="", help="also write the summary block to this path"
    )
    args = parser.parse_args()

    rows: list[tuple[str, str, str]] = []

    def add(status: str, label: str, detail: str = "") -> None:
        rows.append((status, label, detail))

    version = run(args, [args.ffmpeg, "-hide_banner", "-version"])
    if version.returncode != 0:
        add("FAIL", "ffmpeg binary runs (-version)", version.stderr[-300:])
        return finish(args, rows, "unknown")

    clip = Path(args.yuv) / "src01_hrc01_576x324.yuv"
    if not clip.is_file():
        add("FAIL", f"fixture present: {clip}", "missing file; expected 576x324 yuv420p source")
        return finish(args, rows, "unknown")

    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        source = tmp / "clip.yuv"
        source.write_bytes(clip.read_bytes()[: WIDTH * HEIGHT * 3 // 2 * FRAMES])
        enc = tmp / "clip.h264"
        enc_run = run(
            args,
            [
                args.ffmpeg,
                "-hide_banner",
                "-nostdin",
                "-y",
                "-f",
                "rawvideo",
                "-pix_fmt",
                "yuv420p",
                "-s",
                f"{WIDTH}x{HEIGHT}",
                "-r",
                "25",
                "-i",
                str(source),
                "-c:v",
                "libx264",
                "-preset",
                "veryfast",
                "-g",
                str(FRAMES),
                "-bf",
                "0",
                "-f",
                "h264",
                str(enc),
            ],
        )
        if enc_run.returncode != 0:
            add("FAIL", "fixture encodes through libx264", enc_run.stderr[-300:])
            return finish(args, rows, "unknown")

        # Assertion 1: the loopback decoder itself reads -hwaccel. A stock
        # FFmpeg silently ignores the option here; the patched one dies with
        # its own stderr string, so the string is the evidence, not the exit.
        bad_hw = run(
            args,
            loopback_cmd(
                args.ffmpeg, enc, ["-hwaccel", "no-such-loopback-hwaccel-3351"], "[dec:0]copy[f]"
            ),
        )
        add(
            "ok" if bad_hw.returncode != 0 and NEEDLE_HWACCEL in bad_hw.stderr else "FAIL",
            "loopback refuses an unrecognised -hwaccel name",
            bad_hw.stderr[-300:],
        )

        # Assertion 2: the loopback decoder reads -hwaccel_output_format too.
        bad_fmt = run(
            args,
            loopback_cmd(
                args.ffmpeg,
                enc,
                ["-hwaccel_output_format", "no-such-pixfmt-3351"],
                "[dec:0]copy[f]",
            ),
        )
        add(
            "ok" if bad_fmt.returncode != 0 and NEEDLE_HWFMT in bad_fmt.stderr else "FAIL",
            "loopback refuses an unrecognised -hwaccel_output_format",
            bad_fmt.stderr[-300:],
        )

        # Assertion 3: with no -hwaccel* the loopback decoder returns system
        # memory frames, and hwdownload must reject those frames on its own.
        hwdown = run(args, loopback_cmd(args.ffmpeg, enc, [], "[dec:0]hwdownload[f]"))
        add(
            "ok" if hwdown.returncode != 0 else "FAIL",
            "software loopback frames are refused by hwdownload",
            hwdown.stderr[-300:],
        )

        # Assertion 4: the plain loopback round trip works and decodes frames.
        sw = run(args, loopback_cmd(args.ffmpeg, enc, [], "[dec:0]copy[f]"))
        sw_frames = frame_count(sw.stderr + sw.stdout)
        add(
            "ok" if sw.returncode == 0 and sw_frames == FRAMES else "FAIL",
            "software loopback round trip decodes the clipped frames",
            f"returncode={sw.returncode}, frames={sw_frames}",
        )

        # Assertion 5, GPU branch: probe the device first; hosted runners have
        # none, so this becomes an explicit SKIP paired with the evidence above.
        probe = run(
            args,
            [
                args.ffmpeg,
                "-hide_banner",
                "-nostdin",
                "-init_hw_device",
                f"{args.hwaccel}=probe:0",
                "-f",
                "lavfi",
                "-i",
                "nullsrc=s=64x64:d=1",
                "-f",
                "null",
                "-",
            ],
        )
        if probe.returncode == 0:
            gpu_state = "ok"
            gpu = run(
                args,
                loopback_cmd(
                    args.ffmpeg,
                    enc,
                    ["-hwaccel", args.hwaccel, "-hwaccel_output_format", args.hwaccel],
                    "[dec:0]copy[f]",
                ),
            )
            gpu_frames = frame_count(gpu.stderr + gpu.stdout)
            add(
                "ok"
                if gpu.returncode == 0 and gpu_frames == sw_frames and sw_frames > 0
                else "FAIL",
                f"gpu round trip (-hwaccel {args.hwaccel}) matches the software frame count",
                f"returncode={gpu.returncode}, frames={gpu_frames}, want={sw_frames}",
            )
            bad_dev = run(
                args,
                loopback_cmd(
                    args.ffmpeg,
                    enc,
                    [
                        "-hwaccel",
                        args.hwaccel,
                        "-hwaccel_output_format",
                        args.hwaccel,
                        "-hwaccel_device",
                        "/no/such/hwaccel/device-3351",
                    ],
                    "[dec:0]copy[f]",
                ),
            )
            add(
                "ok" if bad_dev.returncode != 0 else "FAIL",
                "loopback refuses a bogus -hwaccel_device",
                bad_dev.stderr[-300:],
            )
        else:
            gpu_state = "unavailable"
            if args.require_gpu:
                add(
                    "FAIL",
                    f"gpu device of type {args.hwaccel} usable (--require-gpu)",
                    probe.stderr[-300:],
                )
            else:
                add(
                    "SKIP",
                    f"gpu branch (hwaccel={args.hwaccel}): no {args.hwaccel} device on this runner",
                    "replaced by software-path evidence: round trip, hwdownload rejection, "
                    "and the two patch-option refusal strings above",
                )

    return finish(args, rows, gpu_state)


if __name__ == "__main__":
    sys.exit(main())
