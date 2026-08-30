#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Waterloo IVC 4K-VQA -> MOS-corpus JSONL adapter (ADR-0369).

Waterloo IVC 4K-VQA (Li, Duanmu, Liu, Wang; ICIAR 2019) -- the University
of Waterloo IVC Lab's professionally-captured 2160p subjective-quality
corpus. 1 200 distorted clips with per-clip MOS on a 0-100 raw scale.

Shared infrastructure: :mod:`ai.src.corpus.base` (ADR-0371).

MOS scale: native 0-100 (verbatim). Downstream code must normalise
across corpora (see ADR-0369 Consequences).

The adapter auto-detects two manifest shapes:
1. Canonical headerless 5-tuple ``encoder, video_number, resolution,
   distortion_level, mos`` (the upstream ``scores.txt`` shape).
2. Standard adapter CSV (LSVQ / KonViD-150k header convention).

License: Waterloo IVC permissive academic license (attribution required).
Obtain the dataset from https://ivc.uwaterloo.ca/database/4KVQA.html and
drop ``scores.txt`` at ``<waterloo-ivc-dir>/manifest.csv``.
"""

from __future__ import annotations

import argparse
import csv
import logging
import os
import shutil
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from corpus import base as _corpus_base  # noqa: E402
from corpus.base import (  # noqa: E402
    CorpusIngestBase,
    RunStats,
    normalise_clip_name,
    pick,
    utc_now_iso,
    write_ingest_manifest,
)

save_progress = _corpus_base.save_progress

_LOG = logging.getLogger("waterloo_ivc_to_corpus_jsonl")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_WATERLOO_IVC_MIN_ROWS: int = 100
_WATERLOO_IVC_DEFAULT_MAX_ROWS: int = 100
# Default working directory; override with ``VMAF_WATERLOO_IVC_DIR`` env
# var for container / non-maintainer layouts.
_DEFAULT_WATERLOO_IVC_DIR: Path = Path(
    os.environ.get(
        "VMAF_WATERLOO_IVC_DIR",
        str(REPO_ROOT / ".corpus" / "waterloo-ivc-4k"),
    )
)
_DEFAULT_OUTPUT: Path = _DEFAULT_WATERLOO_IVC_DIR / "waterloo_ivc_4k.jsonl"
_DEFAULT_CLIPS_SUBDIR: str = "clips"
_DEFAULT_MANIFEST_NAME: str = "manifest.csv"
_DEFAULT_CLIP_SUFFIX: str = ".yuv"
_WATERLOO_IVC_DOWNLOAD_BASE: str = "https://ivc.uwaterloo.ca/database/4KVQA/201908/"

_CSV_FILENAME_KEYS: tuple[str, ...] = ("name", "video_name", "filename", "file_name")
_CSV_URL_KEYS: tuple[str, ...] = ("url", "download_url", "video_url")
_CSV_MOS_KEYS: tuple[str, ...] = ("mos", "MOS", "mos_score")
_CSV_MOS_STD_KEYS: tuple[str, ...] = ("sd", "SD", "mos_std", "mos_std_dev", "SD_MOS")
_CSV_NRATINGS_KEYS: tuple[str, ...] = ("n", "ratings", "num_ratings", "n_ratings")

_DEFAULT_CORPUS_VERSION: str = "waterloo-ivc-4k-201908"
_CORPUS_LABEL: str = "waterloo-ivc-4k"


# ---------------------------------------------------------------------------
# Manifest CSV -- two shapes
# ---------------------------------------------------------------------------


def _is_canonical_headerless(first_line: str) -> bool:
    """Detect the canonical Waterloo IVC headerless 5-tuple shape."""
    parts = [p.strip() for p in first_line.split(",")]
    if len(parts) != 5:
        return False
    encoder = parts[0]
    if not encoder or any(c.isdigit() for c in encoder):
        return False
    try:
        mos_val = float(parts[4])
    except ValueError:
        return False
    return 0.0 <= mos_val <= 100.0


def _synthesise_canonical_filename(
    encoder: str,
    video_number: str,
    resolution: str,
    distortion: str,
    *,
    suffix: str = _DEFAULT_CLIP_SUFFIX,
) -> str:
    """Build the canonical Waterloo IVC clip filename from the 5-tuple fields."""
    return (
        f"{encoder.strip()}_{video_number.strip()}"
        f"_{resolution.strip()}_{distortion.strip()}{suffix}"
    )


def _parse_canonical_headerless(
    lines: list[str], *, clip_suffix: str, csv_path: Path
) -> list[dict[str, Any]]:
    parsed: list[dict[str, Any]] = []
    for line_no, line in enumerate(lines, start=1):
        parts = [p.strip() for p in line.split(",")]
        if len(parts) != 5:
            _LOG.warning(
                "%s:%d: expected 5 fields, got %d; skipping", csv_path, line_no, len(parts)
            )
            continue
        encoder, video_number, resolution, distortion, mos_str = parts
        try:
            mos = float(mos_str)
        except ValueError:
            _LOG.warning("%s:%d: bad MOS value %r; skipping", csv_path, line_no, mos_str)
            continue
        filename = _synthesise_canonical_filename(
            encoder, video_number, resolution, distortion, suffix=clip_suffix
        )
        parsed.append(
            {"filename": filename, "url": "", "mos": mos, "mos_std_dev": 0.0, "n_ratings": 0}
        )
    return parsed


def _parse_standard_csv(csv_path: Path, *, clip_suffix: str) -> list[dict[str, Any]]:
    with csv_path.open(newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            raise ValueError(f"{csv_path}: empty / headerless CSV")
        all_rows = list(reader)

    parsed: list[dict[str, Any]] = []
    for line_no, row in enumerate(all_rows, start=2):
        stem = pick(row, _CSV_FILENAME_KEYS)
        if not stem:
            _LOG.warning("%s:%d: no filename column; skipping", csv_path, line_no)
            continue
        filename = normalise_clip_name(stem, suffix=clip_suffix)
        url = pick(row, _CSV_URL_KEYS) or ""
        mos_str = pick(row, _CSV_MOS_KEYS)
        if mos_str is None:
            _LOG.warning("%s:%d: no MOS column for %s; skipping", csv_path, line_no, filename)
            continue
        try:
            mos = float(mos_str)
        except ValueError:
            _LOG.warning("%s:%d: bad MOS value %r; skipping", csv_path, line_no, mos_str)
            continue
        std_str = pick(row, _CSV_MOS_STD_KEYS)
        try:
            mos_std_dev = float(std_str) if std_str is not None else 0.0
        except ValueError:
            mos_std_dev = 0.0
        n_str = pick(row, _CSV_NRATINGS_KEYS)
        try:
            n_ratings = int(float(n_str)) if n_str is not None else 0
        except ValueError:
            n_ratings = 0
        parsed.append(
            {
                "filename": filename,
                "url": url,
                "mos": mos,
                "mos_std_dev": mos_std_dev,
                "n_ratings": n_ratings,
            }
        )
    return parsed


def parse_manifest_csv(
    csv_path: Path,
    *,
    min_rows: int = _WATERLOO_IVC_MIN_ROWS,
    clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
) -> list[dict[str, Any]]:
    """Parse a Waterloo IVC manifest; auto-detect canonical vs. standard shape."""
    if not csv_path.is_file():
        raise FileNotFoundError(f"Waterloo IVC manifest not found: {csv_path}")

    raw = csv_path.read_text(encoding="utf-8-sig")
    lines = [ln for ln in raw.splitlines() if ln.strip()]
    if not lines:
        raise ValueError(f"{csv_path}: empty manifest")

    if _is_canonical_headerless(lines[0]):
        rows = _parse_canonical_headerless(lines, clip_suffix=clip_suffix, csv_path=csv_path)
    else:
        rows = _parse_standard_csv(csv_path, clip_suffix=clip_suffix)

    if len(rows) < min_rows:
        raise ValueError(
            f"{csv_path}: {len(rows)} rows is below the Waterloo IVC sanity floor ({min_rows}). "
            f"The canonical scores.txt carries 1 200 rows. "
            f"Drop the file from {_WATERLOO_IVC_DOWNLOAD_BASE}scores.txt"
        )
    return rows


# ---------------------------------------------------------------------------
# Adapter subclass
# ---------------------------------------------------------------------------


class WaterlooIVCIngest(CorpusIngestBase):
    """MOS-corpus ingest adapter for Waterloo IVC 4K-VQA (ADR-0369)."""

    corpus_label = _CORPUS_LABEL

    def __init__(
        self,
        *,
        waterloo_ivc_dir: Path,
        min_csv_rows: int = _WATERLOO_IVC_MIN_ROWS,
        clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
        **kwargs: Any,
    ) -> None:
        super().__init__(corpus_dir=waterloo_ivc_dir, **kwargs)
        self._min_csv_rows = min_csv_rows
        self._clip_suffix = clip_suffix

    def iter_source_rows(self, clips_dir: Path) -> Iterator[tuple[Path, dict[str, Any]]]:
        rows = parse_manifest_csv(
            self.manifest_csv, min_rows=self._min_csv_rows, clip_suffix=self._clip_suffix
        )
        _LOG.info("parsed %d manifest rows from %s", len(rows), self.manifest_csv.name)
        for row in rows:
            yield clips_dir / row["filename"], row


# ---------------------------------------------------------------------------
# Module-level run()
# ---------------------------------------------------------------------------


def run(
    *,
    waterloo_ivc_dir: Path,
    output: Path,
    manifest_csv: Path | None = None,
    progress_path: Path | None = None,
    clips_subdir: str = _DEFAULT_CLIPS_SUBDIR,
    ffprobe_bin: str = "ffprobe",
    curl_bin: str = "curl",
    corpus_version: str = _DEFAULT_CORPUS_VERSION,
    runner=subprocess.run,
    now_fn=utc_now_iso,
    attrition_warn_threshold: float = 0.10,
    download_timeout_s: int = 120,
    min_csv_rows: int = _WATERLOO_IVC_MIN_ROWS,
    max_rows: int | None = _WATERLOO_IVC_DEFAULT_MAX_ROWS,
    clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
) -> RunStats:
    """Build the JSONL. Returns a :class:`RunStats`."""
    ingest = WaterlooIVCIngest(
        waterloo_ivc_dir=waterloo_ivc_dir,
        output=output,
        manifest_csv=manifest_csv,
        progress_path=progress_path,
        clips_subdir=clips_subdir,
        ffprobe_bin=ffprobe_bin,
        curl_bin=curl_bin,
        corpus_version=corpus_version,
        runner=runner,
        now_fn=now_fn,
        attrition_warn_threshold=attrition_warn_threshold,
        download_timeout_s=download_timeout_s,
        max_rows=max_rows,
        min_csv_rows=min_csv_rows,
        clip_suffix=clip_suffix,
    )
    return ingest.run()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    ap = make_argument_parser(
        prog="waterloo_ivc_to_corpus_jsonl.py",
        description=(
            "ADR-0369: walk a local Waterloo IVC 4K-VQA extraction (or build "
            "one via resumable downloads), probe each clip via ffprobe, join "
            "with the manifest CSV's MOS scores, and emit one JSONL row per clip."
        ),
    )
    ap.add_argument(
        "--waterloo-ivc-dir",
        type=Path,
        default=_DEFAULT_WATERLOO_IVC_DIR,
        help="Local Waterloo IVC working directory.",
    )
    ap.add_argument("--manifest-csv", type=Path, default=None)
    ap.add_argument("--progress-path", type=Path, default=None)
    ap.add_argument(
        "--clips-subdir",
        default=_DEFAULT_CLIPS_SUBDIR,
        help=f"Subdirectory for clips (default: {_DEFAULT_CLIPS_SUBDIR!r}).",
    )
    ap.add_argument(
        "--clip-suffix",
        default=_DEFAULT_CLIP_SUFFIX,
        help=f"Default extension (default: {_DEFAULT_CLIP_SUFFIX!r}).",
    )
    ap.add_argument("--output", type=Path, default=_DEFAULT_OUTPUT)
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Replay manifest JSON sidecar (default: <output>.manifest.json).",
    )
    ap.add_argument("--ffprobe-bin", default=os.environ.get("FFPROBE_BIN", "ffprobe"))
    ap.add_argument("--curl-bin", default=os.environ.get("CURL_BIN", "curl"))
    ap.add_argument(
        "--corpus-version",
        default=_DEFAULT_CORPUS_VERSION,
        help=f"Dataset version string (default: {_DEFAULT_CORPUS_VERSION!r}).",
    )
    ap.add_argument("--attrition-warn-threshold", type=float, default=0.10)
    ap.add_argument("--download-timeout-s", type=int, default=120)
    ap.add_argument(
        "--max-rows",
        type=int,
        default=_WATERLOO_IVC_DEFAULT_MAX_ROWS,
        help=f"Cap at this many rows (default: {_WATERLOO_IVC_DEFAULT_MAX_ROWS}).",
    )
    ap.add_argument("--full", action="store_true", help="Ingest entire manifest.")
    ap.add_argument("--log-level", default="INFO", choices=("DEBUG", "INFO", "WARNING", "ERROR"))
    return ap


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _build_parser().parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.output.with_suffix(".manifest.json")
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    if shutil.which(args.curl_bin) is None:
        _LOG.warning("curl binary %r not on PATH; downloads will fail", args.curl_bin)
    max_rows: int | None = None if args.full else args.max_rows
    try:
        stats = run(
            waterloo_ivc_dir=args.waterloo_ivc_dir,
            output=args.output,
            manifest_csv=args.manifest_csv,
            progress_path=args.progress_path,
            clips_subdir=args.clips_subdir,
            clip_suffix=args.clip_suffix,
            ffprobe_bin=args.ffprobe_bin,
            curl_bin=args.curl_bin,
            corpus_version=args.corpus_version,
            attrition_warn_threshold=args.attrition_warn_threshold,
            download_timeout_s=args.download_timeout_s,
            max_rows=max_rows,
        )
    except FileNotFoundError as exc:
        msg = str(exc)
        # Append acquisition hint when the corpus root is missing so
        # operators know where to get the data (test asserts on the hint).
        if "corpus directory not found" in msg.lower():
            msg += (
                "\n  Obtain the dataset from https://ivc.uwaterloo.ca/database/4KVQA.html"
                " and drop scores.txt at <waterloo-ivc-dir>/manifest.csv."
            )
        print(f"error: {msg}", file=sys.stderr)
        return 2
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    write_ingest_manifest(
        args.manifest_out,
        schema="waterloo-ivc-corpus-jsonl-manifest-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        corpus_label=_CORPUS_LABEL,
        stats=stats,
        inputs={
            "waterloo_ivc_dir": args.waterloo_ivc_dir,
            "manifest_csv": args.manifest_csv,
            "progress_path": args.progress_path,
        },
        outputs={"jsonl": args.output, "manifest": args.manifest_out},
        config={
            "clips_subdir": args.clips_subdir,
            "clip_suffix": args.clip_suffix,
            "min_csv_rows": _WATERLOO_IVC_MIN_ROWS,
            "max_rows": max_rows,
            "full": args.full,
            "corpus_version": args.corpus_version,
            "attrition_warn_threshold": args.attrition_warn_threshold,
        },
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
