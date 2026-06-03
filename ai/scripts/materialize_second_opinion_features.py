#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Join NR/MOS second-opinion scores onto already-extracted feature tables.

The script is deliberately table-side: it does not invoke external VQA models
or vendor competitor code. Operators run fork/external scorers separately and
feed their JSON/JSONL outputs here so refreshed feature tables can carry
``second_opinion_*`` columns for retraining and signal-mix audits.
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

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

KEY_CANDIDATES: tuple[str, ...] = (
    "clip_id",
    "video_id",
    "chug_video_id",
    "content_id",
    "file_name",
    "filename",
    "basename",
    "name",
    "distorted_path",
    "dis_path",
    "path",
)

SCORE_CANDIDATES: tuple[str, ...] = (
    "score",
    "mos",
    "predicted_mos",
    "predicted_vmaf_or_mos",
    "quality",
    "vmaf",
)

RUNTIME_CANDIDATES: tuple[str, ...] = ("runtime_ms", "runtime_total_ms", "elapsed_ms")
STATUS_OK = "ok"
STATUS_MISSING = "missing"
STATUS_BAD = "bad"


@dataclass(frozen=True)
class ScoreRecord:
    competitor: str
    key: str
    score: float
    status: str
    runtime_ms: float
    frames: int
    source: str


@dataclass
class CompetitorStats:
    competitor: str
    column_prefix: str
    scores_loaded: int = 0
    matched_rows: int = 0
    missing_rows: int = 0
    bad_rows: int = 0


def _parse_labeled_path(value: str) -> tuple[str | None, Path]:
    if "=" in value and not Path(value).exists():
        label, raw_path = value.split("=", 1)
        return label.strip() or None, Path(raw_path)
    return None, Path(value)


def _slug(value: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "_", value.lower()).strip("_")
    if not slug:
        raise ValueError(f"cannot derive column slug from {value!r}")
    return slug


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
    raise ValueError(f"unsupported table format for {path}; use parquet, jsonl, ndjson, or json")


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
                {"rows": _json_safe(df.to_dict(orient="records"))}, indent=2, sort_keys=True
            ),
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


def _read_score_payloads(path: Path) -> list[dict[str, Any]]:
    suffix = path.suffix.lower()
    if suffix in (".jsonl", ".ndjson"):
        rows: list[dict[str, Any]] = []
        with path.open("r", encoding="utf-8") as f:
            for line_no, line in enumerate(f, start=1):
                stripped = line.strip()
                if not stripped:
                    continue
                payload = json.loads(stripped)
                if not isinstance(payload, dict):
                    raise ValueError(f"{path}:{line_no}: score row must be a JSON object")
                rows.append(payload)
        return rows
    if suffix == ".json":
        payload = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(payload, dict):
            rows = payload.get("rows") or payload.get("results")
            if isinstance(rows, list):
                return _ensure_dict_rows(rows, path)
            return [payload]
        if isinstance(payload, list):
            return _ensure_dict_rows(payload, path)
    raise ValueError(f"unsupported score format for {path}; use jsonl, ndjson, or json")


def _ensure_dict_rows(rows: list[Any], path: Path) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for idx, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f"{path}: rows[{idx}] must be a JSON object")
        out.append(row)
    return out


def _find_ci(mapping: dict[str, Any], candidates: tuple[str, ...]) -> Any | None:
    lowered = {str(k).lower(): k for k in mapping}
    for candidate in candidates:
        key = lowered.get(candidate.lower())
        if key is not None:
            return mapping[key]
    return None


def _as_float(value: Any, *, path: str) -> float:
    if isinstance(value, bool) or value is None:
        raise ValueError(f"{path} must be numeric")
    try:
        out = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{path} must be numeric") from exc
    if not math.isfinite(out):
        raise ValueError(f"{path} must be finite")
    return out


def _normalise_key(value: Any, *, mode: str, column_name: str = "") -> str:
    text = str(value).strip()
    if not text:
        return ""
    effective = mode
    if mode == "auto":
        lower = column_name.lower()
        effective = "basename" if any(x in lower for x in ("path", "file", "name")) else "raw"
    if effective == "raw":
        return text
    name = Path(text).name
    if effective == "basename":
        return name
    if effective == "stem":
        return Path(name).stem
    raise ValueError(f"unsupported key normalisation mode: {mode}")


def _infer_feature_key_column(df: pd.DataFrame, requested: str | None) -> str:
    if requested:
        if requested not in df.columns:
            raise ValueError(f"--key-column {requested!r} is not present in feature table")
        return requested
    lowered = {str(c).lower(): str(c) for c in df.columns}
    for candidate in KEY_CANDIDATES:
        found = lowered.get(candidate.lower())
        if found is not None:
            return found
    raise ValueError(
        "could not infer feature key column; pass --key-column. "
        f"Tried: {', '.join(KEY_CANDIDATES)}"
    )


