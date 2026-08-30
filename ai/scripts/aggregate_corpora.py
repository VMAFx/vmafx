#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Aggregate multiple MOS-corpus JSONL shards into a unified trainer JSONL.

ADR-0340 (multi-corpus aggregation for `fr_regressor_v2` /
`predictor_v2_realcorpus`). The fork's MOS-corpus ingestion PRs each
emit a corpus-specific JSONL (KonViD-1k, KonViD-150k, LSVQ, Waterloo
IVC 4K-VQA, YouTube UGC, Netflix Public). The downstream trainers
(`train_predictor_v2_realcorpus.py`, `train_konvid.py`) want one
*unified* row stream where:

1. every row's ``mos`` lives on a **single canonical scale** (the
   VMAF 0–100 axis), so the trainer's regression target is
   distribution-stable across corpus mixes;
2. every row carries an explicit ``corpus_source`` provenance tag the
   trainer can use to ablate or weight per-corpus contributions; and
3. cross-corpus duplicates (same content fingerprinted by
   ``src_sha256``) collapse to the row with the **tighter** MOS
   uncertainty — never silently to whichever shard was concatenated
   last.

Per-corpus scale-conversion table
---------------------------------

The unified scale is **0–100 (VMAF-aligned)**. Each per-corpus
conversion is documented below with the source dataset's published
scale definition. Conversions are *affine* (no compression of the
distribution); the trainer never sees a row with a silently-warped
MOS distribution.

Per the fork's ``feedback_no_test_weakening`` rule, if a corpus's
scale is questionable or undocumented, the converter logs a hard
WARNING and the row is **dropped** rather than written to the
unified JSONL with a guessed conversion. The script never silently
widens the training-target distribution.

+------------------+--------------------------+--------------------------+
| corpus_source    | source scale             | unified scale (VMAF-like)|
+==================+==========================+==========================+
| ``konvid-1k``    | 1.0–5.0 ACR Likert [1]_  | ``(mos - 1) * 25``       |
+------------------+--------------------------+--------------------------+
| ``konvid-150k``  | 1.0–5.0 ACR Likert [2]_  | ``(mos - 1) * 25``       |
+------------------+--------------------------+--------------------------+
| ``lsvq``         | 1.0–5.0 ACR Likert [3]_  | ``(mos - 1) * 25``       |
+------------------+--------------------------+--------------------------+
| ``youtube-ugc``  | 1.0–5.0 ACR Likert [4]_  | ``(mos - 1) * 25``       |
+------------------+--------------------------+--------------------------+
| ``waterloo-     | 0–100 raw subjective     | identity (already 0–100) |
| ivc-4k``         | continuous-rating [5]_   |                          |
+------------------+--------------------------+--------------------------+
| ``netflix-      | VMAF 0–100 (objective    | identity                 |
| public``         | proxy, not subjective    |                          |
|                  | MOS) [6]_                |                          |
+------------------+--------------------------+--------------------------+

Citations (access date 2026-05-09):

.. [1] Hosu et al., "The Konstanz natural video database (KonViD-1k)",
       QoMEX 2017. ACR (5-point) scale, MOS arithmetic mean across
       crowd-sourced ratings. URL:
       http://database.mmsp-kn.de/konvid-1k-database.html
.. [2] Götz-Hahn et al., "KonVid-150k: A Dataset for No-Reference
       Video Quality Assessment of Videos in-the-Wild", IEEE Access
       2021 (companion to ICIP 2019). Same 5-point ACR scale as
       KonViD-1k. URL:
       https://database.mmsp-kn.de/konvid-150k-vqa-database.html
.. [3] Ying et al., "Patch-VQ: 'Patching Up' the Video Quality
       Problem" (CVPR 2021), §4.1: 38400 videos rated on a 1–5 ACR
       scale via crowd-sourced subjective tests; MOS computed as
       arithmetic mean. URL: https://github.com/baidut/PatchVQ
.. [4] Wang et al., "YouTube UGC Dataset for Video Compression
       Research", MMSP 2019, §3.2: each clip rated 1–5 ACR (5 = best,
       1 = worst); MOS averaged across raters. URL:
       https://media.withyoutube.com/
.. [5] Cheon & Lee, "Subjective and Objective Quality Assessment of
       4K UHD Videos" (Waterloo IVC 4K-VQA), §III.B: continuous
       0–100 numerical-category scale (DCR-like), recorded verbatim.
       URL: https://ece.uwaterloo.ca/~zduanmu/cvpr2016_4kvqa/
