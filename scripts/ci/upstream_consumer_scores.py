#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Compare per-frame scores written by libvmaf consumers, as exact text.

Upstream FFmpeg (``libvmaf`` filter), upstream GStreamer (``vmaf`` element)
and the ``vmaf`` CLI all end in ``vmaf_write_output()``, so each writes the
same shapes. JSON: ``{"frames": [{"frameNum": N, "metrics": {name: value}}]}``;
XML: ``<frames><frame frameNum="N" name="value" .../></frames>`` plus
``<pooled_metrics><metric name=".." min=".." .../></pooled_metrics>``; CSV: a
``Frame,name,...`` header and one row per frame (no pooled values). Values are
kept as the literal text of the file (never converted to float), so a one-ulp
difference in the last printed digit is a mismatch. Elements outside the
frames and pooled metrics (version, timing, provenance) are not compared.

Usage::

    upstream_consumer_scores.py A B [--label-a NAME] [--label-b NAME]
                                [--pooled] [--format auto|json|xml|csv]

``--format auto`` (the default) picks the reader from each file's suffix.

Exit status: 0 identical, 1 mismatch (first one printed), 2 unreadable input.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

Scores = dict[int, dict[str, str]]
Pooled = dict[str, dict[str, str]]

EXIT_SAME = 0
EXIT_DIFF = 1
EXIT_SETUP = 2

FORMATS = ("json", "xml", "csv")
POOLED_STATS = ("min", "max", "mean", "harmonic_mean")


class ScoreFileError(Exception):
    """The file is missing, not parseable, or lacks the expected frame structure."""


def _read_document(path: Path) -> dict[str, Any]:
    """Parse ``path`` with every number kept as its literal text."""
    try:
        text = path.read_text(encoding="utf-8")
        doc = json.loads(text, parse_float=str, parse_int=str)
    except (OSError, ValueError) as exc:
        raise ScoreFileError(f"{path}: {exc}") from exc
    if not isinstance(doc, dict) or not isinstance(doc.get("frames"), list):
        raise ScoreFileError(f"{path}: no 'frames' array")
    return doc


def _add_frame(scores: Scores, path: Path, num: int, metrics: dict[str, str]) -> None:
    """Record one frame, refusing a frame number seen before."""
    if num in scores:
        raise ScoreFileError(f"{path}: frame {num} appears twice")
    scores[num] = metrics


def _require_frames(scores: Scores, path: Path) -> Scores:
    if not scores:
        raise ScoreFileError(f"{path}: zero frames scored")
    return scores


def load_scores(path: Path) -> Scores:
    """Return ``{frameNum: {metric: literal text}}`` for the JSON file ``path``."""
    doc = _read_document(path)
    scores: Scores = {}
    for entry in doc["frames"]:
        try:
            num = int(entry["frameNum"])
            metrics = {str(k): str(v) for k, v in entry["metrics"].items()}
        except (KeyError, TypeError, ValueError, AttributeError) as exc:
            raise ScoreFileError(f"{path}: malformed frame entry: {exc!r}") from exc
        _add_frame(scores, path, num, metrics)
    return _require_frames(scores, path)


def load_pooled(path: Path) -> Pooled:
    """Return ``{metric: {statistic: literal text}}`` from ``pooled_metrics``."""
    pooled = _read_document(path).get("pooled_metrics", {})
    if not isinstance(pooled, dict):
        raise ScoreFileError(f"{path}: 'pooled_metrics' is not an object")
    return {str(m): {str(k): str(v) for k, v in s.items()} for m, s in pooled.items()}


def _read_xml_root(path: Path) -> ET.Element:
    """Parse ``path`` as XML; the inputs are this tool's own consumers' output."""
    try:
        root = ET.parse(path).getroot()  # noqa: S314 - own tool output, no entities
    except (OSError, ET.ParseError) as exc:
        raise ScoreFileError(f"{path}: {exc}") from exc
    if root.find("frames") is None:
        raise ScoreFileError(f"{path}: no <frames> element")
    return root


def load_xml(path: Path) -> tuple[Scores, Pooled]:
    """Return the frames and pooled metrics of a ``vmaf_write_output()`` XML file."""
    root = _read_xml_root(path)
    scores: Scores = {}
    for frame in root.iterfind("frames/frame"):
        attrs = dict(frame.attrib)
        try:
            num = int(attrs.pop("frameNum"))
        except (KeyError, ValueError) as exc:
            raise ScoreFileError(f"{path}: malformed <frame>: {exc!r}") from exc
        _add_frame(scores, path, num, attrs)
    pooled: Pooled = {}
    for metric in root.iterfind("pooled_metrics/metric"):
        name = metric.get("name")
        if not name or name in pooled:
            raise ScoreFileError(f"{path}: pooled metric without a unique name: {name!r}")
        pooled[name] = {k: v for k, v in metric.attrib.items() if k in POOLED_STATS}
    return _require_frames(scores, path), pooled


def load_csv(path: Path) -> tuple[Scores, Pooled]:
    """Return the frames of a ``vmaf_write_output()`` CSV file (CSV has no pooled values).

    Every line, header included, ends in a comma, so the last column is empty
    and dropped.
    """
    try:
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="utf-8"))))
    except (OSError, ValueError, csv.Error) as exc:
        raise ScoreFileError(f"{path}: {exc}") from exc
    if not rows or not rows[0] or rows[0][0] != "Frame":
        raise ScoreFileError(f"{path}: no 'Frame,...' header")
    header = rows[0][:-1] if rows[0][-1] == "" else rows[0]
    scores: Scores = {}
    for line, row in enumerate(rows[1:], start=2):
        cells = row[:-1] if row and row[-1] == "" else row
        if len(cells) != len(header):
            raise ScoreFileError(f"{path}:{line}: {len(cells)} cells, header has {len(header)}")
        try:
            num = int(cells[0])
        except ValueError as exc:
            raise ScoreFileError(f"{path}:{line}: frame number {cells[0]!r}") from exc
        _add_frame(scores, path, num, dict(zip(header[1:], cells[1:], strict=True)))
    return _require_frames(scores, path), {}


