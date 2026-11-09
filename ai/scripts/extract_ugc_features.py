#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Extract FULL_FEATURES (+ teacher VMAF) over a UGC manifest.

For each (orig, dis) pair in the YouTube UGC vp9 manifest written by
``fetch_youtube_ugc_subset.py``, decode both clips to a common YUV
geometry via ffmpeg, run ``vmaf`` with the current ``FULL_FEATURES``
pool plus the fork default teacher model (ADR-1168 single source, ``--model``
to override; ADR-1173) as the teacher, and
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
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

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
from ai.data.scores import DEFAULT_MODEL, resolve_teacher_model  # noqa: E402

# Bounds = max(60, 10x the slowest measured run): at the default --max-height 360
# and --max-frames 300, ffprobe took 0.20 s, decode 0.29 s, vmaf 4.20 s.
FFMPEG_TIMEOUT_S = 60
VMAF_TIMEOUT_S = 60

SCHEMA_COLS = (*FULL_FEATURES, "teacher_model", "vmaf")

_METRIC_ALIASES: dict[str, tuple[str, ...]] = {
    "adm3": ("adm3", "integer_adm3"),
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
    """Run ffprobe on a video path and return metadata for the first video stream."""
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
    out = subprocess.check_output(cmd, timeout=FFMPEG_TIMEOUT_S)
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
    subprocess.run(cmd, check=True, timeout=FFMPEG_TIMEOUT_S)
    tmp.rename(dest)
    frame_bytes = w * h * 3 // 2
    return dest.stat().st_size // frame_bytes


def _validate_run_vmaf_args(
    vmaf_bin: Path, ref: Path, dis: Path, w: int, h: int, n_threads: int, model: Path
) -> None:
    if not isinstance(w, int) or isinstance(w, bool) or w <= 0:
        raise ValueError(f"w must be a positive integer, got {w!r}")
    if not isinstance(h, int) or isinstance(h, bool) or h <= 0:
        raise ValueError(f"h must be a positive integer, got {h!r}")
    if not isinstance(n_threads, int) or isinstance(n_threads, bool) or n_threads <= 0:
        raise ValueError(f"n_threads must be a positive integer, got {n_threads!r}")

    for name, p in (("vmaf_bin", vmaf_bin), ("ref", ref), ("dis", dis), ("model", model)):
        s = str(p).strip()
        if not s or s == ".":
            raise ValueError(f"{name} cannot be empty")
        if "\0" in s:
            raise ValueError(f"{name} cannot contain null bytes: {s!r}")


def _resolve_scratch_file(ref: Path, dis: Path) -> Path:
    raw_scratch = os.environ.get("VMAF_TINY_AI_SCRATCH")
    if raw_scratch is not None:
        if not raw_scratch.strip():
            raise ValueError("VMAF_TINY_AI_SCRATCH cannot be empty")
        if "\0" in raw_scratch:
            raise ValueError("VMAF_TINY_AI_SCRATCH contains null byte")
        scratch_path = Path(raw_scratch)
        if not scratch_path.is_absolute():
            raise ValueError(f"VMAF_TINY_AI_SCRATCH must be an absolute path: {raw_scratch!r}")
        scratch_dir = scratch_path.resolve()
    else:
        scratch_dir = Path(tempfile.gettempdir()).resolve()
    scratch_dir.mkdir(parents=True, exist_ok=True)

    safe_ref = "".join(c for c in ref.stem if c.isalnum() or c in ("-", "_")) or "ref"
    safe_dis = "".join(c for c in dis.stem if c.isalnum() or c in ("-", "_")) or "dis"
    with tempfile.NamedTemporaryFile(
        mode="w",
        suffix=".json",
        prefix=f"ugc_vmaf_{safe_ref}_{safe_dis}_",
        dir=scratch_dir,
        delete=False,
    ) as tmp_fh:
        out = Path(tmp_fh.name).resolve()

    try:
        out.relative_to(scratch_dir.resolve())
    except ValueError as err:
        out.unlink(missing_ok=True)
        raise ValueError(f"Temporary file {out} escaped scratch directory {scratch_dir}") from err
    return out


def _build_vmaf_cmd(
    vmaf_bin: Path,
    ref: Path,
    dis: Path,
    w: int,
    h: int,
    n_threads: int,
    model: Path,
    out: Path,
) -> list[str]:
    feature_args: list[str] = []
    for extractor in _extractors_for(FULL_FEATURES):
        feature_args += ["--feature", extractor]
    resolved = resolve_teacher_model(model)
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
        resolved.arg,
        *feature_args,
        "--threads",
        str(n_threads),
        "--no_cuda",
        "--no_sycl",
        "--output",
        str(out),
        "--json",
    ]
    for arg in cmd:
        if not isinstance(arg, str):
            raise TypeError(f"Command argument must be a string, got {type(arg).__name__}: {arg!r}")
        if "\0" in arg:
            raise ValueError(f"Command argument contains null byte: {arg!r}")
    return cmd


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
    _validate_run_vmaf_args(vmaf_bin, ref, dis, w, h, n_threads, model)
    out = _resolve_scratch_file(ref, dis)
    try:
        cmd = _build_vmaf_cmd(vmaf_bin, ref, dis, w, h, n_threads, model, out)
        subprocess.run(
            cmd,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=VMAF_TIMEOUT_S,
        )
        with out.open() as f:
            doc = json.load(f)
        return doc.get("frames", [])
    finally:
        out.unlink(missing_ok=True)


