#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Join subjective MOS labels onto already-extracted feature tables.

The MOS-head trainers consume feature tables that already carry a ``mos`` or
``mos_raw_0_100`` column. This script keeps that labelling step explicit: it
does not extract features, download corpora, or train a model. It reads an
existing feature table, reads one or more label tables, joins by a stable clip
key, and writes an enriched feature table plus an optional audit JSON.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import pandas as pd
from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

MOS_MIN = 1.0
MOS_MAX = 5.0
MOS_RAW_MIN = 0.0
MOS_RAW_MAX = 100.0

KEY_CANDIDATES: tuple[str, ...] = (
    "key",
    "clip_key",
    "clip_id",
    "video_id",
    "chug_video_id",
    "content_id",
    "clip_name",
    "file_name",
    "filename",
    "basename",
    "name",
    "src",
    "source",
    "src_path",
    "distorted_path",
    "dis_path",
    "path",
)

MOS_CANDIDATES: tuple[str, ...] = (
    "mos",
    "MOS",
    "mean_mos",
    "mos_raw_0_100",
    "raw_mos",
    "score",
    "quality",
)

COPY_LABEL_CANDIDATES: tuple[str, ...] = (
    "mos_std_dev",
    "stddev",
    "std_dev",
    "n_ratings",
    "mos_n_ratings",
    "rating_count",
    "split",
    "corpus",
    "corpus_version",
)

STATUS_OK = "ok"
STATUS_MISSING = "missing-label"


@dataclass(frozen=True)
class MosLabel:
    key: str
    mos: float
    mos_raw_0_100: float
    extras: dict[str, Any]
    source: str


@dataclass(frozen=True)
class MaterializeStats:
    feature_table: str
    output: str
    feature_key_column: str
    label_key_column: str
    label_mos_column: str
    key_normalize: str
    feature_key_regex: str
    label_key_regex: str
    min_match_rate: float
    input_rows: int
    output_rows: int
    total_feature_keys: int
    matched_feature_keys: int
    matched_rows: int
    missing_rows: int
    labels_loaded: int
    label_sources: list[str]

    @property
    def match_rate(self) -> float:
        if self.total_feature_keys == 0:
            return 0.0
        return self.matched_feature_keys / self.total_feature_keys


def _read_table(path: Path) -> pd.DataFrame:
    suffix = path.suffix.lower()
    if suffix in (".parquet", ".pq"):
        return pd.read_parquet(path)
    if suffix in (".jsonl", ".ndjson"):
        return pd.read_json(path, lines=True)
    if suffix == ".json":
        payload = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(payload, list):
            return pd.DataFrame(payload)
        if isinstance(payload, dict) and isinstance(payload.get("rows"), list):
            return pd.DataFrame(payload["rows"])
        raise ValueError(f"{path} is JSON but not a list or {{'rows': [...]}} object")
    if suffix == ".csv":
        return pd.read_csv(path)
    raise ValueError(f"unsupported table format for {path}; use parquet, jsonl, json, or csv")


