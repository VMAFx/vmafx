#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Extract middle-frame luma planes from KoNViD-1k MP4s for C2/C3 training.

Reads the populated `konvid-1k.yaml` manifest, drives ffmpeg per clip to
dump the middle frame as 224x224 grayscale `.npy` (uint8), and writes:

  * `<root>/_frames_c2/<key>.npy`        — clean middle-frame luma (C2 input)
  * `<root>/_frames_c3_pairs/<key>_clean.npy`  — C3 self-supervised pair clean
  * `<root>/_frames_c3_pairs/<key>_deg.npy`    — gaussian + JPEG-degraded
  * `ai/data/konvid_frames_nr.parquet`   — (key, frame_path, mos) for C2
  * `ai/data/konvid_pairs_filter.parquet` — (key, deg_path, clean_path) for C3

Idempotent: existing .npy files are skipped. Run once after
`ai/scripts/fetch_konvid_1k.py` + `vmaf-train manifest-scan`.

Usage::

    python ai/scripts/extract_konvid_frames.py
    python ai/scripts/extract_konvid_frames.py --root /custom/path \
                                                 --target-hw 224
"""

from __future__ import annotations

import argparse
import io
import os
import sys
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
import yaml
from PIL import Image

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.run_manifest import write_run_manifest  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

# Bound = max(60, 10x the slowest measured run): one middle-frame extract took
# 0.20 s on a 1080p clip.
FFMPEG_TIMEOUT_S = 60

MANIFEST = REPO_ROOT / "ai" / "src" / "vmaf_train" / "data" / "manifests" / "konvid-1k.yaml"
DATA_DIR = REPO_ROOT / "ai" / "data"

C2_PARQUET = DATA_DIR / "konvid_frames_nr.parquet"
C3_PARQUET = DATA_DIR / "konvid_pairs_filter.parquet"


def _default_root() -> Path:
    env = os.environ.get("VMAF_DATA_ROOT")
    base = Path(env) if env else Path.home() / "datasets"
    return base / "konvid-1k"


def _extract_middle_frame_y(mp4: Path, target_hw: int) -> np.ndarray:
    """Use ffmpeg to read 1 luma frame at the clip midpoint, resized to NxN."""
    # Probe duration once so we can seek to the middle.
    proc = run_command(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "csv=p=0",
            str(mp4),
        ],
        allowed_executables=("ffprobe",),
        check=True,
        capture_output=True,
        text=True,
        timeout_seconds=FFMPEG_TIMEOUT_S,
    )
    dur = float(proc.stdout.strip())
    seek_s = max(0.0, dur / 2.0)

    # Grab a single grayscale frame at target_hw x target_hw via the
    # `gray` pix_fmt (single-channel uint8). Output rawvideo to stdout.
    cmd = [
        "ffmpeg",
        "-loglevel",
        "error",
        "-ss",
        f"{seek_s:.3f}",
        "-i",
        str(mp4),
        "-frames:v",
        "1",
        "-vf",
        f"scale={target_hw}:{target_hw}:flags=area,format=gray",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "gray",
        "-",
    ]
    rv = run_command(
        cmd,
        allowed_executables=("ffmpeg",),
        check=True,
        capture_output=True,
        timeout_seconds=FFMPEG_TIMEOUT_S,
    )
    arr = np.frombuffer(rv.stdout, dtype=np.uint8)
    expected = target_hw * target_hw
    if arr.size != expected:
        raise RuntimeError(f"{mp4.name}: ffmpeg returned {arr.size} bytes, expected {expected}")
    return arr.reshape(target_hw, target_hw)


def _make_degraded(clean: np.ndarray) -> np.ndarray:
    """Self-supervised C3 pair: gaussian blur σ=1.2 + JPEG quality 35."""
    img = Image.fromarray(clean, mode="L")
    # Gaussian blur via PIL (radius is approx-σ).
    from PIL import ImageFilter

    img = img.filter(ImageFilter.GaussianBlur(radius=1.2))
    # Round-trip through JPEG quality 35.
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=35)
    buf.seek(0)
    return np.array(Image.open(buf).convert("L"), dtype=np.uint8)


def _write_manifest(
    *,
    path: Path,
    args: argparse.Namespace,
    raw_argv: list[str],
    root: Path,
    manifest_entries: int,
    processed_count: int,
    missing_count: int,
    error_count: int,
    c2_rows: int,
    c3_rows: int,
) -> None:
    write_run_manifest(
        path,
        schema="konvid-frame-extraction-manifest-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"dataset_root": root, "manifest": MANIFEST},
        outputs={
            "c2_parquet": C2_PARQUET,
            "c3_parquet": C3_PARQUET,
            "c2_frames": root / "_frames_c2",
            "c3_pairs": root / "_frames_c3_pairs",
            "manifest": path,
        },
        sections={
            "manifest_entries": int(manifest_entries),
            "processed_count": int(processed_count),
            "missing_count": int(missing_count),
            "error_count": int(error_count),
            "c2_rows": int(c2_rows),
            "c3_rows": int(c3_rows),
            "target_hw": int(args.target_hw),
        },
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="Dataset root (default: $VMAF_DATA_ROOT/konvid-1k or ~/datasets/konvid-1k)",
    )
    parser.add_argument(
        "--target-hw",
        type=int,
        default=224,
        help="Square frame size after resize (multiple of 32)",
    )
    parser.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Replay manifest JSON sidecar (default: ai/data/konvid_frames_manifest.json).",
    )
    return parser


def _load_entries() -> list[dict[str, Any]]:
    if not MANIFEST.exists():
        raise FileNotFoundError(
            f"manifest not found: {MANIFEST}; run vmaf-train manifest-scan first"
        )

    with MANIFEST.open() as fh:
        doc = yaml.safe_load(fh)
    entries = doc.get("entries") or []
    if not entries:
        raise ValueError("manifest has no entries")
    return entries


def _process_entries(
    entries: list[dict[str, Any]],
    root: Path,
    c2_dir: Path,
    c3_dir: Path,
    target_hw: int,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], int, int]:
    c2_rows: list[dict[str, Any]] = []
    c3_rows: list[dict[str, Any]] = []
    missing_count = 0
    error_count = 0
    n = len(entries)
    for i, e in enumerate(entries):
        key = e["key"]
        mp4 = root / e["path"]
        if not mp4.is_file():
            print(f"[skip] missing {mp4}")
            missing_count += 1
            continue

        c2_path = c2_dir / f"{key}.npy"
        c3_clean_path = c3_dir / f"{key}_clean.npy"
        c3_deg_path = c3_dir / f"{key}_deg.npy"

        if not c2_path.exists():
            try:
                clean = _extract_middle_frame_y(mp4, target_hw)
            except Exception as exc:
                print(f"[error] {mp4.name}: {exc}")
                error_count += 1
                continue
            np.save(c2_path, clean)
        else:
            clean = np.load(c2_path)

        if not c3_clean_path.exists() or not c3_deg_path.exists():
            np.save(c3_clean_path, clean)
            np.save(c3_deg_path, _make_degraded(clean))

        c2_rows.append({"key": key, "frame_path": str(c2_path), "mos": float(e["mos"])})
        c3_rows.append(
            {
                "key": key,
                "deg_path": str(c3_deg_path),
                "clean_path": str(c3_clean_path),
            }
        )
        if (i + 1) % 50 == 0 or i + 1 == n:
            print(f"[extract] {i + 1}/{n} clips")

    return c2_rows, c3_rows, missing_count, error_count


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    args = _build_parser().parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = DATA_DIR / "konvid_frames_manifest.json"

    try:
        entries = _load_entries()
    except (FileNotFoundError, ValueError) as exc:
        sys.exit(str(exc))
    root = args.root or _default_root()
    if not root.is_dir():
        sys.exit(f"dataset root not found: {root}")

    c2_dir = root / "_frames_c2"
    c3_dir = root / "_frames_c3_pairs"
    c2_dir.mkdir(parents=True, exist_ok=True)
    c3_dir.mkdir(parents=True, exist_ok=True)
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    c2_rows, c3_rows, missing_count, error_count = _process_entries(
        entries, root, c2_dir, c3_dir, args.target_hw
    )

    pd.DataFrame(c2_rows).to_parquet(C2_PARQUET, index=False)
    pd.DataFrame(c3_rows).to_parquet(C3_PARQUET, index=False)
    _write_manifest(
        path=args.manifest_out,
        args=args,
        raw_argv=raw_argv,
        root=root,
        manifest_entries=len(entries),
        processed_count=len(c2_rows),
        missing_count=missing_count,
        error_count=error_count,
        c2_rows=len(c2_rows),
        c3_rows=len(c3_rows),
    )
    print(f"[done] C2 parquet: {C2_PARQUET} ({len(c2_rows)} rows)")
    print(f"[done] C3 parquet: {C3_PARQUET} ({len(c3_rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