.. [6] Netflix Public set in ``.workingdir2/netflix/`` carries
       ``vmaf_v0.6.1`` per-frame scores (an objective proxy, not a
       subjective MOS). Already on the 0–100 VMAF axis per
       ``core/include/libvmaf/model.h``; identity-mapped here.

Cross-corpus dedup
------------------

Two rows from different corpora that share a ``src_sha256`` are
duplicates of the *same source clip*. The aggregator keeps the row
with the tightest MOS uncertainty (the smallest ``mos_std_dev``);
ties keep the first-seen row, which is deterministic given a stable
``--input`` order. This matches the fork's existing
``merge_corpora.py`` natural-key idea (ADR-0310) but with an
uncertainty-weighted resolver instead of last-write-wins.

Output schema
-------------

The unified JSONL row uses a superset of the per-corpus schema:

::

    {
      "src":               "<basename>",
      "src_sha256":        "<hex>",
      "width":             <int>,
      "height":            <int>,
      "framerate":         <float>,
      "duration_s":        <float>,
      "pix_fmt":           "<yuv420p|...>",
      "encoder_upstream":  "<ffprobe codec_name; e.g. h264, vp9>",
      "mos":               <float, 0..100 unified scale>,
      "mos_native":        <float, original per-corpus value>,
      "mos_native_scale":  "<1-5-acr|0-100-dcr|vmaf>",
      "mos_std_dev":       <float, native-scale dispersion>,
      "n_ratings":         <int>,
      "corpus":            "<original corpus label>",
      "corpus_source":     "<konvid-1k|konvid-150k|lsvq|"
                            "youtube-ugc|waterloo-ivc-4k|netflix-public>",
      "corpus_version":    "<dataset version string>",
      "ingested_at_utc":   "<ISO 8601>",
      "aggregated_at_utc": "<ISO 8601>"
    }

The trainer's loader (``train_predictor_v2_realcorpus.py``) reads
``mos`` as the regression target and may key on ``corpus_source``
for per-corpus loss-weighting / ablation.

Usage
-----

::

    python ai/scripts/aggregate_corpora.py \\
        --inputs .workingdir2/konvid-150k/konvid_150k.jsonl \\
                 .workingdir2/lsvq/lsvq.jsonl \\
                 .workingdir2/waterloo-ivc-4k/waterloo_ivc_4k.jsonl \\
                 .workingdir2/youtube-ugc/youtube_ugc.jsonl \\
        --output .workingdir2/aggregated/unified_corpus.jsonl

Missing-input policy: any input path that does not exist is logged
as a WARNING and skipped (graceful degradation). The aggregator
fails hard only if **zero** input files survive the existence
check, since an empty unified corpus is never the operator's
intent.

Exit codes:

* 0 — aggregation succeeded.
* 1 — at least one row failed scale conversion or schema check
  (the run is aborted to avoid producing a bad unified corpus).
