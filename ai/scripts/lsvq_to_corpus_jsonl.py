#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""LSVQ -> MOS-corpus JSONL adapter (ADR-0367).

LSVQ -- LIVE Large-Scale Social Video Quality (Ying, Mandal,
Ghadiyaram, Bovik; ICCV 2021) -- is the canonical large-scale
no-reference VQA training corpus: ~39 000 user-generated videos with
~5.5 M individual subjective ratings collapsed into per-clip MOS values
on a 1.0-5.0 scale.

Shared infrastructure: :mod:`ai.src.corpus.base` (ADR-0371).

Pipeline shape::

    .workingdir2/lsvq/
      +-- .download-progress.json
      +-- manifest.csv
      +-- clips/
      +-- lsvq.jsonl

License: LSVQ is CC-BY-4.0 (per ADR-0367). This script does not ship
any clip, MOS value, or derived feature in tree; only the adapter and
schema land in the repo. The corpus is available at
https://github.com/teowu/LSVQ-videos and via HuggingFace.
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

_LOG = logging.getLogger("lsvq_to_corpus_jsonl")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_LSVQ_MIN_ROWS: int = 1000
_LSVQ_DEFAULT_MAX_ROWS: int = 500
# Default working directory; override with ``VMAF_LSVQ_DIR`` env var
# for container / non-maintainer layouts.
_DEFAULT_LSVQ_DIR: Path = Path(
    os.environ.get(
        "VMAF_LSVQ_DIR",
        str(REPO_ROOT / ".corpus" / "lsvq"),
    )
)
_DEFAULT_OUTPUT: Path = _DEFAULT_LSVQ_DIR / "lsvq.jsonl"
_DEFAULT_CLIPS_SUBDIR: str = "clips"
_DEFAULT_MANIFEST_NAME: str = "manifest.csv"
_DEFAULT_CLIP_SUFFIX: str = ".mp4"

_CSV_FILENAME_KEYS: tuple[str, ...] = ("name", "video_name", "filename", "file_name")
_CSV_URL_KEYS: tuple[str, ...] = ("url", "download_url", "video_url")
_CSV_MOS_KEYS: tuple[str, ...] = ("mos", "MOS", "mos_score")
_CSV_MOS_STD_KEYS: tuple[str, ...] = ("sd", "SD", "mos_std", "mos_std_dev", "SD_MOS")
_CSV_NRATINGS_KEYS: tuple[str, ...] = ("n", "ratings", "num_ratings", "n_ratings")

_DEFAULT_CORPUS_VERSION: str = "lsvq-2021"
_CORPUS_LABEL: str = "lsvq"


# ---------------------------------------------------------------------------
# Manifest CSV
# ---------------------------------------------------------------------------


