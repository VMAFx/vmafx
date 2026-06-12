#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""BVI-DVC → full-feature VMAF parquet (corpus-3 for tiny-AI v2).

Mirrors :mod:`ai.scripts.konvid_to_full_features` (the canonical-feature
KoNViD acquisition pipeline shipped in PR #178) but sources clips from
the BVI-DVC Part 1 dataset (Ma, Zhang, Bull 2021) — a 4-tier 4:2:0
10-bit YCbCr reference corpus distributed as a single ``Videos/``
directory inside ``BVI-DVC Part 1.zip``. Tiers are encoded in the
filename prefix:

    A_3840x2176, B_1920x1088, C_960x544, D_480x272

BVI-DVC ships **reference-only** material — no human DMOS — so we
generate the distorted side ourselves at CRF 35 (matching the KoNViD
flow) and treat the libvmaf score as the teacher signal for the
tiny-AI student.

Output schema (one row per (clip, frame) pair):

    key, frame_index, codec, <25 feature columns>, vmaf

(ADR-0559 extended the feature set from 21 to 25 by appending speed_temporal
and speed_chroma_u/v/uv at the end of FULL_FEATURES.)

The ``codec`` column is the encoder family that produced the distorted
side. BVI-DVC ships reference-only material and this script encodes
internally with ``libx264``, so the column is a constant ``"x264"`` —
captured eagerly so the parquet self-describes for the codec-aware FR
regressor (see [ADR-0235](../../docs/adr/0235-codec-aware-fr-regressor.md)).

Output parquet: ``runs/full_features_bvi_dvc_<tier>.parquet`` (gitignored).
Per-clip JSON cache: ``$XDG_CACHE_HOME/vmaf-tiny-ai-bvi-dvc-full/<key>.json``
(separate from the konvid cache so all three corpora can coexist).

The 10-bit input path is the only structural delta from the konvid
script: ffmpeg gets ``-pix_fmt yuv420p10le`` and libvmaf gets
``--bitdepth 10``; everything else (FULL_FEATURES tuple, EXTRACTORS
tuple, CRF 35, single-thread CPU vmaf, model attached for per-frame
teacher score) is verbatim.

Input modes
-----------
``--bvi-zip PATH``
    Path to ``BVI-DVC Part 1.zip`` (original behaviour). Each clip's
    ``.mp4`` is streamed out of the archive, processed, then deleted.

``--bvi-dir PATH``  (ADR-0527)
    Path to a directory containing already-extracted ``.mp4``, ``.mkv``,
    or ``.yuv`` files. The directory is enumerated recursively for files
    matching the BVI-DVC filename convention. For ``.yuv`` inputs the
    width, height, framerate, and bit-depth are parsed directly from the
    filename; no ffprobe decode step is needed. For video-container
    inputs the existing decode path is used unchanged.

The two flags are mutually exclusive. ``--bvi-zip`` is the default when
neither is provided.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

import pandas as pd

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
REPO_ROOT = _SCRIPT_PATHS.repo_root
from ai.data.feature_extractor import FULL_FEATURES, _extractors_for  # noqa: E402

# isort: split
from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import write_text_atomic  # noqa: E402
from aiutils.parquet_utils import write_parquet_atomic  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

EXTRACTORS = tuple(_extractors_for(FULL_FEATURES))


# Filename pattern for zip-sourced MP4s:
# e.g. "DBookcaseBVITexture_480x272_120fps_10bit_420.mp4".
# Group 1 = tier letter (A/B/C/D), 2 = width, 3 = height.
_NAME_RE = re.compile(r"^([ABCD])[A-Za-z0-9]+_(\d+)x(\d+)_\d+fps_10bit_420\.mp4$")

# Resolution → tier mapping used for --bvi-dir mode (covers the four official tiers).
_RES_TO_TIER: dict[tuple[int, int], str] = {
    (3840, 2176): "A",
    (1920, 1088): "B",
    (960, 544): "C",
    (480, 272): "D",
}

# Filename pattern for pre-extracted files (video containers or YUV).
# Accepts any alphanumeric/underscore stem followed by _WxH_<fps>fps_<depth>bit_420.
# Groups: 1=stem, 2=width, 3=height, 4=fps (int portion), 5=depth, 6=extension.
_DIR_NAME_RE = re.compile(r"^([A-Za-z0-9_\-]+?)_(\d+)x(\d+)_(\d+)fps_(\d+)bit_420\.(mkv|mp4|yuv)$")


def _tier_from_resolution(w: int, h: int) -> str | None:
    """Return the tier letter for a (width, height) pair, or None if unknown."""
    return _RES_TO_TIER.get((w, h))


def _run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, **kw)


