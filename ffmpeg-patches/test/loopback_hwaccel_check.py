#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
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
import re
import tempfile
from pathlib import Path

from check_runner import run

ROOT = Path(__file__).resolve().parents[2]
WIDTH, HEIGHT, FRAMES = 576, 324, 6
NEEDLE_HWACCEL = "Unrecognized hwaccel for a loopback decoder"
NEEDLE_HWFMT = "Unrecognised hwaccel output format"
NEEDLE_HWDOWNLOAD = "Impossible to convert between the formats"
# fftools/ffmpeg_hw.c and fftools/ffmpeg_dec.c print these when a device cannot be opened.
NEEDLES_BAD_DEVICE = ("Device creation failed", "Hardware device setup failed")
LOOPBACK_STATS = re.compile(r"Output stream #1:0 \(video\): (\d+) frames encoded")


def loopback_cmd(ffmpeg: str, enc: Path, hw: list[str], filtergraph: str) -> list[str]:
    """Encode the clip into output #0, then decode that encode through ``-dec 0:0`` into output #1.

    ``-dec`` names an encoded output stream that must already exist, as in the FFmpeg
    documentation's loopback example; pointing it at the output its own filtergraph feeds is
    a cycle FFmpeg refuses ("Transcoding graph has a cycle"). The options in ``hw`` sit between
    the two outputs, so they belong to the loopback decoder.
    """
    return [
        ffmpeg,
        "-hide_banner",
        "-nostdin",
        "-y",
        "-loglevel",
        "verbose",
        "-i",
        str(enc),
        "-map",
        "0:v:0",
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-f",
        "null",
        "-",
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


def loopback_frames(stderr: str) -> int:
    """Frames output #1 encoded, from FFmpeg's verbose end-of-run statistics.

    The ``frame=`` progress line counts output #0, the encode, so it says nothing about the
    loopback decoder.
    """
    match = LOOPBACK_STATS.search(stderr)
    return int(match.group(1)) if match else 0


def finish(args: argparse.Namespace, rows: list[tuple[str, str, str]], gpu_state: str) -> int:
    for status, label, detail in rows:
        print(f"{status:<4} {label}")
        if detail:
            print(f"     {detail}")
    n_ok = sum(1 for status, _, _ in rows if status == "ok")
    n_fail = sum(1 for status, _, _ in rows if status == "FAIL")
    n_skip = sum(1 for status, _, _ in rows if status == "SKIP")
    evidence_labels = [label for status, label, _ in rows if status == "ok"]
    evidence = "; ".join(evidence_labels) if evidence_labels else "none"
    summary = (
        f"summary: {n_ok} ok, {n_fail} fail, {n_skip} skip; "
        f"gpu={args.hwaccel} {gpu_state}; evidence={evidence}"
    )
    print(summary)
    if args.summary_out:
        out = Path(args.summary_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(summary + "\n", encoding="utf-8")
    return 1 if any(status == "FAIL" for status, _, _ in rows) else 0


def check_version(args: argparse.Namespace, rows: list[tuple[str, str, str]]) -> bool:
    version = run(args, [args.ffmpeg, "-hide_banner", "-version"])
    if version.returncode != 0:
        rows.append(("FAIL", "ffmpeg binary runs (-version)", version.stderr[-300:]))
        return False
    return True


def prepare_clip(
    args: argparse.Namespace, clip: Path, tmp: Path, rows: list[tuple[str, str, str]]
) -> Path | None:
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
        rows.append(("FAIL", "fixture encodes through libx264", enc_run.stderr[-300:]))
        return None
    return enc


def check_refused_option(
    args: argparse.Namespace,
    enc: Path,
    rows: list[tuple[str, str, str]],
    hw: list[str],
    label: str,
    needle: str,
) -> None:
    result = run(args, loopback_cmd(args.ffmpeg, enc, hw, "[dec:0]copy[f]"))
    rows.append(
        (
            "ok" if result.returncode != 0 and needle in result.stderr else "FAIL",
            label,
            result.stderr[-300:],
        )
    )


def check_hwdownload(args: argparse.Namespace, enc: Path, rows: list[tuple[str, str, str]]) -> None:
    hwdown = run(args, loopback_cmd(args.ffmpeg, enc, [], "[dec:0]hwdownload[f]"))
    rows.append(
        (
            "ok" if hwdown.returncode != 0 and NEEDLE_HWDOWNLOAD in hwdown.stderr else "FAIL",
            "software loopback frames are refused by hwdownload",
            hwdown.stderr[-300:],
        )
    )


def check_software_round_trip(
    args: argparse.Namespace, enc: Path, rows: list[tuple[str, str, str]]
) -> int:
    sw = run(args, loopback_cmd(args.ffmpeg, enc, [], "[dec:0]copy[f]"))
    sw_frames = loopback_frames(sw.stderr)
    rows.append(
        (
            "ok" if sw.returncode == 0 and sw_frames == FRAMES else "FAIL",
            f"software loopback round trip decodes {FRAMES} frames",
            f"returncode={sw.returncode}, frames={sw_frames}",
        )
    )
    return sw_frames


def check_gpu_branch(
    args: argparse.Namespace, enc: Path, rows: list[tuple[str, str, str]], sw_frames: int
) -> str:
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
    if probe.returncode != 0:
        if args.require_gpu:
            rows.append(
                (
                    "FAIL",
                    f"gpu device of type {args.hwaccel} usable (--require-gpu)",
                    probe.stderr[-300:],
                )
            )
        else:
            rows.append(
                (
                    "SKIP",
                    f"gpu branch (hwaccel={args.hwaccel}): no {args.hwaccel} device on this runner",
                    "gpu rows not run; the software-path rows above are the evidence, "
                    "and a FAIL above still fails the check",
                )
            )
        return "unavailable"

    gpu = run(
        args,
        loopback_cmd(
            args.ffmpeg,
            enc,
            ["-hwaccel", args.hwaccel, "-hwaccel_output_format", args.hwaccel],
            "[dec:0]copy[f]",
        ),
    )
    gpu_frames = loopback_frames(gpu.stderr)
    rows.append(
        (
            "ok" if gpu.returncode == 0 and gpu_frames == sw_frames and sw_frames > 0 else "FAIL",
            f"gpu round trip (-hwaccel {args.hwaccel}) matches the software frame count",
            f"returncode={gpu.returncode}, frames={gpu_frames}, want={sw_frames}",
        )
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
    rows.append(
        (
            "ok"
            if bad_dev.returncode not in (0, 124, 127)
            and any(needle in bad_dev.stderr for needle in NEEDLES_BAD_DEVICE)
            else "FAIL",
            "loopback refuses a bogus -hwaccel_device",
            bad_dev.stderr[-300:],
        )
    )
    return "ok"


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
    if not check_version(args, rows):
        return finish(args, rows, "unknown")

    clip = Path(args.yuv) / "src01_hrc01_576x324.yuv"
    if not clip.is_file():
        rows.append(
            (
                "FAIL",
                f"fixture present: {clip}",
                "missing file; expected 576x324 yuv420p source",
            )
        )
        return finish(args, rows, "unknown")

    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        enc = prepare_clip(args, clip, tmp, rows)
        if enc is None:
            return finish(args, rows, "unknown")
        check_refused_option(
            args,
            enc,
            rows,
            ["-hwaccel", "no-such-loopback-hwaccel-3351"],
            "loopback refuses an unrecognised -hwaccel name",
            NEEDLE_HWACCEL,
        )
        check_refused_option(
            args,
            enc,
            rows,
            ["-hwaccel_output_format", "no-such-pixfmt-3351"],
            "loopback refuses an unrecognised -hwaccel_output_format",
            NEEDLE_HWFMT,
        )
        check_hwdownload(args, enc, rows)
        sw_frames = check_software_round_trip(args, enc, rows)
        gpu_state = check_gpu_branch(args, enc, rows, sw_frames)

    return finish(args, rows, gpu_state)


if __name__ == "__main__":
    raise SystemExit(main())