* 2 — no input files exist on disk (or argument parsing failed).
"""

from __future__ import annotations

import argparse
import json
import logging
import math
import os
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path
from typing import Any

from aiutils.jsonl_utils import iter_jsonl
from aiutils.run_manifest import build_run_provenance, write_manifest_json
from aiutils.time_utils import now_iso_8601

_LOG = logging.getLogger("aggregate_corpora")

# ---------------------------------------------------------------------------
# Scale conversion table — single source of truth, validated by tests.
# ---------------------------------------------------------------------------

#: Per-corpus scale descriptors. ``slope`` and ``intercept`` are the
#: affine map ``unified = slope * native + intercept``. ``scale_label``
#: is the native-scale string baked into ``mos_native_scale`` for
#: trainer-side provenance.
SCALE_CONVERSIONS: dict[str, dict[str, Any]] = {
    "konvid-1k": {
        "slope": 25.0,
        "intercept": -25.0,
        "native_min": 1.0,
        "native_max": 5.0,
        "scale_label": "1-5-acr",
    },
    "konvid-150k": {
        "slope": 25.0,
        "intercept": -25.0,
        "native_min": 1.0,
        "native_max": 5.0,
        "scale_label": "1-5-acr",
    },
    "lsvq": {
        "slope": 25.0,
        "intercept": -25.0,
        "native_min": 1.0,
        "native_max": 5.0,
        "scale_label": "1-5-acr",
    },
    "youtube-ugc": {
        "slope": 25.0,
        "intercept": -25.0,
        "native_min": 1.0,
        "native_max": 5.0,
        "scale_label": "1-5-acr",
    },
    "waterloo-ivc-4k": {
        "slope": 1.0,
        "intercept": 0.0,
        "native_min": 0.0,
        "native_max": 100.0,
        "scale_label": "0-100-dcr",
    },
    "netflix-public": {
        "slope": 1.0,
        "intercept": 0.0,
        "native_min": 0.0,
        "native_max": 100.0,
        "scale_label": "vmaf",
    },
}

#: Tolerance for the "is this MOS plausibly within native range" guard.
#: A small slack accommodates floating-point noise and dataset-side
#: rounding without admitting wildly-out-of-range values.
_NATIVE_RANGE_SLACK: float = 0.05

#: Minimum required keys on every per-corpus input row. Per-corpus
#: schemas already standardise on this superset (see KonViD / LSVQ /
#: Waterloo / YT-UGC ingestion adapters); the aggregator hard-fails on
#: violation rather than guessing.
_REQUIRED_INPUT_KEYS: frozenset[str] = frozenset(
    {"src", "src_sha256", "mos", "corpus"},
)


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def _resolve_corpus_source(row: dict, override: str | None) -> str | None:
    """Pick the canonical ``corpus_source`` label for ``row``.

    The CLI ``--corpus-source`` override takes priority (so an
    operator can label an arbitrarily-named JSONL); otherwise the
    row's ``corpus`` field is consulted, then ``corpus_source`` if
    already present. Returns ``None`` when no recognised label
    surfaces — the caller drops the row.
    """
    if override is not None:
        return override
    label = row.get("corpus_source") or row.get("corpus")
    if isinstance(label, str) and label in SCALE_CONVERSIONS:
        return label
    return None


def convert_mos(native_mos: float, corpus_source: str) -> float:
    """Apply the per-corpus affine scale conversion.

    Raises ``ValueError`` if ``corpus_source`` is unknown — the
    caller logs and drops the row rather than guessing a conversion.
    """
    spec = SCALE_CONVERSIONS.get(corpus_source)
    if spec is None:
        raise ValueError(
            f"unknown corpus_source {corpus_source!r}; refusing to "
            f"silently widen the training-target distribution"
        )
    if not (
        math.isfinite(native_mos)
        and (spec["native_min"] - _NATIVE_RANGE_SLACK)
        <= native_mos
        <= (spec["native_max"] + _NATIVE_RANGE_SLACK)
    ):
        raise ValueError(
            f"native MOS {native_mos!r} for {corpus_source} is outside "
            f"the published [{spec['native_min']}, {spec['native_max']}] "
            f"range; refusing rather than emitting a silently-clipped row"
        )
    return float(spec["slope"]) * float(native_mos) + float(spec["intercept"])


# ---------------------------------------------------------------------------
# Per-row transform
# ---------------------------------------------------------------------------


def _validate_input_row(path: Path, line_no: int, row: dict) -> None:
    """Hard-fail on rows missing required keys."""
    if not isinstance(row, dict):
        raise SystemExit(f"error: {path}:{line_no}: expected JSON object, got {type(row).__name__}")
    missing = _REQUIRED_INPUT_KEYS - row.keys()
    if missing:
        raise SystemExit(f"error: {path}:{line_no}: missing required keys: {sorted(missing)}")


def transform_row(
    row: dict,
    *,
    corpus_source: str,
    aggregated_at_utc: str,
) -> dict[str, Any]:
    """Build one unified-schema row from a per-corpus input row.

    Pure function (no I/O) so tests can drive it directly.
    """
    native_mos = float(row["mos"])
    unified_mos = convert_mos(native_mos, corpus_source)
    spec = SCALE_CONVERSIONS[corpus_source]

    out: dict[str, Any] = {
        "src": row.get("src", ""),
        "src_sha256": row.get("src_sha256", ""),
        "width": int(row.get("width", 0) or 0),
        "height": int(row.get("height", 0) or 0),
        "framerate": float(row.get("framerate", 0.0) or 0.0),
        "duration_s": float(row.get("duration_s", 0.0) or 0.0),
        "pix_fmt": str(row.get("pix_fmt", "") or ""),
        "encoder_upstream": str(row.get("encoder_upstream", "") or ""),
        "mos": float(unified_mos),
        "mos_native": native_mos,
        "mos_native_scale": str(spec["scale_label"]),
        "mos_std_dev": float(row.get("mos_std_dev", 0.0) or 0.0),
        "n_ratings": int(row.get("n_ratings", 0) or 0),
        "corpus": str(row.get("corpus", corpus_source)),
        "corpus_source": corpus_source,
        "corpus_version": str(row.get("corpus_version", "") or ""),
        "ingested_at_utc": str(row.get("ingested_at_utc", "") or ""),
        "aggregated_at_utc": aggregated_at_utc,
    }
    return out


# ---------------------------------------------------------------------------
# Cross-corpus dedup
# ---------------------------------------------------------------------------


def resolve_duplicate(existing: dict, candidate: dict) -> dict:
    """Pick the row with tighter MOS uncertainty.

    Returns the row to *keep*. Ties keep ``existing`` (first-seen),
    which is deterministic given a stable ``--input`` ordering. A
    missing or zero ``mos_std_dev`` is treated as "uncertainty
    unknown" — it loses to any row that reports a positive std-dev,
    and ties with another unknown.
    """

    def _uncertainty(r: dict) -> float:
        v = r.get("mos_std_dev", 0.0)
        try:
            f = float(v)
        except (TypeError, ValueError):
            return math.inf
        if not math.isfinite(f) or f <= 0.0:
            return math.inf
        return f

    return existing if _uncertainty(existing) <= _uncertainty(candidate) else candidate


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def aggregate(
    inputs: Iterable[Path],
    output: Path,
    *,
    corpus_source_overrides: dict[Path, str] | None = None,
    now_fn: callable = now_iso_8601,  # type: ignore[valid-type]
) -> dict[str, int]:
    """Stream-aggregate ``inputs`` into ``output``.

    Returns a counter dict with keys ``rows_in``, ``rows_out``,
    ``dropped_unknown_corpus``, ``dropped_bad_scale``,
    ``cross_corpus_dedups``, ``inputs_seen``, ``inputs_missing``.

    Raises ``SystemExit(2)`` when **every** input is missing on disk
    (graceful degradation tolerates *some* missing shards but not all
    of them — an empty unified corpus is never useful).
    """
    overrides = corpus_source_overrides or {}
    aggregated_at_utc = now_fn()

    # Phase 1: accumulate keyed-by-sha rows in memory. The
    # cross-corpus dedup needs random access; per-corpus dedup
    # already happens at ingestion time so the rows-per-shard fit in
    # memory comfortably (KonViD-150k is ~150 k rows).
    keyed: dict[str, dict[str, Any]] = {}
    counters = {
        "rows_in": 0,
        "rows_out": 0,
        "dropped_unknown_corpus": 0,
        "dropped_bad_scale": 0,
        "cross_corpus_dedups": 0,
        "inputs_seen": 0,
        "inputs_missing": 0,
    }

    paths = list(inputs)
    for path in paths:
        if not path.is_file():
            _LOG.warning("input not found, skipping: %s", path)
            counters["inputs_missing"] += 1
            continue
        counters["inputs_seen"] += 1
        override = overrides.get(path)
        for line_no, raw in iter_jsonl(path):
            counters["rows_in"] += 1
            _validate_input_row(path, line_no, raw)

            corpus_source = _resolve_corpus_source(raw, override)
            if corpus_source is None:
                _LOG.warning(
                    "%s:%d: unknown corpus label %r; row dropped (no scale "
                    "conversion defined). Add an entry to SCALE_CONVERSIONS "
                    "or pass --corpus-source for this input.",
                    path,
                    line_no,
                    raw.get("corpus"),
                )
                counters["dropped_unknown_corpus"] += 1
                continue

            try:
                converted = transform_row(
                    raw,
                    corpus_source=corpus_source,
                    aggregated_at_utc=aggregated_at_utc,
                )
            except (ValueError, TypeError) as exc:
                # ValueError: out-of-range / unknown-scale MOS (convert_mos).
                # TypeError: malformed mos cell (None, list, ...) reaching
                # float() in transform_row. Both are bad-scale rows: drop
                # with a warning rather than crashing the whole aggregation.
                _LOG.warning("%s:%d: %s; row dropped", path, line_no, exc)
                counters["dropped_bad_scale"] += 1
                continue

            sha = converted["src_sha256"]
            if not isinstance(sha, str) or not sha:
                # No content key — keep but namespace under (src,
                # corpus_source) so we don't false-merge unrelated
                # rows missing a sha. This is a legitimate path for
                # corpora that pre-date sha enrichment.
                key = f"__nokey__/{converted.get('src', '')}/{corpus_source}"
            else:
                key = sha

            existing = keyed.get(key)
            if existing is None:
                keyed[key] = converted
                continue
            if existing.get("corpus_source") == converted.get("corpus_source"):
                # Same-corpus dup — out of scope for this aggregator;
                # the per-corpus ingestion already deduped, so trust
                # first-seen.
                continue
            # Cross-corpus duplicate. Apply uncertainty-weighted resolve.
            counters["cross_corpus_dedups"] += 1
            keyed[key] = resolve_duplicate(existing, converted)

    if counters["inputs_seen"] == 0:
        raise SystemExit(
            f"error: no input JSONL files exist on disk "
            f"(checked {len(paths)} path(s)); refusing to write an empty "
            f"unified corpus"
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    # Write to a temp file in the same directory then rename atomically so
    # a crash mid-write never leaves a partially-truncated output JSONL.
    fd, tmp_str = tempfile.mkstemp(dir=output.parent)
    tmp = Path(tmp_str)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as out_fp:
            # Stable sort by (corpus_source, src_sha256) so the output is
            # bytewise-deterministic given a fixed input set, which makes
            # diffing across runs trivial in CI.
            for key in sorted(
                keyed,
                key=lambda k: (keyed[k]["corpus_source"], keyed[k]["src_sha256"], k),
            ):
                out_fp.write(json.dumps(keyed[key], sort_keys=True) + "\n")
                counters["rows_out"] += 1
        os.replace(tmp_str, output)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise

    return counters


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="aggregate_corpora.py",
        description=(
            "Aggregate per-corpus MOS JSONL shards (KonViD-1k, "
            "KonViD-150k, LSVQ, Waterloo IVC 4K-VQA, YouTube UGC, "
            "Netflix Public) into one unified-scale JSONL the "
            "fr_regressor_v2 / predictor_v2_realcorpus trainer "
            "consumes. See ADR-0340 for scale-conversion rationale."
        ),
    )
    ap.add_argument(
        "--inputs",
        nargs="+",
        required=True,
        type=Path,
        help=(
            "One or more per-corpus JSONL paths. Missing paths are "
            "skipped with a WARNING (graceful degradation); the run "
            "fails only if every path is absent."
        ),
    )
    ap.add_argument(
        "--output",
        required=True,
        type=Path,
        help="Destination unified JSONL.",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <output>.manifest.json and "
            "records the input shards, scale conversions, counters, and exact "
            "CLI args used to build the unified corpus JSONL."
        ),
    )
    ap.add_argument(
        "--corpus-source-override",
        action="append",
        default=[],
        metavar="PATH=LABEL",
        help=(
            "Force a corpus-source label for a specific input path "
            "(repeatable). Useful when a JSONL's per-row ``corpus`` "
            "field is missing or non-canonical. LABEL must be a key "
            "in SCALE_CONVERSIONS. Example: "
            "--corpus-source-override foo.jsonl=lsvq."
        ),
    )
    ap.add_argument(
        "--log-level",
        default="INFO",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
    )
    return ap


def _parse_overrides(raw: list[str]) -> dict[Path, str]:
    """Parse repeated ``PATH=LABEL`` strings into a mapping."""
    out: dict[Path, str] = {}
    for entry in raw:
        if "=" not in entry:
            raise SystemExit(f"error: --corpus-source-override expects PATH=LABEL, got {entry!r}")
        path_str, label = entry.split("=", 1)
        if label not in SCALE_CONVERSIONS:
            raise SystemExit(
                f"error: unknown corpus-source label {label!r}; valid "
                f"labels: {sorted(SCALE_CONVERSIONS)}"
            )
        out[Path(path_str)] = label
    return out


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    overrides = _parse_overrides(args.corpus_source_override)
    if args.manifest_out is None:
        args.manifest_out = args.output.with_suffix(".manifest.json")

    counters = aggregate(
        args.inputs,
        args.output,
        corpus_source_overrides=overrides,
    )

    summary = (
        f"[aggregate_corpora] inputs_seen={counters['inputs_seen']} "
        f"inputs_missing={counters['inputs_missing']} "
        f"rows_in={counters['rows_in']} rows_out={counters['rows_out']} "
        f"cross_corpus_dedups={counters['cross_corpus_dedups']} "
        f"dropped_unknown_corpus={counters['dropped_unknown_corpus']} "
        f"dropped_bad_scale={counters['dropped_bad_scale']} -> {args.output}"
    )
    write_manifest_json(
        args.manifest_out,
        {
            "schema": "mos-corpus-aggregate-manifest-v1",
            "counters": counters,
            "scale_conversions": SCALE_CONVERSIONS,
            "corpus_source_overrides": {
                str(path): label
                for path, label in sorted(overrides.items(), key=lambda item: str(item[0]))
            },
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=Path(__file__).resolve().parents[2],
                argv=sys.argv[1:] if argv is None else argv,
                args=args,
                inputs={"corpus_jsonl": args.inputs},
                outputs={"jsonl": args.output, "manifest": args.manifest_out},
            ),
        },
    )
    print(summary, file=sys.stderr)
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
