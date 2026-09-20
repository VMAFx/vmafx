#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Phase A real-corpus runner — hardware encoder x CUDA-VMAF pipeline.

Encodes a raw YUV with NVENC / QSV / VAAPI at a CRF/CQ grid, decodes back
to raw YUV, scores with libvmaf (CUDA), and emits one JSONL row per
(source, encoder, cq, frame) carrying:
    * canonical-6 features  (adm2, vif_scale0..3, motion2)
    * per-frame VMAF
    * encode metadata       (encoder, cq, bitrate)

That row schema is what fr_regressor_v2 needs for real training (not the
pooled-only schema the smoke output had).

Usage:
    python3 scripts/dev/hw_encoder_corpus.py \\
        --vmaf-bin core/build-cuda/tools/vmaf \\
        --source .workingdir2/netflix/ref/BigBuckBunny_25fps.yuv \\
        --width 1920 --height 1080 --pix-fmt yuv420p --framerate 25 \\
        --encoder h264_nvenc --cq 19 --cq 25 --cq 31 --cq 37 \\
        --out runs/phase_a/bbb_h264_nvenc.jsonl
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import TextIO

try:
    from scripts.lib.safe_subprocess import CommandResult
    from scripts.lib.safe_subprocess import run as run_command
except ModuleNotFoundError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from lib.safe_subprocess import CommandResult
    from lib.safe_subprocess import run as run_command

CANONICAL_6 = (
    "integer_adm2",
    "integer_vif_scale0",
    "integer_vif_scale1",
    "integer_vif_scale2",
    "integer_vif_scale3",
    "integer_motion2",
)
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MODEL = REPO_ROOT / "model" / "vmaf_v0.6.1.json"


def resolve_executable(value: str | Path) -> Path:
    """Return a stable absolute executable path or fail before doing work."""
    resolved = shutil.which(str(value))
    if resolved is None:
        raise FileNotFoundError(f"executable not found or not executable: {value}")
    path = Path(resolved).resolve(strict=True)
    if not path.is_file() or not os.access(path, os.X_OK):
        raise FileNotFoundError(f"executable not found or not executable: {value}")
    return path


def report_process_output(process: CommandResult) -> None:
    """Surface every diagnostic emitted by a child process."""
    if process.stdout:
        sys.stdout.write(process.stdout)
    if process.stderr:
        sys.stderr.write(process.stderr)


def encode_hw(
    source: Path,
    width: int,
    height: int,
    pix_fmt: str,
    framerate: float,
    encoder: str,
    cq: int,
    out_mp4: Path,
    *,
    qsv_device: Path | None = None,
    vaapi_device: Path | None = None,
    extra: list[str] | None = None,
    ffmpeg_bin: str | Path = "ffmpeg",
) -> tuple[int, float, int]:
    """Run ffmpeg with the requested hardware encoder.

    Returns (returncode, encode_wall_ms, bytes_written).
    """
    pre_args: list[str] = [str(resolve_executable(ffmpeg_bin)), "-y", "-loglevel", "error"]
    if encoder.endswith("_qsv") and qsv_device is not None:
        pre_args += [
            "-init_hw_device",
            f"qsv:hw,child_device={qsv_device}",
        ]
    if encoder.endswith("_vaapi") and vaapi_device is not None:
        pre_args += [
            "-init_hw_device",
            f"vaapi=va:{vaapi_device}",
        ]
    pre_args += [
        "-f",
        "rawvideo",
        "-pix_fmt",
        pix_fmt,
        "-s",
        f"{width}x{height}",
        "-r",
        str(framerate),
        "-i",
        str(source),
    ]
    if encoder.endswith("_nvenc"):
        post = ["-c:v", encoder, "-cq", str(cq), "-preset", "p4"]
    elif encoder.endswith("_qsv"):
        post = ["-c:v", encoder, "-global_quality", str(cq), "-preset", "medium"]
    elif encoder.endswith("_vaapi"):
        post = [
            "-vf",
            "format=nv12,hwupload=extra_hw_frames=16",
            "-c:v",
            encoder,
            "-qp",
            str(cq),
        ]
    elif encoder.endswith("_videotoolbox"):
        # Apple VideoToolbox uses -q:v on the [0, 100] axis (higher =
        # better), opposite direction to x264's CRF [0, 51] (lower =
        # better). Mirror the canonical adapter at
        # tools/vmaf-tune/src/vmaftune/codec_adapters/_videotoolbox_common.py.
        # The harness's `cq` slot is reused as the quality knob; map a
        # CRF-shaped input (0..51) onto the VT scale linearly so the
        # same `--cq 19,25,31,37` grid lights up sensible quality
        # points across both codec families:
        #     q = clamp(100 - 2*cq, 1, 100)
        # When the operator already supplies a VT-native value
        # (cq above _VT_SCALE_PIVOT, where 100 - 2*cq <= 0) we treat
        # the input as already-VT-scale and pass it through clamped
        # to [1, 100].
        _VT_SCALE_PIVOT = 50  # CRF axis -> VT axis crossover.
        q = max(1, min(100, cq)) if cq >= _VT_SCALE_PIVOT else max(1, min(100, 100 - 2 * cq))
        post = ["-c:v", encoder, "-q:v", str(q), "-realtime", "0"]
    else:
        # CPU fallback (libx264) — the corpus may want a CPU baseline row.
        post = ["-c:v", encoder, "-crf", str(cq), "-preset", "medium"]
    if extra:
        post += extra
    cmd = pre_args + post + [str(out_mp4)]

    t0 = time.monotonic()
    p = run_command(
        cmd,
        allowed_executables=(cmd[0],),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=1800,
        max_output_bytes=16 * 1_048_576,
    )
    report_process_output(p)
    elapsed_ms = (time.monotonic() - t0) * 1000.0
    size = out_mp4.stat().st_size if out_mp4.exists() else 0
    return p.returncode, elapsed_ms, size