def _decode_yuv_10bit(src_video: Path, out_yuv: Path) -> tuple[int, int, int]:
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,nb_frames",
            "-of",
            "json",
            str(src_video),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    info = json.loads(probe.stdout)["streams"][0]
    w = int(info["width"])
    h = int(info["height"])
    nb_frames = int(info.get("nb_frames", 0))
    out_yuv.parent.mkdir(parents=True, exist_ok=True)
    _run(
        [
            "ffmpeg",
            "-y",
            "-loglevel",
            "error",
            "-i",
            str(src_video),
            "-pix_fmt",
            "yuv420p10le",
            "-f",
            "rawvideo",
            str(out_yuv),
        ]
    )
    return w, h, nb_frames


def _encode_dis_10bit(src_video: Path, out_yuv: Path, crf: int) -> None:
    out_yuv.parent.mkdir(parents=True, exist_ok=True)
    # libx264 10-bit lossy compression at CRF 35 (matches KoNViD flow);
    # matroska pipe container so the moov atom doesn't need seekable
    # output. Decode side restores to yuv420p10le raw for libvmaf.
    cmd = (
        f"ffmpeg -y -loglevel error -i {shlex.quote(str(src_video))} "
        f"-c:v libx264 -pix_fmt yuv420p10le -crf {crf} -preset fast -an "
        f"-f matroska pipe:1 | "
        f"ffmpeg -y -loglevel error -i pipe:0 -pix_fmt yuv420p10le "
        f"-f rawvideo {shlex.quote(str(out_yuv))}"
    )
    _run(["bash", "-c", cmd])


def _encode_dis_10bit_from_yuv(
    ref_yuv: Path, out_yuv: Path, w: int, h: int, fps: int, crf: int
) -> None:
    """Encode a distorted YUV from a raw 10-bit 4:2:0 YUV reference.

    Used in ``--bvi-dir`` mode when the source is already a raw YUV
    file — ffmpeg reads it with explicit format flags so it does not try
    to auto-detect a container.
    """
    out_yuv.parent.mkdir(parents=True, exist_ok=True)
    cmd = (
        f"ffmpeg -y -loglevel error "
        f"-f rawvideo -pixel_format yuv420p10le -video_size {w}x{h} -framerate {fps} "
        f"-i {shlex.quote(str(ref_yuv))} "
        f"-c:v libx264 -pix_fmt yuv420p10le -crf {crf} -preset fast -an "
        f"-f matroska pipe:1 | "
        f"ffmpeg -y -loglevel error -i pipe:0 -pix_fmt yuv420p10le "
        f"-f rawvideo {shlex.quote(str(out_yuv))}"
    )
    _run(["bash", "-c", cmd])


def _run_vmaf_full(
    vmaf_bin: Path,
    ref_yuv: Path,
    dis_yuv: Path,
    w: int,
    h: int,
    out_json: Path,
    model_path: Path,
    bitdepth: int = 10,
) -> None:
    """Run libvmaf CLI with all FULL_FEATURES extractors + the
    vmaf_v0.6.1 model attached for the per-frame VMAF teacher score.

    BVI-DVC ships 10-bit; ``--bitdepth 10`` is the only structural
    delta vs. the 8-bit konvid invocation. The ``bitdepth`` parameter
    is exposed so callers that synthesise 8-bit test fixtures can pass
    ``bitdepth=8``.
    """
    feat_args: list[str] = []
    for ex in EXTRACTORS:
        feat_args += ["--feature", ex]
    _run(
        [
            str(vmaf_bin),
            "--reference",
            str(ref_yuv),
            "--distorted",
            str(dis_yuv),
            "--width",
            str(w),
            "--height",
            str(h),
            "--pixel_format",
            "420",
            "--bitdepth",
            str(bitdepth),
            "--model",
            f"path={model_path}",
            *feat_args,
            "--threads",
            "1",
            "--no_cuda",
            "--no_sycl",
            "--output",
            str(out_json),
            "--json",
            "-q",
        ]
    )


def _lookup(metrics: dict, name: str) -> float | None:
    """libvmaf may emit ``integer_<name>`` for fixed-point kernels.

    Returns None when the key is absent OR when libvmaf emits a JSON null
    (e.g. ssimulacra2 / ciede2000 on frames where the metric cannot be
    computed for the source content). The caller maps None to float("nan").
    """
    v = metrics.get(name)
    if v is not None:
        return float(v)
    v = metrics.get(f"integer_{name}")
    if v is not None:
        return float(v)
    return None


