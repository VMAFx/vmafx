#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""YouTube UGC -> MOS-corpus JSONL adapter (ADR-0368).

The Google YouTube UGC dataset (Wang, Inguva, Adsumilli; MMSP 2019) is the
field's canonical large-scale UGC video corpus. Hosted in the public-readable
GCS bucket ``gs://ugc-dataset/``.

Shared infrastructure: :mod:`ai.src.corpus.base` (ADR-0371).

Pipeline shape::

    .workingdir2/youtube-ugc/
      +-- .download-progress.json
      +-- manifest.csv
      +-- clips/
      +-- youtube-ugc.jsonl

When the manifest lacks a ``url`` column, the adapter synthesises the
canonical bucket URL from the filename.

License: YouTube UGC is CC-BY (per ADR-0368). This script does not ship
any clip, MOS value, or derived feature in tree; only the adapter and
schema land in the repo. The corpus is hosted in the public GCS bucket
``gs://ugc-dataset/`` (see ``original_videos.csv`` for the manifest).
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

_LOG = logging.getLogger("youtube_ugc_to_corpus_jsonl")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_UGC_MIN_ROWS: int = 200
_UGC_DEFAULT_MAX_ROWS: int = 300
# Default working directory; override with ``VMAF_YOUTUBE_UGC_DIR`` env
# var for container / non-maintainer layouts.
_DEFAULT_UGC_DIR: Path = Path(
    os.environ.get(
        "VMAF_YOUTUBE_UGC_DIR",
        str(REPO_ROOT / ".corpus" / "youtube-ugc"),
    )
)
_DEFAULT_OUTPUT: Path = _DEFAULT_UGC_DIR / "youtube-ugc.jsonl"
_DEFAULT_CLIPS_SUBDIR: str = "clips"
_DEFAULT_MANIFEST_NAME: str = "manifest.csv"
_DEFAULT_CLIP_SUFFIX: str = ".mp4"
_DEFAULT_BUCKET_PREFIX: str = "https://storage.googleapis.com/ugc-dataset/original_videos/"

_CSV_FILENAME_KEYS: tuple[str, ...] = ("vid", "name", "video_name", "filename", "file_name")
_CSV_URL_KEYS: tuple[str, ...] = ("url", "download_url", "video_url")
_CSV_MOS_KEYS: tuple[str, ...] = ("mos", "MOS", "mos_score", "dmos", "DMOS")
_CSV_MOS_STD_KEYS: tuple[str, ...] = ("sd", "SD", "mos_std", "mos_std_dev", "sd_mos", "SD_MOS")
_CSV_NRATINGS_KEYS: tuple[str, ...] = ("n", "ratings", "num_ratings", "n_ratings")

_DEFAULT_CORPUS_VERSION: str = "ugc-2019-orig"
_CORPUS_LABEL: str = "youtube-ugc"


# ---------------------------------------------------------------------------
# Manifest CSV
# ---------------------------------------------------------------------------


def _synth_bucket_url(filename: str, *, prefix: str = _DEFAULT_BUCKET_PREFIX) -> str:
    """Synthesise a canonical GCS bucket URL for ``filename``."""
    return f"{prefix}{filename}"