def decode_to_raw(
    mp4: Path, raw_yuv: Path, pix_fmt: str, *, ffmpeg_bin: str | Path = "ffmpeg"
) -> int:
    """ffmpeg decode mp4 -> raw YUV. Returns rc."""
    cmd = [
        str(resolve_executable(ffmpeg_bin)),
        "-y",
        "-loglevel",
        "error",
        "-i",
        str(mp4),
        "-f",
        "rawvideo",
        "-pix_fmt",
        pix_fmt,
        str(raw_yuv),
    ]
    p = run_command(
        cmd,
        allowed_executables=(cmd[0],),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=600,
        max_output_bytes=16 * 1_048_576,
    )
    report_process_output(p)
    return p.returncode


def score_cuda(
    vmaf_bin: Path,
    ref: Path,
    dist: Path,
    width: int,
    height: int,
    pix_fmt: str,
    json_out: Path,
) -> int:
    """libvmaf CUDA backend, JSON output with per-frame metrics."""
    vmaf_executable = resolve_executable(vmaf_bin)
    pixfmt_map = {"yuv420p": "420", "yuv422p": "422", "yuv444p": "444"}
    bitdepth = 10 if "10" in pix_fmt else (12 if "12" in pix_fmt else 8)
    cmd = [
        str(vmaf_executable),
        "--reference",
        str(ref),
        "--distorted",
        str(dist),
        "--width",
        str(width),
        "--height",
        str(height),
        "--pixel_format",
        pixfmt_map.get(pix_fmt, "420"),
        "--bitdepth",
        str(bitdepth),
        "--model",
        f"path={DEFAULT_MODEL}",
        "--threads",
        "1",
        "--backend",
        "cuda",
        "-q",
        "--json",
        "--output",
        str(json_out),
    ]
    p = run_command(
        cmd,
        allowed_executables=(vmaf_executable,),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=600,
        max_output_bytes=16 * 1_048_576,
    )
    report_process_output(p)
    return p.returncode


def emit_rows(
    payload: dict,
    *,
    src: str,
    encoder: str,
    cq: int,
    enc_bytes: int,
    enc_time_ms: float,
) -> list[dict]:
    rows: list[dict] = []
    for fr in payload.get("frames", []):
        m = fr.get("metrics", {})
        if not all(k in m for k in CANONICAL_6):
            continue
        row = {
            "src": src,
            "encoder": encoder,
            "cq": cq,
            "enc_bytes": enc_bytes,
            "enc_time_ms": enc_time_ms,
            "frame_index": fr.get("frameNum", len(rows)),
            "vmaf": m.get("vmaf"),
            "adm2": m["integer_adm2"],
            "vif_scale0": m["integer_vif_scale0"],
            "vif_scale1": m["integer_vif_scale1"],
            "vif_scale2": m["integer_vif_scale2"],
            "vif_scale3": m["integer_vif_scale3"],
            "motion2": m["integer_motion2"],
        }
        rows.append(row)
    return rows


