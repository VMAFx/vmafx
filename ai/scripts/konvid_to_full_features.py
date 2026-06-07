#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""KoNViD-1k -> FULL_FEATURES VMAF parquet.

This is the full-feature companion to ``konvid_to_vmaf_pairs.py``. It
keeps the same synthetic-FR recipe:

1. Decode each KoNViD-1k source MP4 to 8-bit 4:2:0 YUV as the reference.
2. Encode a distorted side with libx264 at CRF 35 and decode it back to YUV.
3. Run the fork libvmaf CLI once with ``FULL_FEATURES`` and
   ``vmaf_v0.6.1`` attached.
4. Write one parquet row per frame.

Default outputs:

* ``runs/full_features_konvid.parquet``
* ``runs/full_features_konvid_with_folds.parquet``

The folded file adds ``source=fold0..fold4`` using a deterministic
balanced hash order over clip keys; it feeds ``eval_multiseed_v3_v4.py``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pandas as pd

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
REPO_ROOT = _SCRIPT_PATHS.repo_root
from ai.data.feature_extractor import (  # noqa: E402
    DEFAULT_VMAF_BINARY,
    FULL_FEATURES,
    _extractors_for,
)

# isort: split
from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import write_text_atomic  # noqa: E402
from aiutils.parquet_utils import write_parquet_atomic  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402


def _run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, **kw)


def _default_konvid_root() -> Path:
    env = os.environ.get("VMAF_KONVID_1K_DIR")
    if env:
        return Path(env)
    return Path(os.environ.get("VMAF_DATA_ROOT", str(Path.home() / "datasets"))) / "konvid-1k"


def _resolve_videos_dir(konvid_root: Path) -> Path:
    candidates = (
        konvid_root / "KoNViD_1k_videos",
        konvid_root / "videos",
        konvid_root,
    )
    for candidate in candidates:
        if candidate.is_dir() and any(candidate.glob("*.mp4")):
            return candidate
    raise FileNotFoundError(
        f"KoNViD-1k videos not found under {konvid_root}; expected "
        "KoNViD_1k_videos/*.mp4 or videos/*.mp4"
    )


def _ffprobe_video(src_mp4: Path) -> tuple[int, int, str]:
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,codec_name",
            "-of",
            "json",
            str(src_mp4),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    stream = json.loads(probe.stdout)["streams"][0]
    return int(stream["width"]), int(stream["height"]), str(stream.get("codec_name", "unknown"))


def _decode_yuv(src_mp4: Path, out_yuv: Path) -> tuple[int, int, str]:
    width, height, codec_name = _ffprobe_video(src_mp4)
    out_yuv.parent.mkdir(parents=True, exist_ok=True)
    _run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(src_mp4),
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(out_yuv),
        ]
    )
    return width, height, codec_name


def _encode_dis(src_mp4: Path, out_yuv: Path, crf: int) -> None:
    intermediate = out_yuv.with_suffix(".dis.mp4")
    _run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(src_mp4),
            "-an",
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            str(crf),
            "-pix_fmt",
            "yuv420p",
            str(intermediate),
        ]
    )
    _run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(intermediate),
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(out_yuv),
        ]
    )
    intermediate.unlink(missing_ok=True)


def _run_vmaf_full(
    vmaf_bin: Path,
    ref_yuv: Path,
    dis_yuv: Path,
    width: int,
    height: int,
    out_json: Path,
    model_path: Path,
) -> None:
    feat_args: list[str] = []
    for extractor in _extractors_for(FULL_FEATURES):
        feat_args += ["--feature", extractor]
    _run(
        [
            str(vmaf_bin),
            "--reference",
            str(ref_yuv),
            "--distorted",
            str(dis_yuv),
            "--width",
            str(width),
            "--height",
            str(height),
            "--pixel_format",
            "420",
            "--bitdepth",
            "8",
            "--model",
            f"path={model_path}",
            *feat_args,
            "--threads",
            "1",
            "--no_cuda",
            "--no_sycl",
            "--no_vulkan",
            "--output",
            str(out_json),
            "--json",
            "-q",
        ]
    )


def _lookup(metrics: dict, name: str) -> float | None:
    value = metrics.get(name)
    if value is not None:
        return float(value)
    value = metrics.get(f"integer_{name}")
    if value is not None:
        return float(value)
    return None


def _frames_to_rows(key: str, vmaf_json: Path, codec: str) -> list[dict]:
    doc = json.loads(vmaf_json.read_text())
    rows: list[dict] = []
    for frame in doc.get("frames", []):
        metrics = frame.get("metrics", {})
        row: dict = {
            "key": key,
            "frame_index": int(frame["frameNum"]),
            "codec": codec,
        }
        for feature in FULL_FEATURES:
            value = _lookup(metrics, feature)
            row[feature] = float("nan") if value is None else value
        vmaf = metrics.get("vmaf")
        row["vmaf"] = float("nan") if vmaf is None else float(vmaf)
        rows.append(row)
    return rows