def _extract_wrapper_score(payload: dict[str, Any], score_field: str | None) -> tuple[float, int]:
    if score_field and score_field in payload:
        return _as_float(payload[score_field], path=score_field), 0
    value = _find_ci(payload, SCORE_CANDIDATES)
    if value is not None:
        return _as_float(value, path="score"), 0
    summary = payload.get("summary")
    if isinstance(summary, dict):
        value = _find_ci(summary, SCORE_CANDIDATES)
        if value is not None:
            return _as_float(value, path="summary score"), 0
    frames = payload.get("frames")
    if not isinstance(frames, list) or not frames:
        raise ValueError("score row has neither a score field nor non-empty frames[]")
    values: list[float] = []
    for idx, frame in enumerate(frames):
        if not isinstance(frame, dict):
            raise ValueError(f"frames[{idx}] must be an object")
        value = _find_ci(frame, SCORE_CANDIDATES)
        if value is None:
            raise ValueError(f"frames[{idx}] has no score-like field")
        values.append(_as_float(value, path=f"frames[{idx}] score"))
    return sum(values) / len(values), len(values)


def _extract_runtime(payload: dict[str, Any]) -> float:
    value = _find_ci(payload, RUNTIME_CANDIDATES)
    if value is not None:
        return _as_float(value, path="runtime")
    summary = payload.get("summary")
    if isinstance(summary, dict):
        value = _find_ci(summary, RUNTIME_CANDIDATES)
        if value is not None:
            return _as_float(value, path="summary runtime")
    frames = payload.get("frames")
    if isinstance(frames, list):
        total = 0.0
        seen = False
        for frame in frames:
            if isinstance(frame, dict):
                value = _find_ci(frame, RUNTIME_CANDIDATES)
                if value is not None:
                    total += _as_float(value, path="frame runtime")
                    seen = True
        if seen:
            return total
    return float("nan")


def _payload_competitor(payload: dict[str, Any], hint: str | None) -> str:
    value = payload.get("competitor")
    if isinstance(value, str) and value.strip():
        return value.strip()
    summary = payload.get("summary")
    if isinstance(summary, dict):
        value = summary.get("competitor")
        if isinstance(value, str) and value.strip():
            return value.strip()
    if hint:
        return hint
    raise ValueError("score row has no competitor field; use LABEL=path for --scores")


def _payload_key(
    payload: dict[str, Any],
    *,
    score_key_field: str | None,
    key_normalize: str,
) -> str:
    if score_key_field:
        if score_key_field not in payload:
            raise ValueError(f"score row lacks requested key field {score_key_field!r}")
        return _normalise_key(
            payload[score_key_field],
            mode=key_normalize,
            column_name=score_key_field,
        )
    value = _find_ci(payload, KEY_CANDIDATES)
    if value is not None:
        # Recover the actual key name for auto path handling.
        actual = next((str(k) for k, v in payload.items() if v is value), "")
        return _normalise_key(value, mode=key_normalize, column_name=actual)
    summary = payload.get("summary")
    if isinstance(summary, dict):
        value = _find_ci(summary, KEY_CANDIDATES)
        if value is not None:
            actual = next((str(k) for k, v in summary.items() if v is value), "")
            return _normalise_key(value, mode=key_normalize, column_name=actual)
    raise ValueError("score row has no key field; pass --score-key-field")


def _score_record(
    payload: dict[str, Any],
    *,
    competitor_hint: str | None,
    score_key_field: str | None,
    score_field: str | None,
    key_normalize: str,
    source: Path,
) -> ScoreRecord:
    competitor = _payload_competitor(payload, competitor_hint)
    key = _payload_key(
        payload,
        score_key_field=score_key_field,
        key_normalize=key_normalize,
    )
    if not key:
        raise ValueError(f"{source}: empty score key")
    score, frame_count = _extract_wrapper_score(payload, score_field)
    status_value = payload.get("status", STATUS_OK)
    status = str(status_value).strip().lower() if status_value is not None else STATUS_OK
    if status in ("", "pass", "success"):
        status = STATUS_OK
    if status != STATUS_OK:
        status = STATUS_BAD
    return ScoreRecord(
        competitor=competitor,
        key=key,
        score=score,
        status=status,
        runtime_ms=_extract_runtime(payload),
        frames=frame_count,
        source=str(source),
    )


def _load_scores(
    specs: list[str],
    *,
    score_key_field: str | None,
    score_field: str | None,
    key_normalize: str,
) -> dict[str, dict[str, ScoreRecord]]:
    scores: dict[str, dict[str, ScoreRecord]] = {}
    for spec in specs:
        hint, path = _parse_labeled_path(spec)
        for payload in _read_score_payloads(path):
            record = _score_record(
                payload,
                competitor_hint=hint,
                score_key_field=score_key_field,
                score_field=score_field,
                key_normalize=key_normalize,
                source=path,
            )
            by_key = scores.setdefault(record.competitor, {})
            if record.key in by_key:
                raise ValueError(
                    f"duplicate score for competitor={record.competitor!r} key={record.key!r}"
                )
            by_key[record.key] = record
    if not scores:
        raise ValueError("no scores loaded")
    return scores