def _frames_to_rows(key: str, vmaf_json: Path, codec: str) -> list[dict]:
    with vmaf_json.open() as f:
        d = json.load(f)
    rows = []
    for fr in d["frames"]:
        m = fr["metrics"]
        row: dict = {"key": key, "frame_index": int(fr["frameNum"]), "codec": codec}
        for feat in FULL_FEATURES:
            v = _lookup(m, feat)
            row[feat] = float("nan") if v is None else v
        vmaf_v = m.get("vmaf")
        row["vmaf"] = float("nan") if vmaf_v is None else float(vmaf_v)
        rows.append(row)
    return rows


def _process_clip(
    key: str,
    src_video: Path,
    vmaf_bin: Path,
    model_path: Path,
    crf: int,
    cache_dir: Path | None,
    scratch: Path,
    codec: str,
) -> list[dict]:
    if cache_dir is not None:
        cache_path = cache_dir / f"{key}.json"
        if cache_path.is_file():
            return _frames_to_rows(key, cache_path, codec)
    ref_yuv = scratch / f"{key}_ref.yuv"
    dis_yuv = scratch / f"{key}_dis.yuv"
    vmaf_json = scratch / f"{key}_vmaf.json"
    try:
        w, h, _nb = _decode_yuv_10bit(src_video, ref_yuv)
        _encode_dis_10bit(src_video, dis_yuv, crf)
        _run_vmaf_full(vmaf_bin, ref_yuv, dis_yuv, w, h, vmaf_json, model_path)
        rows = _frames_to_rows(key, vmaf_json, codec)
        if cache_dir is not None:
            cache_dir.mkdir(parents=True, exist_ok=True)
            write_text_atomic(cache_dir / f"{key}.json", vmaf_json.read_text())
        return rows
    finally:
        for p in (ref_yuv, dis_yuv, vmaf_json):
            with contextlib.suppress(FileNotFoundError):
                p.unlink()


def _process_clip_yuv(
    key: str,
    src_yuv: Path,
    w: int,
    h: int,
    fps: int,
    depth: int,
    vmaf_bin: Path,
    model_path: Path,
    crf: int,
    cache_dir: Path | None,
    scratch: Path,
    codec: str,
) -> list[dict]:
    """Process a pre-decoded YUV clip from ``--bvi-dir`` mode.

    The caller has already parsed ``w``, ``h``, ``fps``, and ``depth``
    from the filename so no ffprobe step is needed. The reference YUV
    is used directly; the distorted side is re-encoded via libx264 and
    decoded back to raw YUV just as in the MP4 path.
    """
    if cache_dir is not None:
        cache_path = cache_dir / f"{key}.json"
        if cache_path.is_file():
            return _frames_to_rows(key, cache_path, codec)
    dis_yuv = scratch / f"{key}_dis.yuv"
    vmaf_json = scratch / f"{key}_vmaf.json"
    try:
        _encode_dis_10bit_from_yuv(src_yuv, dis_yuv, w, h, fps, crf)
        _run_vmaf_full(
            vmaf_bin,
            src_yuv,
            dis_yuv,
            w,
            h,
            vmaf_json,
            model_path,
            bitdepth=depth,
        )
        rows = _frames_to_rows(key, vmaf_json, codec)
        if cache_dir is not None:
            cache_dir.mkdir(parents=True, exist_ok=True)
            write_text_atomic(cache_dir / f"{key}.json", vmaf_json.read_text())
        return rows
    finally:
        for p in (dis_yuv, vmaf_json):
            with contextlib.suppress(FileNotFoundError):
                p.unlink()


def _select_tier_entries(zf: zipfile.ZipFile, tier: str) -> list[zipfile.ZipInfo]:
    """Filter the zip's ``Videos/`` entries to a single tier (or all).

    ``tier == "all"`` returns every clip across A/B/C/D in deterministic
    sorted order so reruns give the same indexing.
    """
    selected: list[zipfile.ZipInfo] = []
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = info.filename
        if "/Videos/" not in name or not name.endswith(".mp4"):
            continue
        base = name.rsplit("/", 1)[-1]
        m = _NAME_RE.match(base)
        if m is None:
            continue
        clip_tier = m.group(1)
        if tier != "all" and clip_tier != tier:
            continue
        selected.append(info)
    selected.sort(key=lambda i: i.filename)
    return selected


