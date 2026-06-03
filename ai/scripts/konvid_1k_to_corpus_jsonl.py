#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""KoNViD-1k -> MOS-corpus JSONL adapter (Phase 1 of ADR-0325).

ADR-0325 plans a two-phase ingestion of the Konstanz UGC datasets. This
script is **Phase 1**: the small KoNViD-1k predecessor (~1.2k clips,
~5 GB) that validates the conversion pipeline shape before scaling to
KonViD-150k (Phase 2, separate script).

Shared infrastructure: :mod:`ai.src.corpus.base` (ADR-0371).  The
per-corpus boilerplate (ffprobe, SHA-256, JSONL dedup, download state)
lives there; this file contains only the KonViD-1k-specific CSV parsing
and the ``KonViD1kIngest`` subclass.

Pipeline shape::

    .workingdir2/konvid-1k/
      +-- KoNViD_1k_videos/                 # *.mp4 clips (1200)
      +-- KoNViD_1k_metadata/
          +-- KoNViD_1k_attributes.csv      # MOS table

                  |
                  v  ai/scripts/konvid_1k_to_corpus_jsonl.py
                  v
    .workingdir2/konvid-1k/konvid_1k.jsonl  # one row per clip

Schema (one JSON object per line)::

    {
      "src":               "<basename>.mp4",
      "src_sha256":        "<hex>",
      "src_size_bytes":    <int>,
      "width":             <int>,
      "height":            <int>,
      "framerate":         <float>,
      "duration_s":        <float>,
      "pix_fmt":           "<yuv420p|...>",
      "encoder_upstream":  "<ffprobe codec_name; e.g. h264, vp9>",
      "mos":               <float, 1..5>,
      "mos_std_dev":       <float>,
      "n_ratings":         <int>,
      "corpus":            "konvid-1k",
      "corpus_version":    "<dataset version string>",
      "ingested_at_utc":   "<ISO 8601>"
    }

Refusal: if the CSV has more than :data:`_KONVID_1K_MAX_ROWS` rows
(currently 1500) the script refuses with a hint pointing at the
KonViD-150k script (Phase 2). Silently ingesting a ~150k-row CSV
through the 1k-shaped pipeline would mask the wrong corpus being
mounted.

