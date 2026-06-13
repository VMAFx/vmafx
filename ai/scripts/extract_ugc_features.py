#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Extract FULL_FEATURES (+ teacher VMAF) over a UGC manifest.

For each (orig, dis) pair in the YouTube UGC vp9 manifest written by
``fetch_youtube_ugc_subset.py``, decode both clips to a common YUV
geometry via ffmpeg, run ``vmaf`` with the current ``FULL_FEATURES``
pool plus the production vmaf_v0.6.1 predictor as the teacher, and
append per-frame rows to a parquet matching the current full-feature
corpus schema.

Decode geometry: smallest of (orig, dis) original height, capped at
``--max-height`` (default 360). The cap keeps wall-time + intermediate
YUV size bounded; documented trade-off in the v5 ADR.

Output schema (matches the current full-feature corpus refresh):
    corpus, source, frame_index,
    <FULL_FEATURES columns>, vmaf
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pandas as pd

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

from ai.data.feature_extractor import (  # noqa: E402
    DEFAULT_VMAF_BINARY,
    FULL_FEATURES,
    _extractors_for,
)

SCHEMA_COLS = (*FULL_FEATURES, "vmaf")

_METRIC_ALIASES: dict[str, tuple[str, ...]] = {
    "speed_temporal": (
        "speed_temporal",
        "Speed_temporal_feature_speed_temporal_score",
    ),
    "speed_chroma_u": (
        "speed_chroma_u",
        "Speed_chroma_feature_speed_chroma_u_score",
    ),
    "speed_chroma_v": (
        "speed_chroma_v",
        "Speed_chroma_feature_speed_chroma_v_score",
    ),
    "speed_chroma_uv": (
        "speed_chroma_uv",
        "Speed_chroma_feature_speed_chroma_uv_score",
    ),
}


def _ffprobe(path: Path) -> dict:
    cmd = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-print_format",
        "json",
        "-show_streams",
        str(path),
    ]
    out = subprocess.check_output(cmd)
    return json.loads(out)["streams"][0]


def _decode_to_yuv(src: Path, dest: Path, w: int, h: int, max_frames: int) -> int:
    """Decode src video to dest as raw yuv420p 8-bit, scaled to w*h.

    Returns the number of frames written.
    """
    if dest.exists() and dest.stat().st_size > 0:
        # frame count from size
        frame_bytes = w * h * 3 // 2
        return dest.stat().st_size // frame_bytes
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".tmp")
    cmd = [
        "ffmpeg",
        "-y",
        "-loglevel",
        "error",
        "-i",
        str(src),
        "-vf",
        f"scale={w}:{h}:flags=bicubic",
        "-pix_fmt",
        "yuv420p",
        "-frames:v",
        str(max_frames),
        "-f",
        "rawvideo",
        str(tmp),
    ]
    subprocess.run(cmd, check=True)
    tmp.rename(dest)
    frame_bytes = w * h * 3 // 2
    return dest.stat().st_size // frame_bytes


def _run_vmaf(
    vmaf_bin: Path,
    ref: Path,
    dis: Path,
    w: int,
    h: int,
    n_threads: int,
    model: Path,
) -> list[dict]:
    """Run vmaf with FULL_FEATURES + the v0.6.1 model. Return frames list."""
    # Per-pair scratch JSON: predictable /tmp paths leak username + invite
    # collisions on multi-tenant hosts; route through tempfile honouring
    # VMAF_TINY_AI_SCRATCH (same env-var convention as the BVI-DVC / KoNViD
    # full-feature scripts).
    scratch_dir = Path(os.environ.get("VMAF_TINY_AI_SCRATCH", tempfile.gettempdir()))
    scratch_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        suffix=".json",
        prefix=f"ugc_vmaf_{ref.stem}_{dis.stem}_",
        dir=scratch_dir,
        delete=False,
    ) as tmp_fh:
        out = Path(tmp_fh.name)
    try:
        feature_args: list[str] = []
        for extractor in _extractors_for(FULL_FEATURES):
            feature_args += ["--feature", extractor]
        cmd = [
            str(vmaf_bin),
            "-r",
            str(ref),
            "-d",
            str(dis),
            "-w",
            str(w),
            "-h",
            str(h),
            "-p",
            "420",
            "-b",
            "8",
            "-m",
            "path=" + shlex.quote(str(model)),
            *feature_args,
            "--threads",
            str(n_threads),
            "--no_cuda",
            "--no_sycl",
            "--output",
            str(out),
            "--json",
        ]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with out.open() as f:
            doc = json.load(f)
        return doc.get("frames", [])
    finally:
        out.unlink(missing_ok=True)