def build_parser() -> argparse.ArgumentParser:
    """Build the hardware-corpus command line."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--vmaf-bin", type=Path, required=True)
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--pix-fmt", default="yuv420p")
    ap.add_argument("--framerate", type=float, default=25.0)
    ap.add_argument(
        "--encoder",
        required=True,
        choices=[
            "h264_nvenc",
            "hevc_nvenc",
            "av1_nvenc",
            "h264_qsv",
            "hevc_qsv",
            "av1_qsv",
            "h264_vaapi",
            "hevc_vaapi",
            "h264_videotoolbox",
            "hevc_videotoolbox",
            "libx264",
        ],
        help="hardware encoder family. NVENC (NVIDIA), QSV (Intel iHD), "
        "VAAPI (Intel/AMD), VideoToolbox (Apple Silicon / Intel Mac T2), "
        "or libx264 CPU baseline.",
    )
    ap.add_argument(
        "--cq",
        type=int,
        action="append",
        required=True,
        help="quality knob (NVENC: -cq, QSV: -global_quality, "
        "VAAPI: -qp, VideoToolbox: -q:v derived via 100-2*cq, "
        "libx264: -crf). Repeatable.",
    )
    ap.add_argument("--qsv-device", type=Path, default=Path("/dev/dri/renderD129"))
    ap.add_argument("--vaapi-device", type=Path, default=Path("/dev/dri/renderD129"))
    ap.add_argument("--out", type=Path, required=True)
    return ap


def encode_candidate(
    args: argparse.Namespace, cq: int, src_stem: str, workdir: Path
) -> tuple[Path, float, int] | None:
    """Encode one quality point, returning its artifact and measurements."""
    mp4 = workdir / f"{src_stem}_{args.encoder}_cq{cq}.mp4"
    rc, enc_ms, size = encode_hw(
        args.source,
        args.width,
        args.height,
        args.pix_fmt,
        args.framerate,
        args.encoder,
        cq,
        mp4,
        qsv_device=args.qsv_device,
        vaapi_device=args.vaapi_device,
        ffmpeg_bin=args.ffmpeg_bin,
    )
    if rc != 0 or size == 0:
        print(f"[skip] {src_stem} {args.encoder} cq{cq}: encode rc={rc}", file=sys.stderr)
        return None
    return mp4, enc_ms, size


def score_candidate(
    args: argparse.Namespace,
    cq: int,
    src_stem: str,
    workdir: Path,
    mp4: Path,
    enc_ms: float,
    size: int,
) -> tuple[dict, list[dict]] | None:
    """Decode and score one encoded quality point."""
    yuv = workdir / f"{src_stem}_{args.encoder}_cq{cq}.yuv"
    if decode_to_raw(mp4, yuv, args.pix_fmt, ffmpeg_bin=args.ffmpeg_bin) != 0 or not yuv.exists():
        print(f"[skip] {src_stem} {args.encoder} cq{cq}: decode failed", file=sys.stderr)
        return None
    json_out = workdir / "vmaf.json"
    if (
        score_cuda(
            args.vmaf_bin,
            args.source,
            yuv,
            args.width,
            args.height,
            args.pix_fmt,
            json_out,
        )
        != 0
        or not json_out.exists()
    ):
        print(f"[skip] {src_stem} {args.encoder} cq{cq}: score failed", file=sys.stderr)
        return None
    payload = json.loads(json_out.read_text())
    rows = emit_rows(
        payload,
        src=src_stem,
        encoder=args.encoder,
        cq=cq,
        enc_bytes=size,
        enc_time_ms=enc_ms,
    )
    return payload, rows


def write_quality_rows(args: argparse.Namespace, output: TextIO, src_stem: str) -> int:
    """Run every requested quality point and append successful frame rows."""
    written = 0
    for cq in args.cq:
        with tempfile.TemporaryDirectory(prefix="hwenc_") as directory:
            workdir = Path(directory)
            encoded = encode_candidate(args, cq, src_stem, workdir)
            if encoded is None:
                continue
            mp4, enc_ms, size = encoded
            scored = score_candidate(args, cq, src_stem, workdir, mp4, enc_ms, size)
            if scored is None:
                continue
            payload, rows = scored
            for row in rows:
                output.write(json.dumps(row) + "\n")
            written += len(rows)
            print(
                f"[ok] {src_stem} {args.encoder} cq{cq}: "
                f"{len(rows)} rows, vmaf_pool={payload['pooled_metrics']['vmaf']['mean']:.2f}, "
                f"enc={enc_ms:.0f}ms, sz={size}",
                flush=True,
            )
    return written


def main() -> int:
    args = build_parser().parse_args()

    if not args.source.is_file():
        print(f"error: source not found: {args.source}", file=sys.stderr)
        return 2
    try:
        args.ffmpeg_bin = resolve_executable("ffmpeg")
        args.vmaf_bin = resolve_executable(args.vmaf_bin)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    args.out.parent.mkdir(parents=True, exist_ok=True)

    with args.out.open("a", encoding="utf-8") as fh:
        written = write_quality_rows(args, fh, args.source.stem)
    print(f"[done] wrote {written} rows -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