def _frame_row(metrics: dict, teacher_model: str = DEFAULT_MODEL) -> dict:
    """Translate libvmaf JSON metric names to our parquet schema."""

    def m(name: str) -> float:
        keys = _METRIC_ALIASES.get(name, (name, f"integer_{name}"))
        for key in keys:
            value = metrics.get(key)
            if value is not None:
                return float(value)
        prefix = f"integer_{name}_"
        for k, val in metrics.items():
            if k.startswith(prefix) and val is not None:
                return float(val)
        return float("nan")

    row = {feature: m(feature) for feature in FULL_FEATURES}
    row["teacher_model"] = teacher_model
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
    teacher_model: str = DEFAULT_MODEL,
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
            "model": teacher_model,
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
            "teacher_model": teacher_model,
            "feature_columns": list(SCHEMA_COLS),
            "config": {
                "max_height": int(args.max_height),
                "max_frames": int(args.max_frames),
                "threads": int(args.threads),
                "keep_yuv": bool(args.keep_yuv),
            },
        },
    )


def _build_parser() -> argparse.ArgumentParser:
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
        default=None,
        help="Path or version name for teacher VMAF model (default: single-source default model).",
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
    return ap


def _validate_prerequisites(args: argparse.Namespace, resolved_teacher: Any) -> int | None:
    if not args.vmaf_bin.is_file():
        print(f"error: vmaf binary not found: {args.vmaf_bin}", file=sys.stderr)
        return 2
    if resolved_teacher.is_path and not Path(resolved_teacher.resolved).is_file():
        print(f"error: model not found: {resolved_teacher.resolved}", file=sys.stderr)
        return 2
    if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
        print("error: ffmpeg/ffprobe not on PATH", file=sys.stderr)
        return 2
    return None


def _scale_geometry(ow: int, oh: int, max_height: int) -> tuple[int, int] | None:
    target_h = min(oh, max_height)
    target_w = (ow * target_h) // oh
    target_w -= target_w & 1
    target_h -= target_h & 1
    if target_w < 2 or target_h < 2:
        return None
    return target_w, target_h


def _process_distorted_clip(
    stem: str,
    sfx: str,
    dis_src: Path,
    ref_yuv: Path,
    target_w: int,
    target_h: int,
    args: argparse.Namespace,
    resolved_teacher: Any,
    t0: float,
    current_row_count: int,
) -> tuple[list[dict], bool]:
    dis_yuv = args.yuv_dir / f"{stem}_{sfx}_{target_w}x{target_h}.yuv"
    try:
        _decode_to_yuv(dis_src, dis_yuv, target_w, target_h, args.max_frames)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"  [{stem}/{sfx}] decode-dis failed: {exc}", flush=True)
        return [], False
    try:
        frames = _run_vmaf(
            args.vmaf_bin,
            ref_yuv,
            dis_yuv,
            target_w,
            target_h,
            args.threads,
            resolved_teacher.arg,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"  [{stem}/{sfx}] vmaf failed: {exc}", flush=True)
        if not args.keep_yuv:
            dis_yuv.unlink(missing_ok=True)
        return [], False

    rows: list[dict] = []
    source_name = f"ugc-{stem}-{sfx}"
    for frame in frames:
        m = frame.get("metrics", {})
        row = _frame_row(m, teacher_model=resolved_teacher.name)
        row["corpus"] = "ugc"
        row["source"] = source_name
        row["frame_index"] = int(frame.get("frameNum", current_row_count + len(rows)))
        rows.append(row)
    vmaf_val = frames[0].get("metrics", {}).get("vmaf", "-") if frames else "-"
    print(
        f"  [{stem}/{sfx}] {target_w}x{target_h} frames={len(frames)} "
        f"vmaf~{vmaf_val} ({time.monotonic() - t0:.0f}s)",
        flush=True,
    )
    if not args.keep_yuv:
        dis_yuv.unlink(missing_ok=True)
    return rows, True


