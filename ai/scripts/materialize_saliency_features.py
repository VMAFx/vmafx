#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Append saliency aggregate columns to feature-table rows.

The signal-mix audit needs saliency / ROI columns in refreshed feature
tables before predictor and MOS-head retrains can measure whether the
signal helps. This script is deliberately table-shaped: it reads JSONL
or parquet rows, resolves a source clip per row, decodes a bounded
sample to temporary yuv420p via ffmpeg, runs the fork saliency helper,
and writes a new table with `saliency_mean` / `saliency_var`.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(
    __file__,
    include_repo_root=True,
    include_vmaf_tune_src=True,
)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

SubprocessRunner = Callable[..., subprocess.CompletedProcess[str]]
SaliencyFn = Callable[..., Any]


@dataclass(frozen=True)
class SaliencyMaterializeConfig:
    """Config for one table enrichment run."""

    path_column: str = "src"
    width_column: str = "width"
    height_column: str = "height"
    root: Path | None = None
    ffmpeg_bin: str = "ffmpeg"
    ffprobe_bin: str = "ffprobe"
    model_path: Path | None = None
    model_id: str | None = None
    max_frames: int = 8
    frame_samples: int = 8
    temporal_aggregator: str = "mean"
    ema_alpha: float = 0.6
    overwrite: bool = False
    status_column: str = "saliency_status"
    model_id_column: str = "saliency_model_id"
    aggregator_column: str = "saliency_aggregator"
    ema_alpha_column: str = "saliency_ema_alpha"
    # Fallback dimensions used when the row has no width/height columns and
    # ffprobe cannot determine them (e.g. raw YUV files without a container).
    # The Netflix corpus ships all distorted YUVs as 1920x1080 regardless of
    # the encode-ladder height in the filename; set default_width=1920 and
    # default_height=1080 in the manifest to skip per-row ffprobe probing.
    default_width: int = 0
    default_height: int = 0


@dataclass(frozen=True)
class MaterializeSummary:
    """Row counters from a materialisation run."""

    total: int
    ok: int
    skipped_existing: int
    failed: int