def detect_format(path: Path) -> str:
    """Return the reader for ``path`` from its suffix."""
    fmt = path.suffix.lower().lstrip(".")
    if fmt not in FORMATS:
        raise ScoreFileError(f"{path}: cannot tell the format from the suffix; pass --format")
    return fmt


def load(path: Path, fmt: str = "auto") -> tuple[Scores, Pooled]:
    """Return the frames and pooled metrics of ``path`` in format ``fmt``."""
    fmt = detect_format(path) if fmt == "auto" else fmt
    if fmt == "xml":
        return load_xml(path)
    if fmt == "csv":
        return load_csv(path)
    if fmt == "json":
        return load_scores(path), load_pooled(path)
    raise ScoreFileError(f"unknown format {fmt!r}")


def rename_metric(scores: Scores, pooled: Pooled, old: str, new: str) -> None:
    """Rename metric ``old`` to ``new`` in place; values are not touched.

    Needed for one documented upstream quirk: the GStreamer element names its
    model ``self`` (``gstvmafelement.c``), so its score is called ``self``
    where every other consumer calls it ``vmaf``.
    """
    for num, metrics in scores.items():
        if new in metrics:
            raise ScoreFileError(f"frame {num}: cannot rename {old} to {new}: {new} exists")
        if old in metrics:
            metrics[new] = metrics.pop(old)
    if old in pooled:
        if new in pooled:
            raise ScoreFileError(f"cannot rename pooled {old} to {new}: {new} exists")
        pooled[new] = pooled.pop(old)


def _frame_diffs(a: Scores, b: Scores, la: str, lb: str) -> list[str]:
    out: list[str] = []
    for num in sorted(set(a) | set(b)):
        if num not in a or num not in b:
            side = la if num not in a else lb
            out.append(f"frame {num}: missing from {side}")
            continue
        for name in sorted(set(a[num]) | set(b[num])):
            va, vb = a[num].get(name), b[num].get(name)
            if va != vb:
                out.append(f"frame {num} {name}: {la}={va!r} {lb}={vb!r}")
    return out


def _pooled_diffs(a: Pooled, b: Pooled, la: str, lb: str) -> list[str]:
    out: list[str] = []
    for name in sorted(set(a) | set(b)):
        sa, sb = a.get(name, {}), b.get(name, {})
        for stat in sorted(set(sa) | set(sb)):
            if sa.get(stat) != sb.get(stat):
                out.append(f"pooled {name}.{stat}: {la}={sa.get(stat)!r} {lb}={sb.get(stat)!r}")
    return out


def compare(a: Scores, b: Scores, label_a: str = "A", label_b: str = "B") -> list[str]:
    """Return one line per difference between two score sets (empty: identical)."""
    return _frame_diffs(a, b, label_a, label_b)


def compare_pooled(a: Pooled, b: Pooled, label_a: str = "A", label_b: str = "B") -> list[str]:
    """Return one line per difference between two pooled-metric sets."""
    return _pooled_diffs(a, b, label_a, label_b)


def _apply_rename(spec: str | None, scores: Scores, pooled: Pooled, label: str) -> None:
    """Apply one ``OLD=NEW`` rename request, if any, and say so."""
    if not spec:
        return
    old, sep, new = spec.partition("=")
    if not sep or not old or not new:
        raise ScoreFileError(f"rename needs OLD=NEW, got {spec!r}")
    rename_metric(scores, pooled, old, new)
    print(f"note: renamed metric {old} to {new} in {label}")


def _parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    ap.add_argument("--pooled", action="store_true", help="also compare pooled_metrics")
    ap.add_argument(
        "--format",
        choices=("auto", *FORMATS),
        default="auto",
        help="reader for both files (default: from each file's suffix)",
    )
    for side in ("a", "b"):
        ap.add_argument(
            f"--rename-{side}",
            metavar="OLD=NEW",
            help=f"rename metric OLD to NEW in file {side.upper()} before comparing "
            "(values untouched)",
        )
    return ap.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point; returns the process exit status."""
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        sa, pa = load(args.a, args.format)
        sb, pb = load(args.b, args.format)
        _apply_rename(args.rename_a, sa, pa, args.label_a)
        _apply_rename(args.rename_b, sb, pb, args.label_b)
        diffs = compare(sa, sb, args.label_a, args.label_b)
        if args.pooled:
            diffs += compare_pooled(pa, pb, args.label_a, args.label_b)
    except ScoreFileError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return EXIT_SETUP
    if diffs:
        print(f"MISMATCH: {len(diffs)} difference(s); first: {diffs[0]}")
        for line in diffs[1:10]:
            print(f"  also: {line}")
        return EXIT_DIFF
    n_metrics = sum(len(m) for m in sa.values())
    print(
        f"IDENTICAL: {len(sa)} frame(s), {n_metrics} value(s): " f"{args.label_a} == {args.label_b}"
    )
    return EXIT_SAME


if __name__ == "__main__":
    sys.exit(main())