def _cache_path(cache_dir: Path, key: str, crf: int) -> Path:
    return cache_dir / f"features_{len(FULL_FEATURES)}" / f"crf_{crf}" / f"{key}.json"


def _process_clip(
    key: str,
    src_mp4: Path,
    vmaf_bin: Path,
    model_path: Path,
    crf: int,
    cache_dir: Path | None,
    scratch: Path,
    codec_label: str,
    codec_from_source: bool,
) -> list[dict]:
    if cache_dir is not None:
        cache_path = _cache_path(cache_dir, key, crf)
        if cache_path.is_file():
            codec = _ffprobe_video(src_mp4)[2] if codec_from_source else codec_label
            return _frames_to_rows(cache_path.stem, cache_path, codec)

    ref_yuv = scratch / f"{key}_ref.yuv"
    dis_yuv = scratch / f"{key}_dis.yuv"
    vmaf_json = scratch / f"{key}_vmaf.json"
    try:
        width, height, source_codec = _decode_yuv(src_mp4, ref_yuv)
        _encode_dis(src_mp4, dis_yuv, crf)
        _run_vmaf_full(vmaf_bin, ref_yuv, dis_yuv, width, height, vmaf_json, model_path)
        codec = source_codec if codec_from_source else codec_label
        rows = _frames_to_rows(key, vmaf_json, codec)
        if cache_dir is not None:
            cache_path = _cache_path(cache_dir, key, crf)
            cache_path.parent.mkdir(parents=True, exist_ok=True)
            write_text_atomic(cache_path, vmaf_json.read_text())
        return rows
    finally:
        ref_yuv.unlink(missing_ok=True)
        dis_yuv.unlink(missing_ok=True)
        vmaf_json.unlink(missing_ok=True)


def _assign_folds(keys: list[str], fold_count: int) -> dict[str, str]:
    if fold_count <= 1:
        raise ValueError("fold_count must be > 1")
    ordered = sorted(keys, key=lambda k: hashlib.sha256(k.encode("utf-8")).hexdigest())
    return {key: f"fold{idx % fold_count}" for idx, key in enumerate(ordered)}


def _write_outputs(
    rows: list[dict],
    out_path: Path,
    folds_out: Path | None,
    fold_count: int,
) -> None:
    df = pd.DataFrame(rows)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    write_parquet_atomic(df, out_path)
    print(
        f"[konvid-full] wrote {out_path} ({len(df)} frames, "
        f"{df['key'].nunique() if 'key' in df.columns else 0} clips, {len(df.columns)} cols)",
        flush=True,
    )
    if folds_out is None:
        return
    if df.empty:
        folds_out.parent.mkdir(parents=True, exist_ok=True)
        write_parquet_atomic(df.assign(source=pd.Series(dtype=str)), folds_out)
        print(f"[konvid-full] wrote {folds_out} (empty folded output)", flush=True)
        return
    keys = sorted(str(k) for k in df["key"].dropna().unique())
    fold_by_key = _assign_folds(keys, fold_count)
    folded = df.copy()
    folded["source"] = folded["key"].map(fold_by_key)
    folds_out.parent.mkdir(parents=True, exist_ok=True)
    write_parquet_atomic(folded, folds_out)
    print(
        f"[konvid-full] wrote {folds_out} "
        f"(folds={folded['source'].value_counts().sort_index().to_dict()})",
        flush=True,
    )


