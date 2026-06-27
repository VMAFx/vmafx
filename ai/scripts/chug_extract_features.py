#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Materialise full-reference feature rows from local CHUG clips.

CHUG ships one reference row plus six bitrate-ladder rows for each
``chug_content_name``.  ``chug_to_corpus_jsonl.py`` downloads and probes
those clips; this script turns the local JSONL into MOS-head training
rows with real libvmaf features by pairing each distorted clip with its
matching reference clip.

The distorted side is spatially scaled to the reference geometry before
feature extraction.  CHUG's ladder rows intentionally differ in
resolution, while libvmaf's raw-YUV CLI path expects equal geometry.
The output rows remain local-only under ``.workingdir2/chug/``.
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
from collections import Counter
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from ai.data.feature_extractor import (  # noqa: E402
    DEFAULT_FEATURES,
    DEFAULT_VMAF_BINARY,
    FULL_FEATURES,
    FeatureExtractionResult,
    aggregate_clip_stats,
    extract_features,
)

# isort: split
from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import write_text_atomic  # noqa: E402
from aiutils.run_manifest import build_run_provenance  # noqa: E402

# Default working directory for CHUG feature extraction; override with
# ``VMAF_CHUG_DIR`` env var for container / non-maintainer layouts.
DEFAULT_CHUG_DIR = Path(
    os.environ.get(
        "VMAF_CHUG_DIR",
        str(Path(__file__).resolve().parents[2] / ".corpus" / "chug"),
    )
)
DEFAULT_INPUT = DEFAULT_CHUG_DIR / "chug.jsonl"
DEFAULT_OUTPUT = DEFAULT_CHUG_DIR / "chug_features.jsonl"
DEFAULT_CLIPS_DIR = DEFAULT_CHUG_DIR / "clips"
DEFAULT_CACHE_DIR = DEFAULT_CHUG_DIR / "feature-cache"
DEFAULT_FEATURE_SET = "canonical"
DEFAULT_SPLIT_SEED = "chug-hdr-v1"
SPLIT_NAMES = ("train", "val", "test")
HDR_METADATA_FIELDS: tuple[str, ...] = (
    "codec_name",
    "pix_fmt",
    "color_transfer",
    "transfer_class",
    "color_primaries",
    "color_space",
    "color_range",
    "max_content_nits",
    "max_average_nits",
)
VISUAL_SIGNAL_FIELDS: tuple[str, ...] = (
    "luma_std",
    "sharpness_laplacian_var",
    "highfreq_abs_mean",
    "noise_lap_mad",
)
FEATURE_SETS: dict[str, tuple[str, ...]] = {
    "canonical": DEFAULT_FEATURES,
    "full": FULL_FEATURES,
}


@dataclass(frozen=True)
class FeaturePair:
    """One CHUG distorted/reference pair ready for feature extraction."""

    row: dict[str, Any]
    ref_row: dict[str, Any]
    dis_path: Path
    ref_path: Path
    width: int
    height: int
    split: str
    split_key: str


@dataclass(frozen=True)
class ExtractedPairPayload:
    """Cached libvmaf result plus decoded-luma side signals."""

    result: FeatureExtractionResult
    ref_visual_signals: dict[str, float | None]
    dis_visual_signals: dict[str, float | None]


def content_split_for(
    content_name: str,
    *,
    seed: str = DEFAULT_SPLIT_SEED,
    train_ratio: float = 0.80,
    val_ratio: float = 0.10,
) -> str:
    """Return a deterministic content-level train/val/test split."""
    key = f"{seed}\0{content_name}".encode("utf-8")
    digest = hashlib.blake2s(key, digest_size=8).digest()
    value = int.from_bytes(digest, "big") / float(1 << 64)
    if value < train_ratio:
        return "train"
    if value < train_ratio + val_ratio:
        return "val"
    return "test"


def build_content_split_map(
    rows: Iterable[dict[str, Any]],
    *,
    seed: str = DEFAULT_SPLIT_SEED,
) -> dict[str, str]:
    """Return ``{chug_content_name: split}`` without looking at labels."""
    contents = sorted(
        {
            str(row.get("chug_content_name", "")).strip()
            for row in rows
            if str(row.get("chug_content_name", "")).strip()
        }
    )
    return {content: content_split_for(content, seed=seed) for content in contents}