def parse_manifest_csv(
    csv_path: Path,
    *,
    min_rows: int = _LSVQ_MIN_ROWS,
    clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
) -> list[dict[str, Any]]:
    """Parse an LSVQ split CSV; refuse if row count is below floor."""
    if not csv_path.is_file():
        raise FileNotFoundError(f"LSVQ manifest CSV not found: {csv_path}")

    with csv_path.open(newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            raise ValueError(f"{csv_path}: empty / headerless CSV")
        all_rows = list(reader)

    if len(all_rows) < min_rows:
        raise ValueError(
            f"{csv_path}: {len(all_rows)} rows is below the LSVQ sanity floor "
            f"({min_rows}). Canonical LSVQ splits start at ~3 600 rows."
        )

    parsed: list[dict[str, Any]] = []
    for line_no, row in enumerate(all_rows, start=2):
        stem = pick(row, _CSV_FILENAME_KEYS)
        if not stem:
            _LOG.warning(
                "%s:%d: no filename column (looked for %s); skipping",
                csv_path,
                line_no,
                ", ".join(_CSV_FILENAME_KEYS),
            )
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


# ---------------------------------------------------------------------------
# Adapter subclass
# ---------------------------------------------------------------------------


class LSVQIngest(CorpusIngestBase):
    """MOS-corpus ingest adapter for LSVQ (ADR-0367)."""

    corpus_label = _CORPUS_LABEL

    def __init__(
        self,
        *,
        lsvq_dir: Path,
        min_csv_rows: int = _LSVQ_MIN_ROWS,
        clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
        **kwargs: Any,
    ) -> None:
        super().__init__(corpus_dir=lsvq_dir, **kwargs)
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
    lsvq_dir: Path,
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
    min_csv_rows: int = _LSVQ_MIN_ROWS,
    max_rows: int | None = _LSVQ_DEFAULT_MAX_ROWS,
    clip_suffix: str = _DEFAULT_CLIP_SUFFIX,
) -> RunStats:
    """Build the JSONL. Returns a :class:`RunStats`."""
    ingest = LSVQIngest(
        lsvq_dir=lsvq_dir,
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
        prog="lsvq_to_corpus_jsonl.py",
        description=(
            "ADR-0367: walk a local LSVQ extraction (or build one via "
            "resumable downloads), probe each clip via ffprobe, join with "
            "the manifest CSV's MOS scores, and emit one JSONL row per clip."
        ),
    )
    ap.add_argument(
        "--lsvq-dir",
        type=Path,
        default=_DEFAULT_LSVQ_DIR,
        help="Local LSVQ working directory (default: .workingdir2/lsvq/).",
    )
    ap.add_argument(
        "--manifest-csv",
        type=Path,
        default=None,
        help="Path to the split CSV (default: <lsvq-dir>/manifest.csv).",
    )
    ap.add_argument(
        "--progress-path", type=Path, default=None, help="Resumable-download state file."
    )
    ap.add_argument(
        "--clips-subdir",
        default=_DEFAULT_CLIPS_SUBDIR,
        help=f"Subdirectory for downloaded clips (default: {_DEFAULT_CLIPS_SUBDIR!r}).",
    )
    ap.add_argument(
        "--clip-suffix",
        default=_DEFAULT_CLIP_SUFFIX,
        help=f"Default extension for bare-stem CSV names (default: {_DEFAULT_CLIP_SUFFIX!r}).",
    )
    ap.add_argument(
        "--output",
        type=Path,
        default=_DEFAULT_OUTPUT,
        help="Output JSONL path (default: .workingdir2/lsvq/lsvq.jsonl).",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Replay manifest JSON sidecar (default: <output>.manifest.json).",
    )
    ap.add_argument(
        "--ffprobe-bin", default=os.environ.get("FFPROBE_BIN", "ffprobe"), help="ffprobe binary."
    )
    ap.add_argument("--curl-bin", default=os.environ.get("CURL_BIN", "curl"), help="curl binary.")
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
        default=_LSVQ_DEFAULT_MAX_ROWS,
        help=f"Cap manifest at this many rows (default: {_LSVQ_DEFAULT_MAX_ROWS}). Mutually exclusive with --full.",
    )
    ap.add_argument(
        "--full", action="store_true", help="Ingest the entire manifest. Overrides --max-rows."
    )
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
            lsvq_dir=args.lsvq_dir,
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
        print(f"error: {exc}", file=sys.stderr)
        print(
            "hint: obtain LSVQ-videos from " "https://github.com/teowu/LSVQ-videos or HuggingFace",
            file=sys.stderr,
        )
        return 2
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    write_ingest_manifest(
        args.manifest_out,
        schema="lsvq-corpus-jsonl-manifest-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        corpus_label=_CORPUS_LABEL,
        stats=stats,
        inputs={
            "lsvq_dir": args.lsvq_dir,
            "manifest_csv": args.manifest_csv,
            "progress_path": args.progress_path,
        },
        outputs={"jsonl": args.output, "manifest": args.manifest_out},
        config={
            "clips_subdir": args.clips_subdir,
            "clip_suffix": args.clip_suffix,
            "min_csv_rows": _LSVQ_MIN_ROWS,
            "max_rows": max_rows,
            "full": args.full,
            "corpus_version": args.corpus_version,
            "attrition_warn_threshold": args.attrition_warn_threshold,
        },
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