def _write_manifest(
    path: Path,
    *,
    args: argparse.Namespace,
    argv: list[str] | None,
    videos_dir: Path,
    clips_selected: int,
    rows: list[dict],
    folds_out: Path | None,
    elapsed_s: float,
) -> None:
    clips_processed = len({str(row["key"]) for row in rows if "key" in row})
    write_manifest_json(
        path,
        {
            "schema": "konvid-full-features-manifest-v1",
            "features": list(FULL_FEATURES),
            "crf": args.crf,
            "codec": args.codec,
            "codec_from_source": bool(args.codec_from_source),
            "folds": {
                "enabled": folds_out is not None,
                "fold_count": args.fold_count,
            },
            "cache": {
                "enabled": not args.no_cache,
                "directory": None if args.no_cache else str(args.cache_dir),
            },
            "stats": {
                "clips_selected": clips_selected,
                "clips_processed": clips_processed,
                "frames": len(rows),
                "columns": 0 if not rows else len(rows[0]),
                "elapsed_s": round(elapsed_s, 6),
            },
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=REPO_ROOT,
                argv=sys.argv[1:] if argv is None else argv,
                args=args,
                inputs={
                    "konvid_root": args.konvid_root,
                    "videos_dir": videos_dir,
                    "cache_dir": None if args.no_cache else args.cache_dir,
                    "vmaf_bin": args.vmaf_bin,
                    "model": args.model,
                },
                outputs={
                    "parquet": args.out,
                    "folded_parquet": folds_out,
                    "manifest": args.manifest_out,
                },
            ),
        },
    )


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    parser = make_argument_parser(
        prog="konvid_to_full_features.py",
        description=__doc__,
    )
    parser.add_argument(
        "--konvid-root",
        type=Path,
        default=_default_konvid_root(),
        help="KoNViD-1k root. Expected to contain KoNViD_1k_videos/*.mp4.",
    )
    parser.add_argument(
        "--vmaf-bin",
        type=Path,
        default=DEFAULT_VMAF_BINARY,
        help="Path to the fork libvmaf CLI binary.",
    )
    parser.add_argument(
        "--model",
        type=Path,
        default=REPO_ROOT / "model" / "vmaf_v0.6.1.json",
        help="Path to the vmaf_v0.6.1 model JSON.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=REPO_ROOT / "runs" / "full_features_konvid.parquet",
        help="Output parquet without folds.",
    )
    parser.add_argument(
        "--folds-out",
        type=Path,
        default=REPO_ROOT / "runs" / "full_features_konvid_with_folds.parquet",
        help="Optional folded output parquet with source=foldN.",
    )
    parser.add_argument("--no-folds-out", action="store_true")
    parser.add_argument("--fold-count", type=int, default=5)
    parser.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <out>.manifest.json and "
            "records KoNViD root/cache/model inputs, FULL_FEATURES schema, "
            "fold output, row counts, and exact CLI args."
        ),
    )
    parser.add_argument(
        "--scratch",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_TINY_AI_SCRATCH",
                str(Path(tempfile.gettempdir()) / "konvid_full_acquire"),
            )
        ),
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_TINY_AI_CACHE_KONVID_FULL",
                str(Path.home() / ".cache" / "vmaf-tiny-ai-konvid-full"),
            )
        ),
    )
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument("--crf", type=int, default=35)
    parser.add_argument("--max-clips", type=int, default=None)
    parser.add_argument(
        "--codec",
        type=str,
        default="x264",
        help="Codec label baked into the parquet. The script encodes the distorted side via libx264.",
    )
    parser.add_argument(
        "--codec-from-source",
        action="store_true",
        help="Use ffprobe's source codec_name as the codec label instead of --codec.",
    )
    args = parser.parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.out.with_suffix(".manifest.json")

    if not args.vmaf_bin.is_file():
        print(f"error: vmaf binary not found at {args.vmaf_bin}", file=sys.stderr)
        return 2
    if not args.model.is_file():
        print(f"error: model not found at {args.model}", file=sys.stderr)
        return 2

    try:
        videos_dir = _resolve_videos_dir(args.konvid_root)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    clips = sorted(videos_dir.glob("*.mp4"))
    if args.max_clips is not None:
        clips = clips[: args.max_clips]
    if not clips:
        print(f"error: no .mp4 clips found in {videos_dir}", file=sys.stderr)
        return 2

    args.scratch.mkdir(parents=True, exist_ok=True)
    cache_dir = None if args.no_cache else args.cache_dir
    if cache_dir is not None:
        cache_dir.mkdir(parents=True, exist_ok=True)

    print(
        f"[konvid-full] processing {len(clips)} clips from {videos_dir} -> {args.out}",
        flush=True,
    )
    rows: list[dict] = []
    t0 = time.monotonic()
    for idx, clip in enumerate(clips):
        key = clip.stem
        try:
            rows.extend(
                _process_clip(
                    key,
                    clip,
                    args.vmaf_bin,
                    args.model,
                    args.crf,
                    cache_dir,
                    args.scratch,
                    args.codec,
                    args.codec_from_source,
                )
            )
        except subprocess.CalledProcessError as exc:
            print(f"[konvid-full] {key} FAILED: {shlex.join(exc.cmd)}", file=sys.stderr)
            continue
        if (idx + 1) % 10 == 0 or idx + 1 == len(clips):
            print(
                f"[konvid-full] {idx + 1}/{len(clips)} clips, {len(rows)} frames, "
                f"{time.monotonic() - t0:.1f}s",
                flush=True,
            )

    folds_out = None if args.no_folds_out else args.folds_out
    _write_outputs(rows, args.out, folds_out, args.fold_count)
    _write_manifest(
        args.manifest_out,
        args=args,
        argv=raw_argv,
        videos_dir=videos_dir,
        clips_selected=len(clips),
        rows=rows,
        folds_out=folds_out,
        elapsed_s=time.monotonic() - t0,
    )
    print(f"[konvid-full] wrote manifest {args.manifest_out}", flush=True)
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
