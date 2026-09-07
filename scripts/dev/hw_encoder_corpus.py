#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CANONICAL_6 = (
    "integer_adm2",
    "integer_vif_scale0",
    "integer_vif_scale1",
    "integer_vif_scale2",
    "integer_vif_scale3",
    "integer_motion2",
)


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
) -> tuple[int, float, int]:
    """Run ffmpeg with the requested hardware encoder.

    Returns (returncode, encode_wall_ms, bytes_written).
    """
    pre_args: list[str] = ["ffmpeg", "-y", "-loglevel", "error"]
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
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)  # noqa: S603
    elapsed_ms = (time.monotonic() - t0) * 1000.0
    size = out_mp4.stat().st_size if out_mp4.exists() else 0
    return p.returncode, elapsed_ms, size


def decode_to_raw(mp4: Path, raw_yuv: Path, pix_fmt: str) -> int:
    """ffmpeg decode mp4 -> raw YUV. Returns rc."""
    cmd = [
        "ffmpeg",
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
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)  # noqa: S603
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
    pixfmt_map = {"yuv420p": "420", "yuv422p": "422", "yuv444p": "444"}
    bitdepth = 10 if "10" in pix_fmt else (12 if "12" in pix_fmt else 8)
    cmd = [
        str(vmaf_bin),
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
        "path=model/vmaf_v0.6.1.json",
        "--threads",
        "1",
        "--gpumask=0",
        "--no_sycl",
        "--no_vulkan",
        "-q",
        "--json",
        "--output",
        str(json_out),
    ]
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)  # noqa: S603
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


def main() -> int:
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
    args = ap.parse_args()

    if not args.source.is_file():
        print(f"error: source not found: {args.source}", file=sys.stderr)
        return 2
    if not args.vmaf_bin.is_file():
        print(f"error: vmaf binary not found: {args.vmaf_bin}", file=sys.stderr)
        return 2
    args.out.parent.mkdir(parents=True, exist_ok=True)

    src_stem = args.source.stem
    written = 0
    with args.out.open("a", encoding="utf-8") as fh:
        for cq in args.cq:
            with tempfile.TemporaryDirectory(prefix="hwenc_") as td:
                td_path = Path(td)
                mp4 = td_path / f"{src_stem}_{args.encoder}_cq{cq}.mp4"
                rc, enc_ms, sz = encode_hw(
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
                )
                if rc != 0 or sz == 0:
                    print(
                        f"[skip] {src_stem} {args.encoder} cq{cq}: encode rc={rc}", file=sys.stderr
                    )
                    continue
                yuv = td_path / f"{src_stem}_{args.encoder}_cq{cq}.yuv"
                if decode_to_raw(mp4, yuv, args.pix_fmt) != 0 or not yuv.exists():
                    print(
                        f"[skip] {src_stem} {args.encoder} cq{cq}: decode failed", file=sys.stderr
                    )
                    continue
                json_out = td_path / "vmaf.json"
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
                    continue
                payload = json.loads(json_out.read_text())
                rows = emit_rows(
                    payload,
                    src=src_stem,
                    encoder=args.encoder,
                    cq=cq,
                    enc_bytes=sz,
                    enc_time_ms=enc_ms,
                )
                for r in rows:
                    fh.write(json.dumps(r) + "\n")
                written += len(rows)
                print(
                    f"[ok] {src_stem} {args.encoder} cq{cq}: "
                    f"{len(rows)} rows, vmaf_pool="
                    f"{payload['pooled_metrics']['vmaf']['mean']:.2f}, "
                    f"enc={enc_ms:.0f}ms, sz={sz}",
                    flush=True,
                )
    print(f"[done] wrote {written} rows -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