def materialize(
    features: Path,
    score_specs: list[str],
    out: Path,
    *,
    audit_json: Path | None = None,
    key_column: str | None = None,
    score_key_field: str | None = None,
    score_field: str | None = None,
    key_normalize: str = "auto",
    missing_policy: str = "mark",
    prefix: str = "second_opinion",
    overwrite: bool = False,
    run_provenance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    df = _read_table(features)
    column = _infer_feature_key_column(df, key_column)
    records = _load_scores(
        score_specs,
        score_key_field=score_key_field,
        score_field=score_field,
        key_normalize=key_normalize,
    )

    keys = [
        _normalise_key(value, mode=key_normalize, column_name=column)
        for value in df[column].tolist()
    ]
    keep_mask = [True] * len(df)
    stats: dict[str, CompetitorStats] = {}

    for competitor, by_key in sorted(records.items()):
        slug = _slug(competitor)
        col_base = f"{prefix}_{slug}"
        stats[competitor] = CompetitorStats(
            competitor=competitor,
            column_prefix=col_base,
            scores_loaded=len(by_key),
        )
        planned = [
            f"{col_base}_score",
            f"{col_base}_status",
            f"{col_base}_runtime_ms",
            f"{col_base}_frames",
            f"{col_base}_source",
        ]
        existing = [c for c in planned if c in df.columns]
        if existing and not overwrite:
            raise ValueError(
                "refusing to overwrite existing second-opinion columns: " + ", ".join(existing)
            )

        scores: list[float] = []
        statuses: list[str] = []
        runtimes: list[float] = []
        frame_counts: list[int] = []
        sources: list[str] = []
        for idx, key in enumerate(keys):
            record = by_key.get(key)
            if record is None:
                stats[competitor].missing_rows += 1
                scores.append(float("nan"))
                statuses.append(STATUS_MISSING)
                runtimes.append(float("nan"))
                frame_counts.append(0)
                sources.append("")
                if missing_policy == "drop":
                    keep_mask[idx] = False
                continue
            if record.status == STATUS_OK:
                stats[competitor].matched_rows += 1
            else:
                stats[competitor].bad_rows += 1
            scores.append(record.score)
            statuses.append(record.status)
            runtimes.append(record.runtime_ms)
            frame_counts.append(record.frames)
            sources.append(record.source)

        df[f"{col_base}_score"] = scores
        df[f"{col_base}_status"] = statuses
        df[f"{col_base}_runtime_ms"] = runtimes
        df[f"{col_base}_frames"] = frame_counts
        df[f"{col_base}_source"] = sources

    if missing_policy == "fail":
        missing = [s for s in stats.values() if s.missing_rows]
        if missing:
            summary = ", ".join(f"{s.competitor}={s.missing_rows}" for s in missing)
            raise ValueError(f"missing second-opinion scores: {summary}")
    if missing_policy == "drop":
        df = df.loc[keep_mask].reset_index(drop=True)

    _write_table(df, out)
    audit = {
        "feature_table": str(features),
        "output": str(out),
        "key_column": column,
        "key_normalize": key_normalize,
        "input_rows": len(keys),
        "output_rows": len(df),
        "missing_policy": missing_policy,
        "competitors": {name: asdict(stat) for name, stat in sorted(stats.items())},
    }
    if run_provenance is not None:
        audit["run_provenance"] = run_provenance
    if audit_json is not None:
        write_manifest_json(audit_json, audit)
    return audit


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = make_argument_parser(
        prog="materialize_second_opinion_features.py",
        description=(
            "Join externally generated NR/MOS second-opinion scores onto a "
            "feature table without invoking competitor binaries."
        ),
    )
    parser.add_argument("--features", required=True, type=Path, help="input feature table")
    parser.add_argument("--scores", required=True, action="append", help="[LABEL=]score JSON/JSONL")
    parser.add_argument("--out", required=True, type=Path, help="output feature table")
    parser.add_argument("--audit-json", type=Path, default=None, help="optional audit JSON output")
    parser.add_argument("--key-column", default=None, help="feature-table key column")
    parser.add_argument("--score-key-field", default=None, help="score-row key field")
    parser.add_argument("--score-field", default=None, help="score-row scalar field")
    parser.add_argument(
        "--key-normalize",
        default="auto",
        choices=("auto", "raw", "basename", "stem"),
        help="how keys are normalised before joining",
    )
    parser.add_argument(
        "--missing-policy",
        default="mark",
        choices=("mark", "drop", "fail"),
        help="what to do when feature rows lack a score",
    )
    parser.add_argument("--prefix", default="second_opinion", help="output column prefix")
    parser.add_argument("--overwrite", action="store_true", help="replace existing output columns")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _parse_args(raw_argv)
    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"features": args.features, "scores": list(args.scores)},
        outputs={
            "out": str(args.out),
            "audit_json": str(args.audit_json) if args.audit_json is not None else None,
        },
    )
    materialize(
        args.features,
        list(args.scores),
        args.out,
        audit_json=args.audit_json,
        key_column=args.key_column,
        score_key_field=args.score_key_field,
        score_field=args.score_field,
        key_normalize=args.key_normalize,
        missing_policy=args.missing_policy,
        prefix=args.prefix,
        overwrite=args.overwrite,
        run_provenance=run_provenance,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