# ---------------------------------------------------------------------------
# Data type for directory-mode clips (replaces zipfile.ZipInfo).
# ---------------------------------------------------------------------------


class _DirEntry:
    """Lightweight stand-in for a ``zipfile.ZipInfo`` in the dir-scan path."""

    def __init__(self, path: Path, tier: str, w: int, h: int, fps: int, depth: int) -> None:
        self.path = path
        self.tier = tier
        self.w = w
        self.h = h
        self.fps = fps
        self.depth = depth
        # Mimic the ZipInfo attribute used for sorting / display.
        self.filename = str(path)


def _select_tier_entries_dir(bvi_dir: Path, tier: str) -> list[_DirEntry]:
    """Enumerate video-container / ``.yuv`` files in *bvi_dir* that match the
    BVI-DVC naming convention and belong to the requested tier.

    Resolution tiers are determined first from the four canonical
    ``_RES_TO_TIER`` pairs; if no canonical pair matches the parsed
    resolution the file is skipped with a warning.

    ``tier == "all"`` returns every matching clip in deterministic sorted
    order.
    """
    selected: list[_DirEntry] = []
    for path in sorted(bvi_dir.rglob("*")):
        if path.suffix.lower() not in (".mkv", ".mp4", ".yuv"):
            continue
        m = _DIR_NAME_RE.match(path.name)
        if m is None:
            continue
        w = int(m.group(2))
        h = int(m.group(3))
        fps = int(m.group(4))
        depth = int(m.group(5))
        clip_tier = _tier_from_resolution(w, h)
        if clip_tier is None:
            print(
                f"[bvi-dvc-full] warning: {path.name}: resolution {w}x{h} not in "
                "known tier map — skipping",
                file=sys.stderr,
            )
            continue
        if tier != "all" and clip_tier != tier:
            continue
        selected.append(_DirEntry(path, clip_tier, w, h, fps, depth))
    return selected