def read_table(path: Path) -> list[dict[str, Any]]:
    """Read a JSONL or parquet table into row dicts."""
    suffix = path.suffix.lower()
    if suffix == ".jsonl":
        rows: list[dict[str, Any]] = []
        with path.open("r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.strip()
                if line:
                    rows.append(json.loads(line))
        return rows
    if suffix == ".parquet":
        try:
            import pandas as pd
        except ImportError as exc:  # pragma: no cover - optional dependency
            raise RuntimeError("pandas is required to read parquet tables") from exc
        return list(pd.read_parquet(path).to_dict(orient="records"))
    raise ValueError(f"unsupported input extension {path.suffix!r}; expected .jsonl or .parquet")


def write_table(path: Path, rows: Iterable[dict[str, Any]]) -> None:
    """Write rows to JSONL or parquet based on ``path`` suffix."""
    suffix = path.suffix.lower()
    path.parent.mkdir(parents=True, exist_ok=True)
    rows_list = list(rows)
    if suffix == ".jsonl":
        with path.open("w", encoding="utf-8") as fh:
            for row in rows_list:
                fh.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
        return
    if suffix == ".parquet":
        try:
            import pandas as pd
        except ImportError as exc:  # pragma: no cover - optional dependency
            raise RuntimeError("pandas is required to write parquet tables") from exc
        pd.DataFrame.from_records(rows_list).to_parquet(path, index=False)
        return
    raise ValueError(f"unsupported output extension {path.suffix!r}; expected .jsonl or .parquet")


def materialize_rows(
    rows: Iterable[dict[str, Any]],
    cfg: SaliencyMaterializeConfig,
    *,
    runner: SubprocessRunner = subprocess.run,
    saliency_fn: SaliencyFn | None = None,
) -> tuple[list[dict[str, Any]], MaterializeSummary]:
    """Return rows enriched with saliency aggregates plus a summary.

    When multiple rows reference the same source file (common in per-frame
    feature tables where each row represents one frame of a clip), saliency
    is computed once per unique resolved path and cached for the duration of
    the call. This avoids redundant decoding when a table has many rows per
    clip (e.g. the Netflix refresh parquet has ~160 per-frame rows per clip).
    """
    out: list[dict[str, Any]] = []
    ok = 0
    skipped = 0
    failed = 0
    missing_source_count = 0
    missing_col_warned = False
    # In-process cache: resolved path string -> (mean, var) | None
    # None means a previous attempt failed; we propagate the failure for all
    # rows sharing the same source rather than retrying on every row.
    _saliency_cache: dict[str, tuple[float, float] | None] = {}
    for row in rows:
        enriched = dict(row)
        if _has_existing_saliency(enriched) and not cfg.overwrite:
            skipped += 1
            _set_status(enriched, cfg, "skipped-existing")
            out.append(enriched)
            continue
        # Emit a one-time warning when the configured path column is absent
        # from the first row — this almost always means a misconfigured
        # path_column in the manifest (e.g. "src" when the table uses
        # "dis_basename").
        if not missing_col_warned and cfg.path_column not in enriched:
            available = sorted(enriched.keys())
            print(
                f"saliency materialize: WARNING — path_column={cfg.path_column!r} not found in "
                f"row; available columns: {available}. "
                f"Set path_column to the correct column name in your manifest.",
                file=sys.stderr,
            )
            missing_col_warned = True
        # Resolve the source path once to build a cache key.
        source = _resolve_source(enriched, cfg)
        cache_key = str(source) if source is not None else ""
        if cache_key and cache_key in _saliency_cache:
            cached = _saliency_cache[cache_key]
            if cached is None:
                # Previous attempt for this file failed; propagate the same
                # failure status without re-running the expensive decode.
                failed += 1
                out.append(enriched)
                continue
            mean, var = cached
            enriched["saliency_mean"] = mean
            enriched["saliency_var"] = var
            _set_output_metadata(enriched, cfg)
            _set_status(enriched, cfg, "ok")
            ok += 1
            out.append(enriched)
            continue
        result = compute_row_saliency(enriched, cfg, runner=runner, saliency_fn=saliency_fn)
        if cache_key:
            _saliency_cache[cache_key] = result
        if result is None:
            failed += 1
            if enriched.get(cfg.status_column) == "missing-source":
                missing_source_count += 1
            out.append(enriched)
            continue
        mean, var = result
        enriched["saliency_mean"] = mean
        enriched["saliency_var"] = var
        _set_output_metadata(enriched, cfg)
        _set_status(enriched, cfg, "ok")
        ok += 1
        out.append(enriched)
    # Emit a single summary warning when all failures are missing-source —
    # this is the fingerprint of a wrong path_column or missing root config.
    if failed > 0 and missing_source_count == failed:
        print(
            f"saliency materialize: WARNING — all {failed} failures have status "
            f"'missing-source'. Check that path_column={cfg.path_column!r} names a "
            f"column containing valid file paths and that root={cfg.root!r} is set "
            f"correctly for relative paths.",
            file=sys.stderr,
        )
    return out, MaterializeSummary(total=len(out), ok=ok, skipped_existing=skipped, failed=failed)


def compute_row_saliency(
    row: dict[str, Any],
    cfg: SaliencyMaterializeConfig,
    *,
    runner: SubprocessRunner = subprocess.run,
    saliency_fn: SaliencyFn | None = None,
) -> tuple[float, float] | None:
    """Compute `(saliency_mean, saliency_var)` for one row."""
    source = _resolve_source(row, cfg)
    if source is None:
        _set_status(row, cfg, "missing-source")
        return None
    width, height = _row_geometry(row, cfg)
    if width <= 0 or height <= 0:
        probed = _probe_geometry(source, cfg, runner)
        if probed is not None:
            width, height = probed
    if width <= 0 or height <= 0:
        _set_status(row, cfg, "missing-geometry")
        return None
    frames = max(1, int(cfg.max_frames))
    with tempfile.TemporaryDirectory(prefix="vmaf-saliency-features-") as tmp:
        raw_path = Path(tmp) / "clip.yuv"
        # Raw YUV files (.yuv) have no container and need explicit format
        # flags before the -i argument so ffmpeg can decode them correctly.
        extra_input_flags: list[str] = []
        if source.suffix.lower() == ".yuv":
            extra_input_flags = [
                "-f",
                "rawvideo",
                "-video_size",
                f"{width}x{height}",
                "-pix_fmt",
                "yuv420p",
            ]
        cmd = [
            cfg.ffmpeg_bin,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            *extra_input_flags,
            "-i",
            str(source),
            "-frames:v",
            str(frames),
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(raw_path),
        ]
        proc = runner(cmd, capture_output=True, text=True, check=False)
        if int(getattr(proc, "returncode", 1)) != 0 or not raw_path.is_file():
            _set_status(row, cfg, "decode-failed")
            return None
        try:
            fn = saliency_fn or _default_saliency_fn()
            frame_samples = max(1, min(int(cfg.frame_samples), frames))
            mask = fn(
                raw_path,
                width,
                height,
                model_path=cfg.model_path,
                frame_samples=frame_samples,
                temporal_aggregator=cfg.temporal_aggregator,
                ema_alpha=cfg.ema_alpha,
            )
            return _mean_var(mask)
        except Exception:  # pragma: no cover - saliency stack is optional
            _set_status(row, cfg, "model-failed")
            return None


def _default_saliency_fn() -> SaliencyFn:
    from vmaftune.saliency import compute_saliency_map

    return compute_saliency_map


def _has_existing_saliency(row: dict[str, Any]) -> bool:
    return (
        _finite_float(row.get("saliency_mean")) is not None
        and _finite_float(row.get("saliency_var")) is not None
    )


def _set_status(row: dict[str, Any], cfg: SaliencyMaterializeConfig, status: str) -> None:
    if cfg.status_column:
        row[cfg.status_column] = status


def _set_output_metadata(row: dict[str, Any], cfg: SaliencyMaterializeConfig) -> None:
    if cfg.model_id_column:
        row[cfg.model_id_column] = _effective_model_id(cfg)
    if cfg.aggregator_column:
        row[cfg.aggregator_column] = cfg.temporal_aggregator
    if cfg.ema_alpha_column:
        row[cfg.ema_alpha_column] = float(cfg.ema_alpha)


def _effective_model_id(cfg: SaliencyMaterializeConfig) -> str:
    if cfg.model_id:
        return cfg.model_id
    if cfg.model_path is None:
        return "saliency_student_v1"
    return cfg.model_path.stem


def _resolve_source(row: dict[str, Any], cfg: SaliencyMaterializeConfig) -> Path | None:
    value = row.get(cfg.path_column)
    if value is None or value == "":
        return None
    path = Path(str(value))
    if not path.is_absolute() and cfg.root is not None:
        path = cfg.root / path
    return path if path.is_file() else None


def _row_geometry(row: dict[str, Any], cfg: SaliencyMaterializeConfig) -> tuple[int, int]:
    width = _finite_float(row.get(cfg.width_column))
    height = _finite_float(row.get(cfg.height_column))
    w = int(width or 0)
    h = int(height or 0)
    # Fall back to the config-level defaults when the row has no geometry columns
    # (e.g. raw-YUV corpora where width/height were not written to the feature table).
    if w <= 0 and cfg.default_width > 0:
        w = cfg.default_width
    if h <= 0 and cfg.default_height > 0:
        h = cfg.default_height
    return (w, h)


def _probe_geometry(
    source: Path, cfg: SaliencyMaterializeConfig, runner: SubprocessRunner
) -> tuple[int, int] | None:
    cmd = [
        cfg.ffprobe_bin,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height",
        "-of",
        "json",
        str(source),
    ]
    proc = runner(cmd, capture_output=True, text=True, check=False)
    if int(getattr(proc, "returncode", 1)) != 0:
        return None
    try:
        payload = json.loads(getattr(proc, "stdout", "") or "{}")
    except json.JSONDecodeError:
        return None
    streams = payload.get("streams", [])
    if not streams:
        return None
    first = streams[0]
    width = _finite_float(first.get("width"))
    height = _finite_float(first.get("height"))
    if width is None or height is None:
        return None
    return (int(width), int(height))


def _finite_float(value: Any) -> float | None:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return None
    return out if math.isfinite(out) else None


def _mean_var(mask: Any) -> tuple[float, float]:
    import numpy as np

    arr = np.asarray(mask, dtype=np.float32)
    if arr.size == 0:
        raise ValueError("empty saliency mask")
    return (float(arr.mean()), float(arr.var()))


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = make_argument_parser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Input .jsonl or .parquet table")
    parser.add_argument(
        "--output", type=Path, required=True, help="Output .jsonl or .parquet table"
    )
    parser.add_argument("--audit-json", type=Path, default=None, help="Optional audit JSON output")
    parser.add_argument(
        "--path-column", default="src", help="Row column containing the source path"
    )
    parser.add_argument("--width-column", default="width", help="Row column containing width")
    parser.add_argument("--height-column", default="height", help="Row column containing height")
    parser.add_argument("--root", type=Path, default=None, help="Root for relative source paths")
    parser.add_argument("--ffmpeg-bin", default="ffmpeg")
    parser.add_argument("--ffprobe-bin", default="ffprobe")
    parser.add_argument("--model-path", type=Path, default=None)
    parser.add_argument(
        "--model-id",
        default=None,
        help=(
            "Model identifier recorded in output metadata. Defaults to "
            "saliency_student_v1 for the bundled model or the model-path stem."
        ),
    )
    parser.add_argument("--max-frames", type=int, default=8)
    parser.add_argument("--frame-samples", type=int, default=8)
    parser.add_argument(
        "--temporal-aggregator",
        default="mean",
        choices=("mean", "ema", "max", "motion-weighted"),
        help="How sampled per-frame saliency maps are reduced.",
    )
    parser.add_argument(
        "--ema-alpha",
        type=float,
        default=0.6,
        help="Current-frame weight when --temporal-aggregator=ema.",
    )
    parser.add_argument(
        "--overwrite", action="store_true", help="Recompute existing saliency columns"
    )
    parser.add_argument(
        "--status-column",
        default="saliency_status",
        help="Status column name; pass an empty string to omit it",
    )
    parser.add_argument(
        "--model-id-column",
        default="saliency_model_id",
        help="Output metadata column for the model identifier; pass empty to omit.",
    )
    parser.add_argument(
        "--aggregator-column",
        default="saliency_aggregator",
        help="Output metadata column for the temporal reducer; pass empty to omit.",
    )
    parser.add_argument(
        "--ema-alpha-column",
        default="saliency_ema_alpha",
        help="Output metadata column for EMA alpha; pass empty to omit.",
    )
    parser.add_argument(
        "--default-width",
        type=int,
        default=0,
        help=(
            "Fallback frame width used when the row has no width column and ffprobe "
            "cannot determine it (e.g. raw YUV corpora). 0 = no fallback."
        ),
    )
    parser.add_argument(
        "--default-height",
        type=int,
        default=0,
        help=(
            "Fallback frame height used when the row has no height column and ffprobe "
            "cannot determine it (e.g. raw YUV corpora). 0 = no fallback."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _parse_args(raw_argv)
    cfg = SaliencyMaterializeConfig(
        path_column=args.path_column,
        width_column=args.width_column,
        height_column=args.height_column,
        root=args.root,
        ffmpeg_bin=args.ffmpeg_bin,
        ffprobe_bin=args.ffprobe_bin,
        model_path=args.model_path,
        model_id=args.model_id,
        max_frames=args.max_frames,
        frame_samples=args.frame_samples,
        temporal_aggregator=args.temporal_aggregator,
        ema_alpha=args.ema_alpha,
        overwrite=args.overwrite,
        status_column=args.status_column,
        model_id_column=args.model_id_column,
        aggregator_column=args.aggregator_column,
        ema_alpha_column=args.ema_alpha_column,
        default_width=args.default_width,
        default_height=args.default_height,
    )
    rows = read_table(args.input)
    enriched, summary = materialize_rows(rows, cfg)
    write_table(args.output, enriched)
    if args.audit_json is not None:
        config = asdict(cfg)
        for key in ("root", "model_path"):
            if config[key] is not None:
                config[key] = str(config[key])
        write_manifest_json(
            args.audit_json,
            {
                "input": str(args.input),
                "output": str(args.output),
                "config": config,
                "summary": asdict(summary),
                "run_provenance": build_run_provenance(
                    entrypoint=SCRIPT_PATH,
                    repo_root=REPO_ROOT,
                    argv=raw_argv,
                    args=args,
                    inputs={
                        "input": args.input,
                        "root": args.root,
                        "model_path": args.model_path,
                    },
                    outputs={
                        "output": str(args.output),
                        "audit_json": str(args.audit_json),
                    },
                ),
            },
        )
    print(
        "saliency materialize: "
        f"total={summary.total} ok={summary.ok} "
        f"skipped={summary.skipped_existing} failed={summary.failed}",
        file=sys.stderr,
    )
    return 0 if summary.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