def _decode_orig(stem: str, orig: Path, args: argparse.Namespace) -> tuple[Path, int, int] | None:
    try:
        probe = _ffprobe(orig)
        ow = int(probe["width"])
        oh = int(probe["height"])
        if ow <= 0 or oh <= 0:
            raise ValueError(f"degenerate geometry {ow}x{oh}")
    except Exception as exc:
        print(f"  [{stem}] ffprobe/geometry failed: {exc}", flush=True)
        return None

    geom = _scale_geometry(ow, oh, args.max_height)
    if geom is None:
        print(
            f"  [{stem}] degenerate scaled geometry {ow}x{oh}; skip",
            flush=True,
        )
        return None
    target_w, target_h = geom

    ref_yuv = args.yuv_dir / f"{stem}_orig_{target_w}x{target_h}.yuv"
    try:
        _decode_to_yuv(orig, ref_yuv, target_w, target_h, args.max_frames)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"  [{stem}] decode-orig failed: {exc}", flush=True)
        return None
    return ref_yuv, target_w, target_h


def _process_stem(
    stem: str,
    files: dict[str, Any],
    args: argparse.Namespace,
    resolved_teacher: Any,
    t0: float,
    current_row_count: int,
) -> tuple[list[dict], int, int]:
    orig = Path(files["orig"])
    if not orig.is_file():
        print(f"  [{stem}] missing orig, skip", flush=True)
        return [], 0, 0
    decoded = _decode_orig(stem, orig, args)
    if decoded is None:
        return [], 0, 1
    ref_yuv, target_w, target_h = decoded

    stem_rows: list[dict] = []
    pairs = 0
    fails = 0
    for sfx in ("cbr", "vod", "vodlb"):
        if sfx not in files:
            continue
        dis_src = Path(files[sfx])
        if not dis_src.is_file():
            continue
        rows, ok = _process_distorted_clip(
            stem,
            sfx,
            dis_src,
            ref_yuv,
            target_w,
            target_h,
            args,
            resolved_teacher,
            t0,
            current_row_count + len(stem_rows),
        )
        if ok:
            stem_rows.extend(rows)
            pairs += 1
        else:
            fails += 1
    if not args.keep_yuv:
        ref_yuv.unlink(missing_ok=True)
    return stem_rows, pairs, fails


def _write_output_parquet(
    rows: list[dict],
    args: argparse.Namespace,
    raw_argv: list[str],
    manifest_len: int,
    pair_count: int,
    fail_count: int,
    resolved_teacher: Any,
    t0: float,
) -> None:
    df = pd.DataFrame(rows)
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
        manifest_items=manifest_len,
        pair_count=pair_count,
        fail_count=fail_count,
        row_count=len(df),
        source_count=int(df["source"].nunique()),
        teacher_model=resolved_teacher.name,
    )
    print(
        f"[ugc-extract] wrote {args.out_parquet} pairs={pair_count} fails={fail_count} "
        f"rows={len(df)} sources={df['source'].nunique()} "
        f"wall={time.monotonic() - t0:.0f}s",
        flush=True,
    )


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    ap = _build_parser()
    args = ap.parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.out_parquet.with_suffix(".manifest.json")

    resolved_teacher = resolve_teacher_model(args.model)
    err = _validate_prerequisites(args, resolved_teacher)
    if err is not None:
        return err

    manifest = json.loads(args.manifest.read_text())
    print(f"[ugc-extract] manifest stems={len(manifest)}", flush=True)

    rows: list[dict] = []
    pair_count = 0
    fail_count = 0
    t0 = time.monotonic()
    for stem, files in sorted(manifest.items()):
        stem_rows, pairs, fails = _process_stem(stem, files, args, resolved_teacher, t0, len(rows))
        rows.extend(stem_rows)
        pair_count += pairs
        fail_count += fails

    if not rows:
        print("error: no rows extracted", file=sys.stderr)
        return 2

    _write_output_parquet(
        rows, args, raw_argv, len(manifest), pair_count, fail_count, resolved_teacher, t0
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