def _stream_extract(zf: zipfile.ZipFile, info: zipfile.ZipInfo, dest: Path) -> Path:
    """Stream a single zip entry to ``dest`` without unpacking the
    rest of the archive. Returns the dest path."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zf.open(info) as src, dest.open("wb") as out:
        # 4 MiB chunks — keeps RAM bounded for the 372 MB A-tier clips.
        while True:
            chunk = src.read(4 * 1024 * 1024)
            if not chunk:
                break
            out.write(chunk)
    return dest


def _run_zip_mode(
    args: argparse.Namespace, out_path: Path, cache_dir: Path | None
) -> tuple[int, dict[str, object]]:
    """Process clips from a zip archive (original ``--bvi-zip`` path)."""
    if not args.bvi_zip.is_file():
        print(f"error: BVI-DVC zip not found at {args.bvi_zip}", file=sys.stderr)
        return 2, {}

    with zipfile.ZipFile(args.bvi_zip) as zf:
        entries = _select_tier_entries(zf, args.tier)
        if not entries:
            print(
                f"[bvi-dvc-full] ERROR: no BVI-DVC clips found in {args.bvi_zip} "
                f"for tier={args.tier!r}. "
                "Check --bvi-zip and --tier; expected .mp4 entries matching the "
                "BVI-DVC naming convention.",
                file=sys.stderr,
                flush=True,
            )
            return 2, {}
        if args.max_clips is not None:
            entries = entries[: args.max_clips]
        print(
            f"[bvi-dvc-full] mode=zip tier={args.tier} processing "
            f"{len(entries)} clips → {out_path}",
            flush=True,
        )

        rows: list[dict] = []
        t0 = time.time()
        for i, info in enumerate(entries):
            base = info.filename.rsplit("/", 1)[-1]
            key = base[: -len(".mp4")]
            local_mp4 = args.scratch / base
            try:
                # Skip extraction if the cached vmaf JSON already exists —
                # _process_clip will short-circuit before touching the mp4.
                cache_hit = cache_dir is not None and (cache_dir / f"{key}.json").is_file()
                if not cache_hit:
                    _stream_extract(zf, info, local_mp4)
                rows += _process_clip(
                    key,
                    local_mp4,
                    args.vmaf_bin,
                    args.model,
                    args.crf,
                    cache_dir,
                    args.scratch,
                    args.codec,
                )
            finally:
                with contextlib.suppress(FileNotFoundError):
                    local_mp4.unlink()
            if (i + 1) % 5 == 0 or (i + 1) == len(entries):
                wt = time.time() - t0
                print(
                    f"[bvi-dvc-full] {i + 1}/{len(entries)} clips, {len(rows)} frames, {wt:.1f}s",
                    flush=True,
                )

    stats = _write_parquet(rows, len(entries), out_path)
    stats["input_mode"] = "zip"
    stats["tier"] = args.tier
    return 0, stats


def _run_dir_mode(
    args: argparse.Namespace, out_path: Path, cache_dir: Path | None
) -> tuple[int, dict[str, object]]:
    """Process clips from a pre-extracted directory (``--bvi-dir`` path).

    Each file in the directory is processed in-place — no extraction
    step, no deletion.  Video-container files go through the standard
    decode → encode-distorted path; ``.yuv`` files skip the decode step
    entirely and are passed directly to libvmaf as the reference.
    """
    entries = _select_tier_entries_dir(args.bvi_dir, args.tier)
    if not entries:
        print(
            f"[bvi-dvc-full] ERROR: no BVI-DVC clips found in {args.bvi_dir} "
            f"for tier={args.tier!r}. "
            "Check --bvi-dir and --tier; expected files matching the BVI-DVC "
            "naming convention (e.g. BVI_DVC_tier1_*.yuv / *.mkv / *.mp4).",
            file=sys.stderr,
            flush=True,
        )
        return 2, {}
    if args.max_clips is not None:
        entries = entries[: args.max_clips]
    print(
        f"[bvi-dvc-full] mode=dir tier={args.tier} processing {len(entries)} clips → {out_path}",
        flush=True,
    )

    rows: list[dict] = []
    t0 = time.time()
    for i, entry in enumerate(entries):
        key = entry.path.stem
        if entry.path.suffix.lower() == ".yuv":
            rows += _process_clip_yuv(
                key,
                entry.path,
                entry.w,
                entry.h,
                entry.fps,
                entry.depth,
                args.vmaf_bin,
                args.model,
                args.crf,
                cache_dir,
                args.scratch,
                args.codec,
            )
        else:
            # Video-container path: decode to YUV, then re-encode a distorted side.
            local_mp4 = entry.path
            rows += _process_clip(
                key,
                local_mp4,
                args.vmaf_bin,
                args.model,
                args.crf,
                cache_dir,
                args.scratch,
                args.codec,
            )
        if (i + 1) % 5 == 0 or (i + 1) == len(entries):
            wt = time.time() - t0
            print(
                f"[bvi-dvc-full] {i + 1}/{len(entries)} clips, {len(rows)} frames, {wt:.1f}s",
                flush=True,
            )

    stats = _write_parquet(rows, len(entries), out_path)
    stats["input_mode"] = "dir"
    stats["tier"] = args.tier
    return 0, stats


def _write_parquet(rows: list[dict], n_clips: int, out_path: Path) -> dict[str, object]:
    df = pd.DataFrame(rows)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    write_parquet_atomic(df, out_path)
    print(
        f"[bvi-dvc-full] wrote {out_path} ({len(df)} frames, "
        f"{n_clips} clips, {len(df.columns)} cols)",
        flush=True,
    )
    return {
        "clips_selected": n_clips,
        "frames": len(df),
        "columns": len(df.columns),
    }


def _write_manifest(
    path: Path,
    *,
    args: argparse.Namespace,
    argv: list[str] | None,
    out_path: Path,
    cache_dir: Path | None,
    stats: dict[str, object],
) -> None:
    write_manifest_json(
        path,
        {
            "schema": "bvi-dvc-full-features-manifest-v1",
            "features": list(FULL_FEATURES),
            "extractors": list(EXTRACTORS),
            "crf": args.crf,
            "codec": args.codec,
            "cache": {
                "enabled": cache_dir is not None,
                "directory": None if cache_dir is None else str(cache_dir),
            },
            "stats": stats,
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=REPO_ROOT,
                argv=sys.argv[1:] if argv is None else argv,
                args=args,
                inputs={
                    "bvi_zip": args.bvi_zip,
                    "bvi_dir": args.bvi_dir,
                    "cache_dir": cache_dir,
                    "scratch": args.scratch,
                    "vmaf_bin": args.vmaf_bin,
                    "model": args.model,
                },
                outputs={
                    "parquet": out_path,
                    "manifest": args.manifest_out,
                },
            ),
        },
    )


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(
        prog="bvi_dvc_to_full_features.py",
        description=__doc__,
    )

    # Mutually exclusive input-source group (ADR-0524).
    src_group = ap.add_mutually_exclusive_group()
    src_group.add_argument(
        "--bvi-zip",
        type=Path,
        default=None,
        help="Path to BVI-DVC Part 1.zip. Mutually exclusive with --bvi-dir.",
    )
    src_group.add_argument(
        "--bvi-dir",
        type=Path,
        default=None,
        help=(
            "Path to a directory containing already-extracted BVI-DVC "
            ".mp4, .mkv, or .yuv files. Files matching the BVI-DVC naming "
            "convention (e.g. ``Clip_480x272_30fps_8bit_420.yuv``) are "
            "enumerated and processed without extraction. "
            "Mutually exclusive with --bvi-zip."
        ),
    )

    ap.add_argument(
        "--tier",
        choices=("A", "B", "C", "D", "all"),
        default="D",
        help="Resolution tier to process (A=3840x2176, B=1920x1088, "
        "C=960x544, D=480x272, all=every tier in sorted order).",
    )
    ap.add_argument(
        "--vmaf-bin",
        type=Path,
        default=REPO_ROOT / "core" / "build-cpu" / "tools" / "vmaf",
        help="Path to the vmaf CLI binary.",
    )
    ap.add_argument(
        "--model",
        type=Path,
        default=REPO_ROOT / "model" / "vmaf_v0.6.1.json",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output parquet (default: runs/full_features_bvi_dvc_<tier>.parquet).",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <out>.manifest.json and "
            "records BVI input mode, tier, cache/model inputs, FULL_FEATURES "
            "schema, row counts, and exact CLI args."
        ),
    )
    ap.add_argument(
        "--scratch",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_TINY_AI_SCRATCH",
                str(Path(tempfile.gettempdir()) / "bvi_dvc_full_acquire"),
            )
        ),
    )
    ap.add_argument(
        "--cache-dir",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_TINY_AI_CACHE_BVI_DVC_FULL",
                str(Path.home() / ".cache" / "vmaf-tiny-ai-bvi-dvc-full"),
            )
        ),
    )
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--crf", type=int, default=35)
    ap.add_argument("--max-clips", type=int, default=None)
    ap.add_argument(
        "--codec",
        type=str,
        default="x264",
        help="Codec label baked into the parquet's `codec` column. Must "
        "match an entry in ai/src/vmaf_train/codec.py CODEC_VOCAB (or it "
        "will bucket to 'unknown' at training time). The script always "
        "encodes via libx264 today; this flag exists so a future "
        "multi-codec sweep can reuse the same harness.",
    )
    args = ap.parse_args(raw_argv)

    # Resolve the default input source: if neither flag was given, fall
    # back to the legacy VMAF_BVI_DVC_ZIP env-var / hard-coded path so
    # existing callers that omit --bvi-zip keep working.
    if args.bvi_zip is None and args.bvi_dir is None:
        args.bvi_zip = Path(
            os.environ.get(
                "VMAF_BVI_DVC_ZIP",
                str(REPO_ROOT / ".corpus" / "bvi-dvc-raw" / "BVI-DVC Part 1.zip"),
            )
        )

    if not args.vmaf_bin.is_file():
        print(f"error: vmaf binary not found at {args.vmaf_bin}", file=sys.stderr)
        return 2
    if not args.model.is_file():
        print(f"error: model not found at {args.model}", file=sys.stderr)
        return 2

    out_path = args.out or (REPO_ROOT / "runs" / f"full_features_bvi_dvc_{args.tier}.parquet")
    args.out = out_path
    if args.manifest_out is None:
        args.manifest_out = out_path.with_suffix(".manifest.json")
    args.scratch.mkdir(parents=True, exist_ok=True)
    cache_dir = None if args.no_cache else args.cache_dir
    if cache_dir is not None:
        cache_dir.mkdir(parents=True, exist_ok=True)

    if args.bvi_dir is not None:
        if not args.bvi_dir.is_dir():
            print(f"error: --bvi-dir path is not a directory: {args.bvi_dir}", file=sys.stderr)
            return 2
        rc, stats = _run_dir_mode(args, out_path, cache_dir)
    else:
        # --bvi-zip path (original behaviour).
        rc, stats = _run_zip_mode(args, out_path, cache_dir)

    if rc == 0:
        _write_manifest(
            args.manifest_out,
            args=args,
            argv=raw_argv,
            out_path=out_path,
            cache_dir=cache_dir,
            stats=stats,
        )
        print(f"[bvi-dvc-full] wrote manifest {args.manifest_out}", flush=True)
    return rc


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