def write_split_manifest(
    rows: Iterable[dict[str, Any]],
    *,
    output: Path,
    seed: str = DEFAULT_SPLIT_SEED,
    run_provenance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Write a local-only split manifest keyed by CHUG content."""
    rows_list = list(rows)
    split_map = build_content_split_map(rows_list, seed=seed)
    counts = Counter(split_map.values())
    payload = {
        "policy": "content-name-blake2s-80-10-10",
        "seed": seed,
        "n_contents": len(split_map),
        "counts": {name: int(counts.get(name, 0)) for name in SPLIT_NAMES},
        "splits": split_map,
    }
    if run_provenance is not None:
        payload["run_provenance"] = run_provenance
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return payload


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line:
                continue
            rows.append(json.loads(line))
    return rows


def _truthy_ref_flag(row: dict[str, Any]) -> bool:
    raw = row.get("chug_ref")
    if isinstance(raw, bool):
        return raw
    try:
        return int(raw) == 1
    except (TypeError, ValueError):
        return False


def is_reference_row(row: dict[str, Any]) -> bool:
    """Return true for CHUG reference rows."""
    bitrate = str(row.get("chug_bitrate_label", "")).strip().lower()
    ladder = str(row.get("chug_bitladder", "")).strip().lower()
    return _truthy_ref_flag(row) or bitrate == "ref" or "_ref" in ladder


def _row_clip_path(row: dict[str, Any], clips_dir: Path) -> Path:
    return clips_dir / str(row["src"])


def _ffprobe_hdr_payload(
    clip_path: Path,
    *,
    ffprobe_bin: str,
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> dict[str, Any] | None:
    cmd = [
        ffprobe_bin,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,pix_fmt,codec_name,color_transfer,color_primaries,"
        "color_space,color_range:stream_side_data=side_data_type,max_content,max_average",
        "-of",
        "json",
        str(clip_path),
    ]
    try:
        proc = runner(cmd, check=False, capture_output=True, text=True)
    except (FileNotFoundError, OSError):
        return None
    if int(getattr(proc, "returncode", 1)) != 0:
        return None
    try:
        return json.loads(getattr(proc, "stdout", "") or "{}")
    except json.JSONDecodeError:
        return None


def _classify_transfer(raw: str) -> str:
    text = raw.strip().lower()
    if text in {"smpte2084", "smpte-st-2084", "smpte_st_2084"}:
        return "pq"
    if text in {"arib-std-b67", "arib_std_b67", "aribstdb67", "hlg"}:
        return "hlg"
    if not text:
        return "unknown"
    return "sdr"


def _normalised_stream_text(stream: dict[str, Any], key: str) -> str:
    value = str(stream.get(key) or "").strip().lower()
    return value or "unknown"


def _metadata_number(value: Any) -> float | None:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def _side_data_number(stream: dict[str, Any], key: str) -> float | None:
    direct = _metadata_number(stream.get(key))
    if direct is not None:
        return direct
    for entry in stream.get("side_data_list") or []:
        parsed = _metadata_number(entry.get(key))
        if parsed is not None:
            return parsed
    return None


def hdr_metadata_from_payload(payload: dict[str, Any] | None) -> dict[str, Any]:
    """Return normalized HDR/display metadata from one ffprobe JSON payload."""
    streams = (payload or {}).get("streams") or []
    stream = streams[0] if streams else {}
    transfer = _normalised_stream_text(stream, "color_transfer")
    return {
        "codec_name": _normalised_stream_text(stream, "codec_name"),
        "pix_fmt": _normalised_stream_text(stream, "pix_fmt"),
        "color_transfer": transfer,
        "transfer_class": _classify_transfer("" if transfer == "unknown" else transfer),
        "color_primaries": _normalised_stream_text(stream, "color_primaries"),
        "color_space": _normalised_stream_text(stream, "color_space"),
        "color_range": _normalised_stream_text(stream, "color_range"),
        "max_content_nits": _side_data_number(stream, "max_content"),
        "max_average_nits": _side_data_number(stream, "max_average"),
    }


def probe_clip_hdr_metadata(
    clip_path: Path,
    *,
    ffprobe_bin: str = "ffprobe",
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> dict[str, Any]:
    """Probe one local clip and return row-safe HDR/display metadata."""
    payload = _ffprobe_hdr_payload(clip_path, ffprobe_bin=ffprobe_bin, runner=runner)
    return hdr_metadata_from_payload(payload)


def _prefixed_hdr_metadata(prefix: str, metadata: dict[str, Any] | None) -> dict[str, Any]:
    values = metadata or hdr_metadata_from_payload(None)
    return {f"{prefix}_{field}": values.get(field) for field in HDR_METADATA_FIELDS}


def audit_chug_hdr_metadata(
    rows: Iterable[dict[str, Any]],
    *,
    clips_dir: Path,
    output: Path,
    split_seed: str = DEFAULT_SPLIT_SEED,
    ffprobe_bin: str = "ffprobe",
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    run_provenance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Probe local CHUG clips and write a compact HDR metadata audit."""
    rows_list = list(rows)
    split_map = build_content_split_map(rows_list, seed=split_seed)
    transfer_counts: Counter[str] = Counter()
    primaries_counts: Counter[str] = Counter()
    pix_fmt_counts: Counter[str] = Counter()
    split_row_counts: Counter[str] = Counter()
    malformed: list[dict[str, Any]] = []
    missing = 0
    probe_failed = 0
    probed = 0

    for row in rows_list:
        clip_path = _row_clip_path(row, clips_dir)
        content = str(row.get("chug_content_name", "")).strip()
        split = split_map.get(content, "unknown")
        split_row_counts[split] += 1
        if not clip_path.is_file():
            missing += 1
            continue
        payload = _ffprobe_hdr_payload(clip_path, ffprobe_bin=ffprobe_bin, runner=runner)
        streams = (payload or {}).get("streams") or []
        if not streams:
            probe_failed += 1
            continue
        metadata = hdr_metadata_from_payload(payload)
        probed += 1
        transfer = metadata["transfer_class"]
        primaries = metadata["color_primaries"]
        pix_fmt = metadata["pix_fmt"]
        transfer_counts[transfer] += 1
        primaries_counts[primaries] += 1
        pix_fmt_counts[pix_fmt] += 1
        if transfer in {"pq", "hlg"} and primaries not in {
            "bt2020",
            "bt2020nc",
            "bt2020-ncl",
            "bt2020c",
            "bt2020-cl",
        }:
            malformed.append(
                {
                    "src": row.get("src", ""),
                    "chug_content_name": content,
                    "split": split,
                    "color_transfer": metadata["color_transfer"],
                    "color_primaries": primaries,
                    "pix_fmt": pix_fmt,
                }
            )

    payload = {
        "policy": "chug-hdr-ffprobe-audit-v1",
        "split_policy": "content-name-blake2s-80-10-10",
        "split_seed": split_seed,
        "rows": len(rows_list),
        "probed": probed,
        "missing_files": missing,
        "probe_failed": probe_failed,
        "transfer_counts": dict(sorted(transfer_counts.items())),
        "primaries_counts": dict(sorted(primaries_counts.items())),
        "pix_fmt_counts": dict(sorted(pix_fmt_counts.items())),
        "split_row_counts": dict(sorted(split_row_counts.items())),
        "malformed_hdr_rows": malformed,
    }
    if run_provenance is not None:
        payload["run_provenance"] = run_provenance
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return payload


def build_feature_pairs(
    rows: Iterable[dict[str, Any]],
    *,
    clips_dir: Path,
    split_map: dict[str, str] | None = None,
    split: str = "all",
    include_reference_identity: bool = False,
) -> list[FeaturePair]:
    """Pair CHUG distorted rows with the matching reference row.

    Rows without a ``chug_content_name`` or without an available reference
    are skipped.  Reference identity pairs are opt-in because they add a
    large cluster of near-perfect samples that can dominate small smoke
    runs.
    """
    rows_list = list(rows)
    refs: dict[str, dict[str, Any]] = {}
    for row in rows_list:
        content = str(row.get("chug_content_name", ""))
        if content and is_reference_row(row):
            refs[content] = row

    pairs: list[FeaturePair] = []
    for row in rows_list:
        content = str(row.get("chug_content_name", ""))
        if not content:
            continue
        row_split = (split_map or {}).get(content) or content_split_for(content)
        if split != "all" and row_split != split:
            continue
        row_is_ref = is_reference_row(row)
        if row_is_ref and not include_reference_identity:
            continue
        ref = refs.get(content)
        if ref is None:
            continue
        width = int(ref.get("width", 0) or ref.get("chug_width_manifest", 0) or 0)
        height = int(ref.get("height", 0) or ref.get("chug_height_manifest", 0) or 0)
        if width <= 0 or height <= 0:
            continue
        pairs.append(
            FeaturePair(
                row=row,
                ref_row=ref,
                dis_path=_row_clip_path(row, clips_dir),
                ref_path=_row_clip_path(ref, clips_dir),
                width=width,
                height=height,
                split=row_split,
                split_key=content,
            )
        )
    return pairs


def _decode_to_yuv10(
    *,
    src: Path,
    out: Path,
    width: int,
    height: int,
    ffmpeg_bin: str,
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    scale_filter = f"scale={width}:{height}:flags=bicubic,format=yuv420p10le"
    cmd = [
        ffmpeg_bin,
        "-y",
        "-loglevel",
        "error",
        "-i",
        str(src),
        "-vf",
        scale_filter,
        "-pix_fmt",
        "yuv420p10le",
        "-f",
        "rawvideo",
        str(out),
    ]
    runner(cmd, check=True)


def _empty_visual_signals() -> dict[str, float | None]:
    return {field: None for field in VISUAL_SIGNAL_FIELDS}


def _read_yuv420p10le_luma_frame(
    handle,
    *,
    frame_index: int,
    width: int,
    height: int,
    frame_bytes: int,
) -> np.ndarray | None:
    plane_bytes = width * height * 2
    handle.seek(frame_index * frame_bytes)
    raw = handle.read(plane_bytes)
    if len(raw) != plane_bytes:
        return None
    luma = np.frombuffer(raw, dtype="<u2").astype(np.float32).reshape(height, width)
    denom = 1023.0 if float(np.nanmax(luma)) <= 1023.0 else 65535.0
    return luma / denom


def _frame_visual_signals(luma: np.ndarray) -> dict[str, float]:
    if luma.shape[0] < 3 or luma.shape[1] < 3:
        return {
            "luma_std": float(np.std(luma)),
            "sharpness_laplacian_var": 0.0,
            "highfreq_abs_mean": 0.0,
            "noise_lap_mad": 0.0,
        }
    center = luma[1:-1, 1:-1]
    lap = luma[:-2, 1:-1] + luma[2:, 1:-1] + luma[1:-1, :-2] + luma[1:-1, 2:] - (4.0 * center)
    dx = np.abs(np.diff(luma, axis=1))
    dy = np.abs(np.diff(luma, axis=0))
    lap_median = float(np.median(lap))
    return {
        "luma_std": float(np.std(luma)),
        "sharpness_laplacian_var": float(np.var(lap)),
        "highfreq_abs_mean": float((float(np.mean(dx)) + float(np.mean(dy))) * 0.5),
        "noise_lap_mad": float(1.4826 * np.median(np.abs(lap - lap_median))),
    }


def compute_visual_signals_from_yuv10(
    yuv_path: Path,
    *,
    width: int,
    height: int,
    max_frames: int = 8,
) -> dict[str, float | None]:
    """Compute cheap decoded-luma noise/grain/blur proxies from yuv420p10le."""
    if width <= 0 or height <= 0 or not yuv_path.is_file():
        return _empty_visual_signals()
    frame_bytes = (width * height + 2 * ((width // 2) * (height // 2))) * 2
    if frame_bytes <= 0:
        return _empty_visual_signals()
    n_frames = yuv_path.stat().st_size // frame_bytes
    if n_frames <= 0:
        return _empty_visual_signals()
    sample_count = min(max_frames, n_frames)
    indices = np.linspace(0, n_frames - 1, num=sample_count, dtype=np.int64)
    frame_stats: list[dict[str, float]] = []
    with yuv_path.open("rb") as handle:
        for index in indices:
            luma = _read_yuv420p10le_luma_frame(
                handle,
                frame_index=int(index),
                width=width,
                height=height,
                frame_bytes=frame_bytes,
            )
            if luma is not None:
                frame_stats.append(_frame_visual_signals(luma))
    if not frame_stats:
        return _empty_visual_signals()
    return {
        field: float(np.mean([stats[field] for stats in frame_stats]))
        for field in VISUAL_SIGNAL_FIELDS
    }


def _prefixed_visual_signals(
    prefix: str,
    signals: dict[str, float | None] | None,
) -> dict[str, float | None]:
    values = signals or _empty_visual_signals()
    return {f"{prefix}_{field}": values.get(field) for field in VISUAL_SIGNAL_FIELDS}


def _visual_signal_deltas(
    ref_signals: dict[str, float | None] | None,
    dis_signals: dict[str, float | None] | None,
) -> dict[str, float | None]:
    ref_values = ref_signals or _empty_visual_signals()
    dis_values = dis_signals or _empty_visual_signals()
    deltas: dict[str, float | None] = {}
    for field in VISUAL_SIGNAL_FIELDS:
        ref = ref_values.get(field)
        dis = dis_values.get(field)
        deltas[f"feature_delta_{field}"] = (
            float(dis) - float(ref) if ref is not None and dis is not None else None
        )
    return deltas


def _cache_key(pair: FeaturePair, feature_set: str) -> str:
    dis_sha = str(pair.row.get("src_sha256", pair.dis_path.stem))
    ref_sha = str(pair.ref_row.get("src_sha256", pair.ref_path.stem))
    return f"{feature_set}-{ref_sha[:16]}-{dis_sha[:16]}-{pair.width}x{pair.height}"


def _read_done_keys(output: Path) -> set[str]:
    if not output.is_file():
        return set()
    done: set[str] = set()
    for row in _load_jsonl(output):
        key = row.get("feature_pair_key")
        if isinstance(key, str) and key:
            done.add(key)
    return done


def _extract_pair_payload(
    pair: FeaturePair,
    *,
    feature_names: tuple[str, ...],
    feature_set: str,
    cache_dir: Path,
    ffmpeg_bin: str,
    vmaf_bin: Path,
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    extractor: Callable[..., FeatureExtractionResult] = extract_features,
) -> ExtractedPairPayload:
    cache_path = cache_dir / f"{_cache_key(pair, feature_set)}.json"
    visual_cache_path = cache_dir / f"{_cache_key(pair, feature_set)}.visual.json"
    result: FeatureExtractionResult | None = None
    if cache_path.is_file():
        result = FeatureExtractionResult.from_jsonable(json.loads(cache_path.read_text()))
    if result is not None and visual_cache_path.is_file():
        visual_payload = json.loads(visual_cache_path.read_text())
        return ExtractedPairPayload(
            result=result,
            ref_visual_signals=visual_payload.get("ref") or _empty_visual_signals(),
            dis_visual_signals=visual_payload.get("dis") or _empty_visual_signals(),
        )

    cache_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="chug-features-") as tmp_s:
        tmp = Path(tmp_s)
        ref_yuv = tmp / "ref.yuv"
        dis_yuv = tmp / "dis.yuv"
        _decode_to_yuv10(
            src=pair.ref_path,
            out=ref_yuv,
            width=pair.width,
            height=pair.height,
            ffmpeg_bin=ffmpeg_bin,
            runner=runner,
        )
        _decode_to_yuv10(
            src=pair.dis_path,
            out=dis_yuv,
            width=pair.width,
            height=pair.height,
            ffmpeg_bin=ffmpeg_bin,
            runner=runner,
        )
        if result is None:
            result = extractor(
                ref_yuv,
                dis_yuv,
                pair.width,
                pair.height,
                features=feature_names,
                vmaf_binary=vmaf_bin,
                pix_fmt="420",
                bitdepth=10,
            )
            write_text_atomic(
                cache_path,
                json.dumps(result.to_jsonable(), sort_keys=True),
            )
        visual_payload = {
            "ref": compute_visual_signals_from_yuv10(
                ref_yuv,
                width=pair.width,
                height=pair.height,
            ),
            "dis": compute_visual_signals_from_yuv10(
                dis_yuv,
                width=pair.width,
                height=pair.height,
            ),
        }
        write_text_atomic(
            visual_cache_path,
            json.dumps(visual_payload, sort_keys=True),
        )

    return ExtractedPairPayload(
        result=result,
        ref_visual_signals=visual_payload["ref"],
        dis_visual_signals=visual_payload["dis"],
    )


def _finite_or_none(value: float) -> float | None:
    return float(value) if math.isfinite(float(value)) else None


def _build_output_row(
    pair: FeaturePair,
    result: FeatureExtractionResult,
    *,
    feature_set: str,
    ref_hdr_metadata: dict[str, Any] | None = None,
    dis_hdr_metadata: dict[str, Any] | None = None,
    ref_visual_signals: dict[str, float | None] | None = None,
    dis_visual_signals: dict[str, float | None] | None = None,
) -> dict[str, Any]:
    stats = aggregate_clip_stats(result.per_frame)
    n = len(result.feature_names)
    row = dict(pair.row)
    row.update(
        {
            "feature_pair_key": _cache_key(pair, feature_set),
            "feature_source": "chug-fr-ref-aligned",
            "feature_set": feature_set,
            "feature_names": list(result.feature_names),
            "feature_width": pair.width,
            "feature_height": pair.height,
            "feature_bitdepth": 10,
            "feature_alignment": "distorted_scaled_to_reference",
            "feature_ref_src": pair.ref_row.get("src", ""),
            "feature_ref_sha256": pair.ref_row.get("src_sha256", ""),
            "chug_reference_video_id": pair.ref_row.get("chug_video_id", ""),
            "split": pair.split,
            "chug_split_key": pair.split_key,
            "chug_split_policy": "content-name-blake2s-80-10-10",
            "n_feature_frames": int(result.n_frames),
        }
    )
    row.update(_prefixed_hdr_metadata("feature_ref", ref_hdr_metadata))
    row.update(_prefixed_hdr_metadata("feature_dis", dis_hdr_metadata))
    row.update(_prefixed_visual_signals("feature_ref", ref_visual_signals))
    row.update(_prefixed_visual_signals("feature_dis", dis_visual_signals))
    row.update(_visual_signal_deltas(ref_visual_signals, dis_visual_signals))
    stat_names = ("mean", "p10", "p90", "std")
    for stat_idx, stat_name in enumerate(stat_names):
        offset = stat_idx * n
        for feat_idx, feat_name in enumerate(result.feature_names):
            value = _finite_or_none(float(stats[offset + feat_idx]))
            row[f"{feat_name}_{stat_name}"] = value
            if stat_name == "mean":
                row[feat_name] = value
    return row


def run(
    *,
    input_jsonl: Path,
    output_jsonl: Path,
    clips_dir: Path,
    cache_dir: Path,
    feature_set: str = DEFAULT_FEATURE_SET,
    max_rows: int | None = None,
    split: str = "all",
    split_seed: str = DEFAULT_SPLIT_SEED,
    split_manifest: Path | None = None,
    audit_output: Path | None = None,
    include_reference_identity: bool = False,
    ffmpeg_bin: str = "ffmpeg",
    ffprobe_bin: str = "ffprobe",
    vmaf_bin: Path = DEFAULT_VMAF_BINARY,
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    extractor: Callable[..., FeatureExtractionResult] = extract_features,
    run_provenance: dict[str, Any] | None = None,
) -> int:
    """Materialise CHUG feature rows and return the number written."""
    if split not in ("all", *SPLIT_NAMES):
        raise ValueError(f"unknown split {split!r}; valid: all, {', '.join(SPLIT_NAMES)}")
    try:
        feature_names = FEATURE_SETS[feature_set]
    except KeyError as exc:
        raise ValueError(
            f"unknown feature set {feature_set!r}; valid: {sorted(FEATURE_SETS)}"
        ) from exc

    rows = _load_jsonl(input_jsonl)
    split_map = build_content_split_map(rows, seed=split_seed)
    if split_manifest is not None:
        write_split_manifest(
            rows,
            output=split_manifest,
            seed=split_seed,
            run_provenance=run_provenance,
        )
    if audit_output is not None:
        audit_chug_hdr_metadata(
            rows,
            clips_dir=clips_dir,
            output=audit_output,
            split_seed=split_seed,
            ffprobe_bin=ffprobe_bin,
            runner=runner,
            run_provenance=run_provenance,
        )
    pairs = build_feature_pairs(
        rows,
        clips_dir=clips_dir,
        split_map=split_map,
        split=split,
        include_reference_identity=include_reference_identity,
    )
    if max_rows is not None:
        pairs = pairs[:max_rows]

    output_jsonl.parent.mkdir(parents=True, exist_ok=True)
    done = _read_done_keys(output_jsonl)
    hdr_cache: dict[Path, dict[str, Any]] = {}

    def hdr_for(path: Path) -> dict[str, Any]:
        resolved = path.resolve()
        if resolved not in hdr_cache:
            hdr_cache[resolved] = probe_clip_hdr_metadata(
                path,
                ffprobe_bin=ffprobe_bin,
                runner=runner,
            )
        return hdr_cache[resolved]

    written = 0
    with output_jsonl.open("a", encoding="utf-8") as out:
        for idx, pair in enumerate(pairs, start=1):
            pair_key = _cache_key(pair, feature_set)
            if pair_key in done:
                continue
            if not pair.ref_path.is_file() or not pair.dis_path.is_file():
                continue
            # Per-clip isolation: one bad pair (ffmpeg/vmaf failure, malformed
            # cached JSON, read error) must be logged and skipped, not abort the
            # whole batch (R3-19).  The done-set/cache resume lets a re-run
            # continue, but the active batch should keep going on its own.
            try:
                payload = _extract_pair_payload(
                    pair,
                    feature_names=feature_names,
                    feature_set=feature_set,
                    cache_dir=cache_dir,
                    ffmpeg_bin=ffmpeg_bin,
                    vmaf_bin=vmaf_bin,
                    runner=runner,
                    extractor=extractor,
                )
                out.write(
                    json.dumps(
                        _build_output_row(
                            pair,
                            payload.result,
                            feature_set=feature_set,
                            ref_hdr_metadata=hdr_for(pair.ref_path),
                            dis_hdr_metadata=hdr_for(pair.dis_path),
                            ref_visual_signals=payload.ref_visual_signals,
                            dis_visual_signals=payload.dis_visual_signals,
                        )
                    )
                    + "\n"
                )
                out.flush()
            except Exception as exc:
                print(f"[chug-features] FAIL {pair_key}: {exc}", file=sys.stderr, flush=True)
                continue
            done.add(pair_key)
            written += 1
            if idx % 100 == 0:
                print(f"[chug-features] {idx}/{len(pairs)} rows considered; wrote {written}")
    return written


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(
        prog="chug_extract_features.py",
        description=__doc__,
    )
    ap.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    ap.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    ap.add_argument("--clips-dir", type=Path, default=DEFAULT_CLIPS_DIR)
    ap.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    ap.add_argument("--feature-set", choices=sorted(FEATURE_SETS), default=DEFAULT_FEATURE_SET)
    ap.add_argument("--split", choices=("all", *SPLIT_NAMES), default="all")
    ap.add_argument("--split-seed", default=DEFAULT_SPLIT_SEED)
    ap.add_argument("--split-manifest", type=Path, default=None)
    ap.add_argument(
        "--audit-output",
        type=Path,
        default=None,
        help="write a local ffprobe HDR metadata audit JSON before extraction",
    )
    ap.add_argument("--max-rows", type=int, default=100)
    ap.add_argument("--full", action="store_true", help="Process every available pair.")
    ap.add_argument(
        "--include-reference-identity",
        action="store_true",
        help="Also emit ref==distorted identity rows for CHUG reference clips.",
    )
    ap.add_argument("--ffmpeg-bin", default="ffmpeg")
    ap.add_argument("--ffprobe-bin", default="ffprobe")
    ap.add_argument("--vmaf-bin", type=Path, default=DEFAULT_VMAF_BINARY)
    args = ap.parse_args(raw_argv)
    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={
            "input_jsonl": args.input,
            "clips_dir": args.clips_dir,
            "cache_dir": args.cache_dir,
            "vmaf_bin": args.vmaf_bin,
        },
        outputs={
            "feature_jsonl": args.output,
            "split_manifest": args.split_manifest,
            "audit": args.audit_output,
        },
    )

    with contextlib.suppress(KeyboardInterrupt):
        written = run(
            input_jsonl=args.input,
            output_jsonl=args.output,
            clips_dir=args.clips_dir,
            cache_dir=args.cache_dir,
            feature_set=args.feature_set,
            max_rows=None if args.full else args.max_rows,
            split=args.split,
            split_seed=args.split_seed,
            split_manifest=args.split_manifest,
            audit_output=args.audit_output,
            include_reference_identity=args.include_reference_identity,
            ffmpeg_bin=args.ffmpeg_bin,
            ffprobe_bin=args.ffprobe_bin,
            vmaf_bin=args.vmaf_bin,
            run_provenance=run_provenance,
        )
        print(f"[chug-features] wrote {written} rows to {args.output}")
        return 0
    return 130


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
