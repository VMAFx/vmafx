#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""CHUG -> MOS-corpus JSONL adapter.

CHUG (Crowdsourced User-Generated HDR Video Quality Dataset; Saini,
Bovik, Birkbeck, Wang, Adsumilli, ICIP 2025) contains 5,992 UGC-HDR
bitrate-ladder videos with subjective MOS values. The public manifest
ships MOS on a 0-100 axis; this adapter stores that raw value as
``mos_raw_0_100`` and maps ``mos`` to the fork's MOS-head 1-5 axis via
``1 + 4 * mos_raw / 100``.

Pipeline shape::

    .workingdir2/chug/
      +-- manifest.csv              # downloaded from the CHUG repo
      +-- .download-progress.json
      +-- clips/
      +-- chug.jsonl

License: the repository README advertises CC BY-NC 4.0, while
``license.txt`` contains CC BY-NC-SA 4.0 text. Treat CHUG as
non-commercial/share-alike research data until clarified. This script
does not ship any clip, MOS value, or derived feature in tree.
"""

from __future__ import annotations

import argparse
import csv
import logging
import os
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
    pick,
    utc_now_iso,
    write_ingest_manifest,
)

save_progress = _corpus_base.save_progress

_LOG = logging.getLogger("chug_to_corpus_jsonl")

_CHUG_MIN_ROWS: int = 1000
_CHUG_DEFAULT_MAX_ROWS: int = 500
# Default working directory for CHUG inputs/outputs. Override with
# ``VMAF_CHUG_DIR`` env var (e.g. inside the dev-mcp container or on
# operator machines that don't use the maintainer's ``.workingdir2``
# layout). Falls back to the in-tree default for backwards compatibility.
_DEFAULT_CHUG_DIR: Path = Path(
    os.environ.get(
        "VMAF_CHUG_DIR",
        str(REPO_ROOT / ".corpus" / "chug"),
    )
)
_DEFAULT_OUTPUT: Path = _DEFAULT_CHUG_DIR / "chug.jsonl"
_DEFAULT_CLIPS_SUBDIR: str = "clips"
_DEFAULT_MANIFEST_NAME: str = "manifest.csv"
_DEFAULT_CORPUS_VERSION: str = "chug-icip-2025"
_CORPUS_LABEL: str = "chug"
_S3_BASE_URL: str = "https://ugchdrmturk.s3.us-east-2.amazonaws.com/videos"

_CSV_VIDEO_KEYS: tuple[str, ...] = ("Video", "video", "video_id", "id")
_CSV_MOS_KEYS: tuple[str, ...] = ("mos_j", "MOS", "mos")
_CSV_SOS_KEYS: tuple[str, ...] = ("sos_j", "SOS", "sos")
_CSV_REF_KEYS: tuple[str, ...] = ("ref", "reference")
_CSV_NAME_KEYS: tuple[str, ...] = ("name", "filename", "file_name")
_CSV_BITLADDER_KEYS: tuple[str, ...] = ("bitladder", "bitrate_ladder")
_CSV_RESOLUTION_KEYS: tuple[str, ...] = ("resolution",)
_CSV_BITRATE_KEYS: tuple[str, ...] = ("bitrate",)
_CSV_ORIENTATION_KEYS: tuple[str, ...] = ("orientation",)
_CSV_FRAMERATE_KEYS: tuple[str, ...] = ("framerate", "fps")
_CSV_CONTENT_KEYS: tuple[str, ...] = ("content_name", "content")
_CSV_HEIGHT_KEYS: tuple[str, ...] = ("height",)
_CSV_WIDTH_KEYS: tuple[str, ...] = ("width",)


def _float_field(row: dict[str, str], keys: tuple[str, ...], default: float = 0.0) -> float:
    raw = pick(row, keys)
    if raw is None:
        return default
    try:
        return float(raw)
    except ValueError:
        return default


def _int_field(row: dict[str, str], keys: tuple[str, ...], default: int = 0) -> int:
    raw = pick(row, keys)
    if raw is None:
        return default
    try:
        return int(float(raw))
    except ValueError:
        return default


def _mos_0_100_to_1_5(mos_raw: float) -> float:
    return 1.0 + 4.0 * max(0.0, min(100.0, mos_raw)) / 100.0


def _video_url(video_id: str) -> str:
    return f"{_S3_BASE_URL}/{video_id}.mp4"


def parse_manifest_csv(csv_path: Path, *, min_rows: int = _CHUG_MIN_ROWS) -> list[dict[str, Any]]:
    """Parse CHUG's public ``chug.csv`` manifest."""
    if not csv_path.is_file():
        raise FileNotFoundError(f"CHUG manifest CSV not found: {csv_path}")

    with csv_path.open(newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            raise ValueError(f"{csv_path}: empty / headerless CSV")
        all_rows = list(reader)

    if len(all_rows) < min_rows:
        raise ValueError(
            f"{csv_path}: {len(all_rows)} rows is below the CHUG sanity floor ({min_rows})"
        )

    parsed: list[dict[str, Any]] = []
    for line_no, row in enumerate(all_rows, start=2):
        video_id = pick(row, _CSV_VIDEO_KEYS)
        if not video_id:
            _LOG.warning("%s:%d: missing Video column; skipping", csv_path, line_no)
            continue
        mos_raw = _float_field(row, _CSV_MOS_KEYS, default=float("nan"))
        if mos_raw != mos_raw:
            _LOG.warning("%s:%d: missing/bad MOS for %s; skipping", csv_path, line_no, video_id)
            continue
        filename = f"{video_id}.mp4"
        parsed.append(
            {
                "filename": filename,
                "url": _video_url(video_id),
                "mos": _mos_0_100_to_1_5(mos_raw),
                "mos_raw_0_100": mos_raw,
                "mos_std_dev": _float_field(row, _CSV_SOS_KEYS),
                "n_ratings": 0,
                "chug_video_id": video_id,
                "chug_ref": _int_field(row, _CSV_REF_KEYS),
                "chug_name": pick(row, _CSV_NAME_KEYS) or "",
                "chug_bitladder": pick(row, _CSV_BITLADDER_KEYS) or "",
                "chug_resolution": pick(row, _CSV_RESOLUTION_KEYS) or "",
                "chug_bitrate_label": pick(row, _CSV_BITRATE_KEYS) or "",
                "chug_orientation": pick(row, _CSV_ORIENTATION_KEYS) or "",
                "chug_framerate_manifest": _float_field(row, _CSV_FRAMERATE_KEYS),
                "chug_content_name": pick(row, _CSV_CONTENT_KEYS) or "",
                "chug_height_manifest": _int_field(row, _CSV_HEIGHT_KEYS),
                "chug_width_manifest": _int_field(row, _CSV_WIDTH_KEYS),
            }
        )
    return parsed


class CHUGIngest(CorpusIngestBase):
    """MOS-corpus ingest adapter for CHUG."""

    corpus_label = _CORPUS_LABEL

    def __init__(
        self,
        *,
        chug_dir: Path,
        min_csv_rows: int = _CHUG_MIN_ROWS,
        **kwargs: Any,
    ) -> None:
        super().__init__(corpus_dir=chug_dir, **kwargs)
        self._min_csv_rows = min_csv_rows

    def iter_source_rows(self, clips_dir: Path) -> Iterator[tuple[Path, dict[str, Any]]]:
        rows = parse_manifest_csv(self.manifest_csv, min_rows=self._min_csv_rows)
        _LOG.info("parsed %d CHUG manifest rows from %s", len(rows), self.manifest_csv.name)
        for row in rows:
            yield clips_dir / row["filename"], row

    def _build_jsonl_row(
        self,
        clip_path: Path,
        manifest_row: dict[str, Any],
        geometry: dict[str, Any],
        ingested_at_utc: str,
        src_sha256: str,
    ) -> dict[str, Any]:
        row = super()._build_jsonl_row(
            clip_path,
            manifest_row,
            geometry,
            ingested_at_utc,
            src_sha256,
        )
        for key in (
            "mos_raw_0_100",
            "chug_video_id",
            "chug_ref",
            "chug_name",
            "chug_bitladder",
            "chug_resolution",
            "chug_bitrate_label",
            "chug_orientation",
            "chug_framerate_manifest",
            "chug_content_name",
            "chug_height_manifest",
            "chug_width_manifest",
        ):
            row[key] = manifest_row[key]
        return row


def run(
    *,
    chug_dir: Path,
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
    min_csv_rows: int = _CHUG_MIN_ROWS,
    max_rows: int | None = _CHUG_DEFAULT_MAX_ROWS,
) -> RunStats:
    """Build CHUG JSONL. Returns a :class:`RunStats`."""
    ingest = CHUGIngest(
        chug_dir=chug_dir,
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
    )
    return ingest.run()


def _build_parser() -> argparse.ArgumentParser:
    parser = make_argument_parser(description=__doc__)
    parser.add_argument("--chug-dir", type=Path, default=_DEFAULT_CHUG_DIR)
    parser.add_argument("--manifest-csv", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=_DEFAULT_OUTPUT)
    parser.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Replay manifest JSON sidecar (default: <output>.manifest.json).",
    )
    parser.add_argument("--clips-subdir", default=_DEFAULT_CLIPS_SUBDIR)
    parser.add_argument("--ffprobe-bin", default="ffprobe")
    parser.add_argument("--curl-bin", default="curl")
    parser.add_argument("--download-timeout-s", type=int, default=300)
    parser.add_argument("--attrition-warn-threshold", type=float, default=0.10)
    parser.add_argument("--min-csv-rows", type=int, default=_CHUG_MIN_ROWS)
    parser.add_argument("--max-rows", type=int, default=_CHUG_DEFAULT_MAX_ROWS)
    parser.add_argument(
        "--full",
        action="store_true",
        help="ingest the whole CHUG manifest instead of the laptop-class --max-rows cap",
    )
    parser.add_argument("--verbose", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.output.with_suffix(".manifest.json")
    logging.basicConfig(
        level=logging.INFO if args.verbose else logging.WARNING,
        format="%(levelname)s:%(name)s:%(message)s",
    )
    max_rows = None if args.full else args.max_rows
    try:
        stats = run(
            chug_dir=args.chug_dir,
            output=args.output,
            manifest_csv=args.manifest_csv,
            clips_subdir=args.clips_subdir,
            ffprobe_bin=args.ffprobe_bin,
            curl_bin=args.curl_bin,
            attrition_warn_threshold=args.attrition_warn_threshold,
            download_timeout_s=args.download_timeout_s,
            min_csv_rows=args.min_csv_rows,
            max_rows=max_rows,
        )
    except (FileNotFoundError, ValueError) as exc:
        print(f"chug_to_corpus_jsonl: {exc}", file=sys.stderr)
        return 2
    write_ingest_manifest(
        args.manifest_out,
        schema="chug-corpus-jsonl-manifest-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        corpus_label=_CORPUS_LABEL,
        stats=stats,
        inputs={
            "chug_dir": args.chug_dir,
            "manifest_csv": args.manifest_csv,
            "progress_path": None,
        },
        outputs={"jsonl": args.output, "manifest": args.manifest_out},
        config={
            "clips_subdir": args.clips_subdir,
            "min_csv_rows": args.min_csv_rows,
            "max_rows": max_rows,
            "full": args.full,
            "corpus_version": _DEFAULT_CORPUS_VERSION,
        },
    )
    print(
        "chug_to_corpus_jsonl: "
        f"written={stats.written} skipped_download={stats.skipped_download} "
        f"skipped_broken={stats.skipped_broken} dedups={stats.dedups} "
        f"attrition={stats.attrition_pct:.3f}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