def _frame_row(metrics: dict) -> dict:
    """Translate libvmaf JSON metric names to our parquet schema."""

    def m(name: str) -> float:
        keys = _METRIC_ALIASES.get(name, (name, f"integer_{name}"))
        for key in keys:
            value = metrics.get(key)
            if value is not None:
                return float(value)
        return float("nan")

    row = {feature: m(feature) for feature in FULL_FEATURES}
    row["vmaf"] = m("vmaf")
    return row


def _write_manifest(
    *,
    path: Path,
    args: argparse.Namespace,
    raw_argv: list[str],
    manifest_items: int,
    pair_count: int,
    fail_count: int,
    row_count: int,
    source_count: int,
) -> None:
    from aiutils.run_manifest import write_run_manifest

    write_run_manifest(
        path,
        schema="ugc-full-feature-extraction-manifest-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={
            "manifest": args.manifest,
            "vmaf_binary": args.vmaf_bin,
            "model": args.model,
        },
        outputs={
            "parquet": args.out_parquet,
            "manifest": path,
            "yuv_dir": args.yuv_dir,
        },
        sections={
            "manifest_items": int(manifest_items),
            "pair_count": int(pair_count),
            "fail_count": int(fail_count),
            "row_count": int(row_count),
            "source_count": int(source_count),
            "feature_columns": list(SCHEMA_COLS),
            "config": {
                "max_height": int(args.max_height),
                "max_frames": int(args.max_frames),
                "threads": int(args.threads),
                "keep_yuv": bool(args.keep_yuv),
            },
        },
    )


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument(
        "--yuv-dir",
        type=Path,
        required=True,
        help="Working dir for decoded raw YUVs (deleted after extract).",
    )
    ap.add_argument("--vmaf-bin", type=Path, default=DEFAULT_VMAF_BINARY)
    ap.add_argument(
        "--model",
        type=Path,
        default=REPO_ROOT / "model" / "vmaf_v0.6.1.json",
        help="Path to the vmaf_v0.6.1 model JSON.",
    )
    ap.add_argument("--out-parquet", type=Path, required=True)
    ap.add_argument(
        "--max-height",
        type=int,
        default=360,
        help="Cap decode height; smaller = faster, less memory.",
    )
    ap.add_argument(
        "--max-frames", type=int, default=300, help="Cap frames per pair; ~10s @ 30fps."
    )
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--keep-yuv", action="store_true")
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Replay manifest JSON sidecar (default: <out-parquet>.manifest.json).",
    )
    args = ap.parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.out_parquet.with_suffix(".manifest.json")

    if not args.vmaf_bin.is_file():
        print(f"error: vmaf binary not found: {args.vmaf_bin}", file=sys.stderr)
        return 2
    if not args.model.is_file():
        print(f"error: model not found: {args.model}", file=sys.stderr)
        return 2
    if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
        print("error: ffmpeg/ffprobe not on PATH", file=sys.stderr)
        return 2

    manifest = json.loads(args.manifest.read_text())
    print(f"[ugc-extract] manifest stems={len(manifest)}", flush=True)

    rows: list[dict] = []
    pair_count = 0
    fail_count = 0
    t0 = time.monotonic()
    for stem, files in sorted(manifest.items()):
        orig = Path(files["orig"])
        if not orig.is_file():
            print(f"  [{stem}] missing orig, skip", flush=True)
            continue
        try:
            probe = _ffprobe(orig)
        except Exception as exc:  # pragma: no cover
            print(f"  [{stem}] ffprobe failed: {exc}", flush=True)
            fail_count += 1
            continue
        ow = int(probe["width"])
        oh = int(probe["height"])
        # Down-scale to max_height keeping aspect
        target_h = min(oh, args.max_height)
        target_w = (ow * target_h) // oh
        # Make even
        target_w -= target_w & 1
        target_h -= target_h & 1
        ref_yuv = args.yuv_dir / f"{stem}_orig_{target_w}x{target_h}.yuv"
        try:
            _decode_to_yuv(orig, ref_yuv, target_w, target_h, args.max_frames)
        except subprocess.CalledProcessError as exc:
            print(f"  [{stem}] decode-orig failed: {exc}", flush=True)
            fail_count += 1
            continue

        for sfx in ("cbr", "vod", "vodlb"):
            if sfx not in files:
                continue
            dis_src = Path(files[sfx])
            if not dis_src.is_file():
                continue
            dis_yuv = args.yuv_dir / f"{stem}_{sfx}_{target_w}x{target_h}.yuv"
            try:
                _decode_to_yuv(dis_src, dis_yuv, target_w, target_h, args.max_frames)
            except subprocess.CalledProcessError as exc:
                print(f"  [{stem}/{sfx}] decode-dis failed: {exc}", flush=True)
                fail_count += 1
                continue
            try:
                frames = _run_vmaf(
                    args.vmaf_bin,
                    ref_yuv,
                    dis_yuv,
                    target_w,
                    target_h,
                    args.threads,
                    args.model,
                )
            except subprocess.CalledProcessError as exc:
                print(f"  [{stem}/{sfx}] vmaf failed: {exc}", flush=True)
                fail_count += 1
                if not args.keep_yuv:
                    dis_yuv.unlink(missing_ok=True)
                continue
            source_name = f"ugc-{stem}-{sfx}"
            for frame in frames:
                m = frame.get("metrics", {})
                row = _frame_row(m)
                row["corpus"] = "ugc"
                row["source"] = source_name
                row["frame_index"] = int(frame.get("frameNum", len(rows)))
                rows.append(row)
            pair_count += 1
            print(
                f"  [{stem}/{sfx}] {target_w}x{target_h} frames={len(frames)} "
                f"vmaf~{frames[0].get('metrics',{}).get('vmaf','-') if frames else '-'} "
                f"({time.monotonic()-t0:.0f}s)",
                flush=True,
            )
            if not args.keep_yuv:
                dis_yuv.unlink(missing_ok=True)
        if not args.keep_yuv:
            ref_yuv.unlink(missing_ok=True)

    if not rows:
        print("error: no rows extracted", file=sys.stderr)
        return 2

    df = pd.DataFrame(rows)
    # Reorder to canonical schema
    full_cols = ("corpus", "source", "frame_index", *SCHEMA_COLS)
    for c in full_cols:
        if c not in df.columns:
            df[c] = float("nan")
    df = df[list(full_cols)]
    args.out_parquet.parent.mkdir(parents=True, exist_ok=True)
    df.to_parquet(args.out_parquet, index=False)
    _write_manifest(
        path=args.manifest_out,
        args=args,
        raw_argv=raw_argv,
        manifest_items=len(manifest),
        pair_count=pair_count,
        fail_count=fail_count,
        row_count=len(df),
        source_count=int(df["source"].nunique()),
    )
    print(
        f"[ugc-extract] wrote {args.out_parquet} pairs={pair_count} fails={fail_count} "
        f"rows={len(df)} sources={df['source'].nunique()} "
        f"wall={time.monotonic()-t0:.0f}s",
        flush=True,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