def _write_table(df: pd.DataFrame, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    suffix = path.suffix.lower()
    if suffix in (".parquet", ".pq"):
        df.to_parquet(path, index=False)
        return
    if suffix in (".jsonl", ".ndjson"):
        with path.open("w", encoding="utf-8") as f:
            for row in df.to_dict(orient="records"):
                f.write(json.dumps(_json_safe(row), sort_keys=True) + "\n")
        return
    if suffix == ".json":
        path.write_text(
            json.dumps(
                {"rows": _json_safe(df.to_dict(orient="records"))},
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        return
    raise ValueError(f"unsupported output format for {path}; use parquet, jsonl, ndjson, or json")


def _json_safe(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(k): _json_safe(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_json_safe(v) for v in value]
    if pd.isna(value):
        return None
    if hasattr(value, "item"):
        try:
            return value.item()
        except ValueError:
            return value
    return value


def _find_column(
    df: pd.DataFrame, requested: str | None, candidates: tuple[str, ...], flag: str
) -> str:
    if requested:
        if requested not in df.columns:
            raise ValueError(f"{flag} {requested!r} is not present")
        return requested
    lowered = {str(c).lower(): str(c) for c in df.columns}
    for candidate in candidates:
        found = lowered.get(candidate.lower())
        if found is not None:
            return found
    raise ValueError(f"could not infer {flag}; tried: {', '.join(candidates)}")


def _as_float(value: Any, *, field: str) -> float:
    if value is None or isinstance(value, bool):
        raise ValueError(f"{field} must be numeric")
    try:
        out = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{field} must be numeric") from exc
    if not math.isfinite(out):
        raise ValueError(f"{field} must be finite")
    return out


def _normalise_key(
    value: Any,
    *,
    mode: str,
    column_name: str,
    regex: str | None,
) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return ""
    text = str(value).strip()
    if not text:
        return ""
    effective = mode
    if mode == "auto":
        lower = column_name.lower()
        effective = (
            "basename" if any(x in lower for x in ("path", "file", "name", "src")) else "raw"
        )
    if effective == "basename":
        text = Path(text).name
    elif effective == "stem":
        text = Path(text).stem
    elif effective != "raw":
        raise ValueError(f"unsupported key normalisation mode: {mode}")
    if regex:
        match = re.search(regex, text)
        if match is None:
            return ""
        text = match.group(1) if match.groups() else match.group(0)
    return text.strip()


def _mos_payload(value: Any, *, column: str) -> tuple[float, float]:
    raw = _as_float(value, field=column)
    if column.lower() == "mos_raw_0_100" or raw > MOS_MAX:
        if not (MOS_RAW_MIN <= raw <= MOS_RAW_MAX):
            raise ValueError(f"{column} must be in [0, 100]")
        mos = MOS_MIN + (MOS_MAX - MOS_MIN) * (raw / MOS_RAW_MAX)
        return mos, raw
    if not (MOS_MIN <= raw <= MOS_MAX):
        raise ValueError(f"{column} must be in [1, 5]")
    raw_0_100 = (raw - MOS_MIN) * (MOS_RAW_MAX / (MOS_MAX - MOS_MIN))
    return raw, raw_0_100


def _normalise_extra_name(name: str) -> str:
    if name in {"stddev", "std_dev"}:
        return "mos_std_dev"
    if name in {"n_ratings", "rating_count"}:
        return "mos_n_ratings"
    return name


def _label_from_row(
    row: dict[str, Any],
    *,
    key_column: str,
    mos_column: str,
    key_normalize: str,
    key_regex: str | None,
    source: Path,
) -> MosLabel | None:
    key = _normalise_key(
        row.get(key_column),
        mode=key_normalize,
        column_name=key_column,
        regex=key_regex,
    )
    if not key:
        return None
    mos, raw = _mos_payload(row.get(mos_column), column=mos_column)
    extras: dict[str, Any] = {}
    for candidate in COPY_LABEL_CANDIDATES:
        if candidate not in row:
            continue
        value = row[candidate]
        if value is None or (isinstance(value, float) and math.isnan(value)):
            continue
        extras[_normalise_extra_name(candidate)] = value
    return MosLabel(
        key=key,
        mos=mos,
        mos_raw_0_100=raw,
        extras=extras,
        source=str(source),
    )


def _labels_equal(a: MosLabel, b: MosLabel) -> bool:
    return (
        abs(a.mos - b.mos) <= 1e-6
        and abs(a.mos_raw_0_100 - b.mos_raw_0_100) <= 1e-4
        and a.extras == b.extras
    )


def _load_labels(
    paths: list[Path],
    *,
    key_column: str | None,
    mos_column: str | None,
    key_normalize: str,
    key_regex: str | None,
) -> tuple[dict[str, MosLabel], str, str]:
    labels: dict[str, MosLabel] = {}
    inferred_key = ""
    inferred_mos = ""
    for path in paths:
        df = _read_table(path)
        current_key = _find_column(df, key_column, KEY_CANDIDATES, "--label-key-column")
        current_mos = _find_column(df, mos_column, MOS_CANDIDATES, "--label-mos-column")
        if not inferred_key:
            inferred_key = current_key
        if not inferred_mos:
            inferred_mos = current_mos
        for row in df.to_dict(orient="records"):
            label = _label_from_row(
                row,
                key_column=current_key,
                mos_column=current_mos,
                key_normalize=key_normalize,
                key_regex=key_regex,
                source=path,
            )
            if label is None:
                continue
            previous = labels.get(label.key)
            if previous is not None:
                if _labels_equal(previous, label):
                    continue
                raise ValueError(f"conflicting MOS labels for key {label.key!r}")
            labels[label.key] = label
    if not labels:
        raise ValueError("no MOS labels loaded")
    return labels, inferred_key, inferred_mos


def _planned_columns(labels: dict[str, MosLabel], status_column: str) -> list[str]:
    extras = sorted({key for label in labels.values() for key in label.extras})
    return ["mos", "mos_raw_0_100", status_column, "mos_label_source", *extras]


def _existing_target_columns(df: pd.DataFrame, planned: list[str]) -> list[str]:
    return [column for column in planned if column in df.columns]


def materialize(
    features: Path,
    label_paths: list[Path],
    out: Path,
    *,
    audit_json: Path | None = None,
    feature_key_column: str | None = None,
    label_key_column: str | None = None,
    label_mos_column: str | None = None,
    key_normalize: str = "auto",
    feature_key_regex: str | None = None,
    label_key_regex: str | None = None,
    min_match_rate: float = 0.95,
    status_column: str = "mos_label_status",
    overwrite: bool = False,
    run_provenance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Join MOS labels onto ``features`` and return an audit dictionary."""
    if not (0.0 <= min_match_rate <= 1.0):
        raise ValueError("--min-match-rate must be in [0, 1]")
    df = _read_table(features)
    feature_column = _find_column(df, feature_key_column, KEY_CANDIDATES, "--feature-key-column")
    labels, inferred_label_key, inferred_label_mos = _load_labels(
        label_paths,
        key_column=label_key_column,
        mos_column=label_mos_column,
        key_normalize=key_normalize,
        key_regex=label_key_regex,
    )
    planned = _planned_columns(labels, status_column)
    existing = _existing_target_columns(df, planned)
    if existing and not overwrite:
        raise ValueError("refusing to overwrite existing MOS columns: " + ", ".join(existing))

    row_keys = [
        _normalise_key(
            value,
            mode=key_normalize,
            column_name=feature_column,
            regex=feature_key_regex,
        )
        for value in df[feature_column].tolist()
    ]
    unique_feature_keys = {key for key in row_keys if key}
    matched_feature_keys = unique_feature_keys & set(labels)
    match_rate = (
        len(matched_feature_keys) / len(unique_feature_keys) if unique_feature_keys else 0.0
    )
    if match_rate < min_match_rate:
        raise ValueError(
            f"MOS label match rate {match_rate:.3f} below --min-match-rate "
            f"{min_match_rate:.3f} ({len(matched_feature_keys)}/{len(unique_feature_keys)} keys)"
        )

    mos_values: list[float] = []
    raw_values: list[float] = []
    statuses: list[str] = []
    sources: list[str] = []
    extras_by_column: dict[str, list[Any]] = {
        column: []
        for column in planned
        if column not in {"mos", "mos_raw_0_100", status_column, "mos_label_source"}
    }
    matched_rows = 0
    missing_rows = 0
    for key in row_keys:
        label = labels.get(key)
        if label is None:
            mos_values.append(float("nan"))
            raw_values.append(float("nan"))
            statuses.append(STATUS_MISSING)
            sources.append("")
            for values in extras_by_column.values():
                values.append(None)
            missing_rows += 1
            continue
        mos_values.append(label.mos)
        raw_values.append(label.mos_raw_0_100)
        statuses.append(STATUS_OK)
        sources.append(label.source)
        for column, values in extras_by_column.items():
            values.append(label.extras.get(column))
        matched_rows += 1

    df["mos"] = mos_values
    df["mos_raw_0_100"] = raw_values
    df[status_column] = statuses
    df["mos_label_source"] = sources
    for column, values in extras_by_column.items():
        df[column] = values

    _write_table(df, out)
    stats = MaterializeStats(
        feature_table=str(features),
        output=str(out),
        feature_key_column=feature_column,
        label_key_column=inferred_label_key,
        label_mos_column=inferred_label_mos,
        key_normalize=key_normalize,
        feature_key_regex=feature_key_regex or "",
        label_key_regex=label_key_regex or "",
        min_match_rate=min_match_rate,
        input_rows=len(row_keys),
        output_rows=len(df),
        total_feature_keys=len(unique_feature_keys),
        matched_feature_keys=len(matched_feature_keys),
        matched_rows=matched_rows,
        missing_rows=missing_rows,
        labels_loaded=len(labels),
        label_sources=[str(path) for path in label_paths],
    )
    audit = asdict(stats)
    audit["match_rate"] = stats.match_rate
    if run_provenance is not None:
        audit["run_provenance"] = run_provenance
    if audit_json is not None:
        write_manifest_json(audit_json, audit)
    return audit


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = make_argument_parser(
        prog="materialize_mos_labels.py",
        description=(
            "Join subjective MOS labels onto an already-extracted feature table. "
            "Use this before real MOS-head training; use trainer --smoke for "
            "synthetic-only CI checks."
        ),
    )
    parser.add_argument("--features", required=True, type=Path, help="input feature table")
    parser.add_argument("--labels", required=True, action="append", type=Path, help="label table")
    parser.add_argument("--out", required=True, type=Path, help="output feature table")
    parser.add_argument("--audit-json", type=Path, default=None, help="optional audit JSON output")
    parser.add_argument("--feature-key-column", default=None, help="feature-table key column")
    parser.add_argument("--label-key-column", default=None, help="label-table key column")
    parser.add_argument("--label-mos-column", default=None, help="label-table MOS column")
    parser.add_argument(
        "--key-normalize",
        default="auto",
        choices=("auto", "raw", "basename", "stem"),
        help="how keys are normalised before regex extraction and joining",
    )
    parser.add_argument(
        "--feature-key-regex",
        default=None,
        help="regex applied to feature keys after normalisation; first group wins",
    )
    parser.add_argument(
        "--label-key-regex",
        default=None,
        help="regex applied to label keys after normalisation; first group wins",
    )
    parser.add_argument(
        "--min-match-rate",
        type=float,
        default=0.95,
        help="minimum unique-key match rate required before writing output",
    )
    parser.add_argument(
        "--status-column",
        default="mos_label_status",
        help="output status column for ok/missing-label rows",
    )
    parser.add_argument("--overwrite", action="store_true", help="replace existing MOS columns")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _parse_args(raw_argv)
    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"features": args.features, "labels": args.labels},
        outputs={
            "out": str(args.out),
            "audit_json": str(args.audit_json) if args.audit_json is not None else None,
        },
    )
    materialize(
        args.features,
        list(args.labels),
        args.out,
        audit_json=args.audit_json,
        feature_key_column=args.feature_key_column,
        label_key_column=args.label_key_column,
        label_mos_column=args.label_mos_column,
        key_normalize=args.key_normalize,
        feature_key_regex=args.feature_key_regex,
        label_key_regex=args.label_key_regex,
        min_match_rate=args.min_match_rate,
        status_column=args.status_column,
        overwrite=args.overwrite,
        run_provenance=run_provenance,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