def parse_manifest_csv(
    csv_path: Path,
    *,
    min_rows: int = _UGC_MIN_ROWS,
    clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
    bucket_prefix: str = _DEFAULT_BUCKET_PREFIX,
) -> list[dict[str, Any]]:
    """Parse a YouTube UGC split CSV; synthesise URLs when absent."""
    if not csv_path.is_file():
        raise FileNotFoundError(f"YouTube UGC manifest CSV not found: {csv_path}")

    with csv_path.open(newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            raise ValueError(f"{csv_path}: empty / headerless CSV")
        all_rows = list(reader)

    if len(all_rows) < min_rows:
        raise ValueError(
            f"{csv_path}: {len(all_rows)} rows is below the YouTube UGC sanity floor ({min_rows})."
        )

    parsed: list[dict[str, Any]] = []
    for line_no, row in enumerate(all_rows, start=2):
        stem = pick(row, _CSV_FILENAME_KEYS)
        if not stem:
            _LOG.warning("%s:%d: no filename column; skipping", csv_path, line_no)
            continue
        filename = normalise_clip_name(stem, suffix=clip_suffix)
        url = pick(row, _CSV_URL_KEYS) or ""
        if not url:
            url = _synth_bucket_url(filename, prefix=bucket_prefix)
            _LOG.debug("%s:%d: synthesised bucket URL for %s", csv_path, line_no, filename)
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


# ---------------------------------------------------------------------------
# Adapter subclass
# ---------------------------------------------------------------------------


class YouTubeUGCIngest(CorpusIngestBase):
    """MOS-corpus ingest adapter for YouTube UGC (ADR-0368)."""

    corpus_label = _CORPUS_LABEL

    def __init__(
        self,
        *,
        ugc_dir: Path,
        min_csv_rows: int = _UGC_MIN_ROWS,
        clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
        bucket_prefix: str = _DEFAULT_BUCKET_PREFIX,
        **kwargs: Any,
    ) -> None:
        super().__init__(corpus_dir=ugc_dir, **kwargs)
        self._min_csv_rows = min_csv_rows
        self._clip_suffix = clip_suffix
        self._bucket_prefix = bucket_prefix

    def iter_source_rows(self, clips_dir: Path) -> Iterator[tuple[Path, dict[str, Any]]]:
        rows = parse_manifest_csv(
            self.manifest_csv,
            min_rows=self._min_csv_rows,
            clip_suffix=self._clip_suffix,
            bucket_prefix=self._bucket_prefix,
        )
        _LOG.info("parsed %d manifest rows from %s", len(rows), self.manifest_csv.name)
        for row in rows:
            yield clips_dir / row["filename"], row


# ---------------------------------------------------------------------------
# Module-level run()
# ---------------------------------------------------------------------------


def run(
    *,
    ugc_dir: Path,
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
    download_timeout_s: int = 300,
    min_csv_rows: int = _UGC_MIN_ROWS,
    max_rows: int | None = _UGC_DEFAULT_MAX_ROWS,
    clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
    bucket_prefix: str = _DEFAULT_BUCKET_PREFIX,
) -> RunStats:
    """Build the JSONL. Returns a :class:`RunStats`."""
    ingest = YouTubeUGCIngest(
        ugc_dir=ugc_dir,
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
        bucket_prefix=bucket_prefix,
    )
    return ingest.run()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    ap = make_argument_parser(
        prog="youtube_ugc_to_corpus_jsonl.py",
        description=(
            "ADR-0368: walk a local YouTube UGC extraction (or build one via "
            "resumable downloads from gs://ugc-dataset/), probe each clip via "
            "ffprobe, join with the manifest CSV's MOS scores, and emit one "
            "JSONL row per clip."
        ),
    )
    ap.add_argument(
        "--ugc-dir",
        type=Path,
        default=_DEFAULT_UGC_DIR,
        help="Local YouTube UGC working directory.",
    )
    ap.add_argument("--manifest-csv", type=Path, default=None)
    ap.add_argument("--progress-path", type=Path, default=None)
    ap.add_argument("--clips-subdir", default=_DEFAULT_CLIPS_SUBDIR)
    ap.add_argument(
        "--clip-suffix",
        default=_DEFAULT_CLIP_SUFFIX,
        help=f"Default extension (default: {_DEFAULT_CLIP_SUFFIX!r}).",
    )
    ap.add_argument(
        "--bucket-prefix",
        default=_DEFAULT_BUCKET_PREFIX,
        help="GCS bucket URL prefix for synthesising download URLs.",
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
    ap.add_argument(
        "--download-timeout-s",
        type=int,
        default=300,
        help="Per-clip curl --max-time seconds (default: 300; UGC clips are large).",
    )
    ap.add_argument(
        "--max-rows",
        type=int,
        default=_UGC_DEFAULT_MAX_ROWS,
        help=f"Cap at this many rows (default: {_UGC_DEFAULT_MAX_ROWS}).",
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
            ugc_dir=args.ugc_dir,
            output=args.output,
            manifest_csv=args.manifest_csv,
            progress_path=args.progress_path,
            clips_subdir=args.clips_subdir,
            clip_suffix=args.clip_suffix,
            bucket_prefix=args.bucket_prefix,
            ffprobe_bin=args.ffprobe_bin,
            curl_bin=args.curl_bin,
            corpus_version=args.corpus_version,
            attrition_warn_threshold=args.attrition_warn_threshold,
            download_timeout_s=args.download_timeout_s,
            max_rows=max_rows,
        )
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        print(
            "hint: obtain YouTube UGC via gs://ugc-dataset/ "
            "or use original_videos.csv from the dataset homepage",
            file=sys.stderr,
        )
        return 2
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    write_ingest_manifest(
        args.manifest_out,
        schema="youtube-ugc-corpus-jsonl-manifest-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        corpus_label=_CORPUS_LABEL,
        stats=stats,
        inputs={
            "ugc_dir": args.ugc_dir,
            "manifest_csv": args.manifest_csv,
            "progress_path": args.progress_path,
        },
        outputs={"jsonl": args.output, "manifest": args.manifest_out},
        config={
            "clips_subdir": args.clips_subdir,
            "clip_suffix": args.clip_suffix,
            "bucket_prefix": args.bucket_prefix,
            "min_csv_rows": _UGC_MIN_ROWS,
            "max_rows": max_rows,
            "full": args.full,
            "corpus_version": args.corpus_version,
            "attrition_warn_threshold": args.attrition_warn_threshold,
        },
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