License: KonViD-1k is research-only. The script does **not** ship any
clip, MOS value, or derived feature in tree (per ADR-0325).
"""

from __future__ import annotations

import argparse
import csv
import logging
import os
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from corpus.base import CorpusIngestBase, pick, utc_now_iso, write_ingest_manifest  # noqa: E402

_LOG = logging.getLogger("konvid_1k_to_corpus_jsonl")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_KONVID_1K_MAX_ROWS: int = 1500
# Default working directory; override with ``VMAF_KONVID_1K_DIR`` env
# var for container / non-maintainer layouts.
_DEFAULT_KONVID_DIR: Path = Path(
    os.environ.get(
        "VMAF_KONVID_1K_DIR",
        str(REPO_ROOT / ".corpus" / "konvid-1k"),
    )
)
_DEFAULT_OUTPUT: Path = _DEFAULT_KONVID_DIR / "konvid_1k.jsonl"
_VIDEOS_SUBDIRS: tuple[str, ...] = ("KoNViD_1k_videos", "videos")
_METADATA_SUBDIRS: tuple[str, ...] = ("KoNViD_1k_metadata", "metadata")

_CSV_FILENAME_KEYS: tuple[str, ...] = ("file_name", "video_name", "filename", "name")
_CSV_MOS_KEYS: tuple[str, ...] = ("MOS", "mos")
_CSV_MOS_STD_KEYS: tuple[str, ...] = ("SD", "sd", "mos_std", "mos_std_dev", "SD_MOS")
_CSV_NRATINGS_KEYS: tuple[str, ...] = ("n", "ratings", "num_ratings", "n_ratings")
_CSV_FLICKR_KEYS: tuple[str, ...] = ("flickr_id", "id")

_DEFAULT_CORPUS_VERSION: str = "konvid-1k-2017"
_CORPUS_LABEL: str = "konvid-1k"


# ---------------------------------------------------------------------------
# CSV helpers
# ---------------------------------------------------------------------------


def _resolve_subdir(root: Path, candidates: tuple[str, ...]) -> Path | None:
    for name in candidates:
        p = root / name
        if p.is_dir():
            return p
    return None


def _find_attributes_csv(metadata_dir: Path) -> Path | None:
    candidates: list[Path] = []
    candidates.extend(metadata_dir.glob("*attributes*.csv"))
    candidates.extend(metadata_dir.glob("*mos*.csv"))
    if not candidates:
        csvs = list(metadata_dir.glob("*.csv"))
        if len(csvs) == 1:
            return csvs[0]
        return None
    return sorted(candidates)[0]


def parse_mos_csv(csv_path: Path) -> list[dict[str, Any]]:
    """Parse a KonViD-1k MOS CSV; refuse if row count exceeds the 1k ceiling."""
    if not csv_path.is_file():
        raise FileNotFoundError(f"KonViD-1k MOS CSV not found: {csv_path}")

    with csv_path.open(newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            raise ValueError(f"{csv_path}: empty / headerless CSV")
        all_rows = list(reader)

    if len(all_rows) > _KONVID_1K_MAX_ROWS:
        raise ValueError(
            f"{csv_path}: {len(all_rows)} rows exceeds the KonViD-1k "
            f"sanity ceiling ({_KONVID_1K_MAX_ROWS}). This looks like "
            f"the KonViD-150k attributes CSV. Run the Phase 2 ingestion "
            f"script (ai/scripts/konvid_150k_to_corpus_jsonl.py; see ADR-0325 S.Phase 2)."
        )

    parsed: list[dict[str, Any]] = []
    for line_no, row in enumerate(all_rows, start=2):
        filename = pick(row, _CSV_FILENAME_KEYS)
        if not filename:
            _LOG.warning(
                "%s:%d: no filename column (looked for %s); skipping",
                csv_path,
                line_no,
                ", ".join(_CSV_FILENAME_KEYS),
            )
            continue
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
                "mos": mos,
                "mos_std_dev": mos_std_dev,
                "n_ratings": n_ratings,
                "url": "",
            }
        )
    return parsed


# ---------------------------------------------------------------------------
# Adapter subclass
# ---------------------------------------------------------------------------


class KonViD1kIngest(CorpusIngestBase):
    """MOS-corpus ingest adapter for KonViD-1k (ADR-0325 Phase 1)."""

    corpus_label = _CORPUS_LABEL

    def __init__(self, *, konvid_dir: Path, **kwargs: Any) -> None:
        super().__init__(corpus_dir=konvid_dir, **kwargs)
        self._konvid_dir = konvid_dir

    def iter_source_rows(self, clips_dir: Path) -> Iterator[tuple[Path, dict[str, Any]]]:
        videos_dir = _resolve_subdir(self._konvid_dir, _VIDEOS_SUBDIRS) or self._konvid_dir
        metadata_dir = _resolve_subdir(self._konvid_dir, _METADATA_SUBDIRS) or self._konvid_dir

        csv_path = _find_attributes_csv(metadata_dir)
        if csv_path is None:
            raise FileNotFoundError(
                f"KonViD-1k MOS CSV not found under {metadata_dir}.\n"
                f"  Expected one of: KoNViD_1k_attributes.csv / *mos*.csv"
            )

        csv_rows = parse_mos_csv(csv_path)
        _LOG.info("parsed %d CSV rows from %s", len(csv_rows), csv_path.name)

        for row in csv_rows:
            clip_path = videos_dir / row["filename"]
            if not clip_path.is_file():
                hits = list(videos_dir.rglob(row["filename"]))
                if not hits:
                    _LOG.warning("clip missing on disk: %s", row["filename"])
                    continue
                clip_path = hits[0]
            yield clip_path, row


# ---------------------------------------------------------------------------
# Backward-compatible functional entry point
# ---------------------------------------------------------------------------


def run(
    *,
    konvid_dir: Path,
    output: Path,
    ffprobe_bin: str = "ffprobe",
    corpus_version: str = _DEFAULT_CORPUS_VERSION,
    runner=None,
    now_fn=utc_now_iso,
) -> tuple[int, int, int]:
    """Build the JSONL. Returns ``(written, skipped_broken, dedups)``."""
    import subprocess as _sp

    ingest = KonViD1kIngest(
        konvid_dir=konvid_dir,
        output=output,
        ffprobe_bin=ffprobe_bin,
        corpus_version=corpus_version,
        runner=runner or _sp.run,
        now_fn=now_fn,
        max_rows=None,
    )
    stats = ingest.run()
    return stats.written, stats.skipped_broken, stats.dedups


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    ap = make_argument_parser(
        prog="konvid_1k_to_corpus_jsonl.py",
        description=(
            "Phase 1 of ADR-0325: walk a local KoNViD-1k extraction, "
            "probe each clip via ffprobe, join with the attribute CSV's "
            "MOS scores, and emit one JSONL row per clip."
        ),
    )
    ap.add_argument(
        "--konvid-dir",
        type=Path,
        default=_DEFAULT_KONVID_DIR,
        help=(
            "Local KoNViD-1k extraction (default: .workingdir2/konvid-1k/). "
            "Must contain the videos and a metadata CSV."
        ),
    )
    ap.add_argument(
        "--output",
        type=Path,
        default=_DEFAULT_OUTPUT,
        help="Output JSONL path (default: .workingdir2/konvid-1k/konvid_1k.jsonl).",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Replay manifest JSON sidecar (default: <output>.manifest.json).",
    )
    ap.add_argument(
        "--ffprobe-bin",
        default=os.environ.get("FFPROBE_BIN", "ffprobe"),
        help="ffprobe binary (default: $FFPROBE_BIN or 'ffprobe').",
    )
    ap.add_argument(
        "--corpus-version",
        default=_DEFAULT_CORPUS_VERSION,
        help=f"Dataset version string baked into each row (default: {_DEFAULT_CORPUS_VERSION!r}).",
    )
    ap.add_argument(
        "--log-level",
        default="INFO",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
    )
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
    try:
        written, skipped_broken, dedups = run(
            konvid_dir=args.konvid_dir,
            output=args.output,
            ffprobe_bin=args.ffprobe_bin,
            corpus_version=args.corpus_version,
        )
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        # Surface the KonViD-1k download URL when the corpus directory
        # is missing — the test asserts the hint is present and it
        # spares users a search through the README.
        if "Corpus directory not found" in str(exc) or "MOS CSV not found" in str(exc):
            print(
                "hint: download KonViD-1k from "
                "http://database.mmsp-kn.de/konvid-1k-database.html",
                file=sys.stderr,
            )
        return 2
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    write_ingest_manifest(
        args.manifest_out,
        schema="konvid-1k-corpus-jsonl-manifest-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        corpus_label=_CORPUS_LABEL,
        stats={
            "written": written,
            "skipped_download": 0,
            "skipped_broken": skipped_broken,
            "dedups": dedups,
            "attrition_pct": 0.0,
        },
        inputs={"konvid_dir": args.konvid_dir},
        outputs={"jsonl": args.output, "manifest": args.manifest_out},
        config={"corpus_version": args.corpus_version},
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
