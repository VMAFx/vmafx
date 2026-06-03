#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Audit VQA signal coverage, redundancy, and missing metric families.

This tool is deliberately analysis-only: it reads already-extracted parquet or
JSONL tables, classifies columns into human-facing signal families, and reports
where the current feature mix is strong, redundant, complementary, or blind.
It does not extract features, train models, or mutate corpus files.
"""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

TARGET_CANDIDATES: tuple[str, ...] = (
    "vmaf",
    "vmaf_score",
    "mos_raw_0_100",
    "mos",
    "dmos",
    "score",
)

METADATA_EXCLUDE_RE = re.compile(
    r"(^src$|sha|basename|path|filename|uuid|id$|timestamp|ingested|manifest|"
    r"license|url|bucket|split|fold|corpus_version)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class SignalFamily:
    key: str
    label: str
    question: str
    patterns: tuple[str, ...]
    candidate_metrics: tuple[str, ...]
    recommendation: str

    def matches(self, column: str) -> bool:
        return any(re.search(pattern, column, re.IGNORECASE) for pattern in self.patterns)


SIGNAL_FAMILIES: tuple[SignalFamily, ...] = (
    SignalFamily(
        key="canonical_fr",
        label="FR detail and motion baseline",
        question="How close is the distorted clip to a reference on the Netflix VMAF core?",
        patterns=(
            r"^adm2(_(mean|p10|p90|std))?$",
            r"^adm_scale[0-3](_(mean|p10|p90|std))?$",
            r"^vif_scale[0-3](_(mean|p10|p90|std))?$",
            r"^motion[23]?(_(mean|p10|p90|std))?$",
            r"vmaf",
        ),
        candidate_metrics=("Netflix HDR VMAF", "VMAF NEG"),
        recommendation="Keep as the compatibility anchor; look for additive gains, not replacements.",
    ),
    SignalFamily(
        key="error_energy",
        label="Error energy and HVS-weighted PSNR",
        question="How large is the pixel-plane or HVS-weighted distortion?",
        patterns=(r"psnr", r"mse", r"rmse", r"error_energy"),
        candidate_metrics=("WS-PSNR", "xPSNR"),
        recommendation="Use as a cheap diagnostic axis and as a redundancy check against richer metrics.",
    ),
    SignalFamily(
        key="structural_similarity",
        label="Local structural similarity",
        question="Are local luminance/contrast/structure relationships preserved?",
        patterns=(r"(^|_)ssim($|_)", r"ms_ssim", r"msssim", r"iw_ssim"),
        candidate_metrics=("IW-SSIM", "FSIM", "GMSD"),
        recommendation="Intersect with texture metrics; plain SSIM often saturates on modern encodes.",
    ),
    SignalFamily(
        key="texture_deep_perceptual",
        label="Texture and deep perceptual similarity",
        question="Does the feature stack see texture loss, hallucinated detail, and learned FR artifacts?",
        patterns=(r"ssimulacra2", r"lpips", r"dists", r"deep", r"texture", r"vgg"),
        candidate_metrics=("DISTS", "LPIPS", "PieAPP", "ST-LPIPS"),
        recommendation="Prioritise candidate metrics here when canonical-6 gains flatten.",
    ),
    SignalFamily(
        key="color_chroma",
        label="Color and chroma fidelity",
        question="Does the stack see chroma shifts, color-difference errors, and subsampling damage?",
        patterns=(r"ciede", r"deltae", r"chroma", r"_cb$", r"_cr$", r"color", r"primaries"),
        candidate_metrics=("CIEDE2000", "HDR-VDP color terms", "ICtCp delta"),
        recommendation="Pair with HDR metadata; chroma-only gains are easy to miss in luma-heavy models.",
    ),
    SignalFamily(
        key="banding_tone_mapping",
        label="Banding and tone-mapping artifacts",
        question="Does the stack see banding, quantisation steps, PQ/HLG tone-map damage, or clipping?",
        patterns=(
            r"cambi",
            r"band",
            r"pq",
            r"hlg",
            r"tone",
            r"transfer",
            r"maxcll",
            r"maxfall",
            r"clip",
        ),
        candidate_metrics=("HDR-VDP-3", "PU-PSNR", "PU-SSIM", "VMAF HDR"),
        recommendation="Treat missing or flat rows here as HDR blockers even if SDR VMAF is strong.",
    ),
    SignalFamily(
        key="temporal_flicker",
        label="Temporal instability, flicker, and scene structure",
        question="Does the stack see motion, exposure flicker, shot cuts, and temporal pooling risks?",
        patterns=(
            r"motion",
            r"speed_temporal",
            r"flicker",
            r"exposure",
            r"shot",
            r"transnet",
            r"scene",
            r"_p10$",
            r"_p90$",
            r"_std$",
        ),
        candidate_metrics=("ST-RRED", "SpEED-QA", "FovVideoVDP temporal terms"),
        recommendation="Use with per-scene pooling; a high frame-average can hide temporal failures.",
    ),
    SignalFamily(
        key="saliency_roi",
        label="Saliency and ROI weighting",
        question="Does the stack know which pixels humans are likely to look at?",
        patterns=(r"saliency", r"mobilesal", r"u2net", r"roi", r"attention", r"mask"),
        candidate_metrics=("U2NetP", "MobileSal", "VMAF ROI sidecars", "SAM-derived ROI"),
        recommendation="High-value intersection with encoder profiles: spend bits where saliency is high.",
    ),
    SignalFamily(
        key="hdr_display_panel",
        label="HDR display and panel context",
        question="Does the stack know bit depth, EOTF, luminance, mastering data, and panel limits?",
        patterns=(
            r"hdr",
            r"bitdepth",
            r"pix_fmt",
            r"eotf",
            r"bt2020",
            r"bt2100",
            r"luminance",
            r"nit",
            r"panel",
            r"display",
            r"mastering",
            r"peak",
        ),
        candidate_metrics=(
            "panel calibration metadata",
            "HDR10+ metadata",
            "Dolby Vision metadata",
        ),
        recommendation="Do not promote HDR guidance without this axis; CHUG MOS alone is not panel-aware.",
    ),
    SignalFamily(
        key="source_geometry_content",
        label="Source geometry and content metadata",
        question="Does the table know resolution, duration, frame rate, orientation, and content class?",
        patterns=(
            r"(^|_)width$",
            r"(^|_)height$",
            r"resolution",
            r"duration",
            r"framerate",
            r"frame_count",
            r"orientation",
            r"category",
            r"content",
        ),
        candidate_metrics=(
            "SI/TI complexity",
            "content-class labels",
            "camera/device metadata",
        ),
        recommendation="Keep as conditioning metadata; do not confuse it with perceptual evidence.",
    ),
    SignalFamily(
        key="codec_encoder",
        label="Codec, rate-control, and hardware context",
        question="Does the model know which encoder produced the artifact and at what operating point?",
        patterns=(
            r"codec",
            r"encoder",
            r"preset",
            r"tune",
            r"crf",
            r"qp",
            r"bitrate",
            r"rc_mode",
            r"qsv",
            r"nvenc",
            r"amf",
            r"svt",
            r"x26[45]",
        ),
        candidate_metrics=("encoder delay/B-frame structure", "AQ/psy knobs", "VBV pressure"),
        recommendation="Keep separate from perceptual signals so profiles can explain codec-specific gains.",
    ),
    SignalFamily(
        key="no_reference_ugc",
        label="No-reference UGC and subjective MOS context",
        question="Does the stack learn viewer-rated quality without a clean reference?",
        patterns=(
            r"mos",
            r"dmos",
            r"n_ratings",
            r"std_dev",
            r"ugc",
            r"content",
            r"category",
            r"orientation",
            r"brisque",
            r"niqe",
            r"piqe",
            r"nima",
            r"second_opinion",
            r"dover",
            r"q[_-]?align",
            r"fast[_-]?vqa",
            r"musiq",
            r"clip[_-]?iqa",
            r"maniqa",
        ),
        candidate_metrics=("DOVER", "Q-Align", "FAST-VQA", "MUSIQ", "CLIP-IQA", "MANIQA", "NIQE"),
        recommendation="Use as a second opinion against FR metrics; it is the bridge to user-facing MOS.",
    ),
    SignalFamily(
        key="noise_grain_blur",
        label="Noise, grain, blur, and sharpening",
        question="Does the stack separate artistic grain from destructive noise, blur, or oversharpening?",
        patterns=(r"noise", r"grain", r"blur", r"sharp", r"edge", r"laplacian", r"denoise"),
        candidate_metrics=("VMAF NEG", "VIDEVAL noise features", "BRISQUE naturalness"),
        recommendation="Add explicit columns before changing grain or denoise defaults.",
    ),
)


@dataclass
class TableAudit:
    label: str
    path: Path
    rows: int
    columns: list[str]
    target: str | None
    numeric_columns: list[str]
    signal_columns: list[str]
    family_columns: dict[str, list[str]]
    family_health: dict[str, dict[str, Any]]
    column_stats: dict[str, dict[str, float]]
    target_correlations: list[dict[str, Any]]
    redundant_pairs: list[dict[str, Any]]
    intersections: list[dict[str, Any]]
    missing_families: list[str]
    weak_families: list[str]


def _parse_input(value: str) -> tuple[str | None, Path]:
    if "=" in value and not Path(value).exists():
        label, raw_path = value.split("=", 1)
        return label.strip() or None, Path(raw_path)
    return None, Path(value)


def _read_table(path: Path):
    import pandas as pd

    suffix = path.suffix.lower()
    if suffix in (".parquet", ".pq"):
        return pd.read_parquet(path)
    if suffix in (".jsonl", ".ndjson"):
        return pd.read_json(path, lines=True)
    if suffix == ".json":
        payload = json.loads(path.read_text())
        if isinstance(payload, list):
            return pd.DataFrame(payload)
        if isinstance(payload, dict):
            rows = payload.get("rows")
            if isinstance(rows, list):
                return pd.DataFrame(rows)
        raise ValueError(f"{path} is JSON but not a list or {{'rows': [...]}} object")
    raise ValueError(f"unsupported input format for {path}; use parquet, jsonl, ndjson, or json")


def _infer_target(columns: list[str], requested: str | None) -> str | None:
    if requested:
        return requested if requested in columns else None
    lowered = {c.lower(): c for c in columns}
    for candidate in TARGET_CANDIDATES:
        if candidate.lower() in lowered:
            return lowered[candidate.lower()]
    return None


def _is_signal_numeric(column: str, target: str | None) -> bool:
    if target and column == target:
        return False
    if target and column.lower() in {candidate.lower() for candidate in TARGET_CANDIDATES}:
        return False
    return not METADATA_EXCLUDE_RE.search(column)


def _finite_stats(series) -> dict[str, float]:
    values = series.to_numpy(dtype=np.float64, na_value=np.nan)
    finite = values[np.isfinite(values)]
    if finite.size == 0:
        return {
            "finite_ratio": 0.0,
            "mean": float("nan"),
            "std": float("nan"),
            "min": float("nan"),
            "max": float("nan"),
        }
    return {
        "finite_ratio": float(finite.size / max(values.size, 1)),
        "mean": float(np.mean(finite)),
        "std": float(np.std(finite)),
        "min": float(np.min(finite)),
        "max": float(np.max(finite)),
    }


def _is_numeric_series(series) -> bool:
    from pandas.api.types import is_numeric_dtype

    dtype = series.dropna().infer_objects().dtype
    return bool(is_numeric_dtype(dtype))


def _pearson_corr(a, b) -> float:
    ax = a.to_numpy(dtype=np.float64, na_value=np.nan)
    bx = b.to_numpy(dtype=np.float64, na_value=np.nan)
    mask = np.isfinite(ax) & np.isfinite(bx)
    if int(mask.sum()) < 3:
        return float("nan")
    x = ax[mask]
    y = bx[mask]
    if float(np.std(x)) <= 1e-12 or float(np.std(y)) <= 1e-12:
        return float("nan")
    return float(np.corrcoef(x, y)[0, 1])


def _spearman_corr(a, b) -> float:
    ranked_a = a.rank(method="average")
    ranked_b = b.rank(method="average")
    return _pearson_corr(ranked_a, ranked_b)


def _family_keys_for(column: str) -> list[str]:
    return [family.key for family in SIGNAL_FAMILIES if family.matches(column)]


def _primary_family(column: str) -> str:
    keys = _family_keys_for(column)
    return keys[0] if keys else "uncategorized"


def _family_label(key: str) -> str:
    for family in SIGNAL_FAMILIES:
        if family.key == key:
            return family.label
    return key


def audit_table(
    label: str,
    path: Path,
    *,
    target: str | None = None,
    redundancy_threshold: float = 0.95,
    complement_threshold: float = 0.70,
    min_finite_ratio: float = 0.80,
) -> TableAudit:
    df = _read_table(path)
    columns = [str(c) for c in df.columns]
    df.columns = columns
    inferred_target = _infer_target(columns, target)
    numeric_columns = [c for c in columns if c in df and _is_numeric_series(df[c])]
    signal_columns = [c for c in numeric_columns if _is_signal_numeric(c, inferred_target)]

    family_columns: dict[str, list[str]] = {family.key: [] for family in SIGNAL_FAMILIES}
    for column in columns:
        for family in SIGNAL_FAMILIES:
            if family.matches(column):
                family_columns[family.key].append(column)

    column_stats = {column: _finite_stats(df[column]) for column in signal_columns}
    family_health: dict[str, dict[str, Any]] = {}
    missing_families: list[str] = []
    weak_families: list[str] = []
    for family in SIGNAL_FAMILIES:
        matched = family_columns[family.key]
        numeric = [c for c in matched if c in signal_columns]
        healthy = [
            c
            for c in numeric
            if column_stats[c]["finite_ratio"] >= min_finite_ratio
            and not math.isnan(column_stats[c]["std"])
            and column_stats[c]["std"] > 1e-12
        ]
        if not matched:
            missing_families.append(family.key)
            status = "missing"
        elif not healthy:
            weak_families.append(family.key)
            status = "weak"
        else:
            status = "covered"
        family_health[family.key] = {
            "status": status,
            "matched_columns": matched,
            "numeric_columns": numeric,
            "healthy_columns": healthy,
            "candidate_metrics": list(family.candidate_metrics),
            "recommendation": family.recommendation,
        }

    target_correlations: list[dict[str, Any]] = []
    if inferred_target and inferred_target in numeric_columns:
        for column in signal_columns:
            pearson = _pearson_corr(df[column], df[inferred_target])
            spearman = _spearman_corr(df[column], df[inferred_target])
            if math.isnan(pearson) and math.isnan(spearman):
                continue
            target_correlations.append(
                {
                    "column": column,
                    "family": _primary_family(column),
                    "pearson": pearson,
                    "spearman": spearman,
                    "abs_pearson": abs(pearson) if not math.isnan(pearson) else float("nan"),
                }
            )
        target_correlations.sort(
            key=lambda item: (
                math.isnan(item["abs_pearson"]),
                -item["abs_pearson"] if not math.isnan(item["abs_pearson"]) else 0.0,
            )
        )

    redundant_pairs: list[dict[str, Any]] = []
    intersections: list[dict[str, Any]] = []
    ranked_by_target = {
        item["column"]: item
        for item in target_correlations
        if not math.isnan(item["abs_pearson"]) and item["abs_pearson"] >= 0.20
    }
    for i, left in enumerate(signal_columns):
        for right in signal_columns[i + 1 :]:
            corr = _pearson_corr(df[left], df[right])
            if math.isnan(corr):
                continue
            left_family = _primary_family(left)
            right_family = _primary_family(right)
            pair = {
                "left": left,
                "right": right,
                "left_family": left_family,
                "right_family": right_family,
                "pearson": corr,
                "abs_pearson": abs(corr),
            }
            if abs(corr) >= redundancy_threshold:
                redundant_pairs.append(pair)
            if (
                left_family != right_family
                and abs(corr) <= complement_threshold
                and left in ranked_by_target
                and right in ranked_by_target
            ):
                intersections.append(
                    {
                        **pair,
                        "left_target_abs_pearson": ranked_by_target[left]["abs_pearson"],
                        "right_target_abs_pearson": ranked_by_target[right]["abs_pearson"],
                    }
                )

    redundant_pairs.sort(key=lambda item: -item["abs_pearson"])
    intersections.sort(
        key=lambda item: -(item["left_target_abs_pearson"] + item["right_target_abs_pearson"])
    )

    return TableAudit(
        label=label,
        path=path,
        rows=len(df),
        columns=columns,
        target=inferred_target,
        numeric_columns=numeric_columns,
        signal_columns=signal_columns,
        family_columns=family_columns,
        family_health=family_health,
        column_stats=column_stats,
        target_correlations=target_correlations,
        redundant_pairs=redundant_pairs,
        intersections=intersections,
        missing_families=missing_families,
        weak_families=weak_families,
    )


def _audit_to_jsonable(audit: TableAudit) -> dict[str, Any]:
    return {
        "label": audit.label,
        "path": str(audit.path),
        "rows": audit.rows,
        "columns": audit.columns,
        "target": audit.target,
        "numeric_columns": audit.numeric_columns,
        "signal_columns": audit.signal_columns,
        "family_columns": audit.family_columns,
        "family_health": audit.family_health,
        "column_stats": audit.column_stats,
        "target_correlations": audit.target_correlations,
        "redundant_pairs": audit.redundant_pairs,
        "intersections": audit.intersections,
        "missing_families": audit.missing_families,
        "weak_families": audit.weak_families,
    }


def _format_float(value: float) -> str:
    if math.isnan(value):
        return "n/a"
    return f"{value:.3f}"


def render_markdown(audits: list[TableAudit], *, top_k: int = 12) -> str:
    lines: list[str] = [
        "# Signal-mix audit",
        "",
        "This report audits extracted VQA tables for signal-family coverage,",
        "target correlation, redundancy, complementary intersections, and blind spots.",
        "It is advisory: use it to choose the next metrics/models to wire or retrain.",
        "",
        "## Inputs",
        "",
        "| Table | Rows | Columns | Target | Signal columns |",
        "|---|---:|---:|---|---:|",
    ]
    for audit in audits:
        lines.append(
            f"| `{audit.label}` | {audit.rows} | {len(audit.columns)} | "
            f"`{audit.target or 'none'}` | {len(audit.signal_columns)} |"
        )

    lines.extend(
        [
            "",
            "## Coverage Matrix",
            "",
            "| Signal family | Question | Status by table | Candidate metrics not yet guaranteed |",
            "|---|---|---|---|",
        ]
    )
    for family in SIGNAL_FAMILIES:
        statuses = []
        for audit in audits:
            health = audit.family_health[family.key]
            matched = len(health["matched_columns"])
            healthy = len(health["healthy_columns"])
            statuses.append(f"`{audit.label}`: {health['status']} ({healthy}/{matched})")
        lines.append(
            f"| {family.label} | {family.question} | {'<br>'.join(statuses)} | "
            f"{', '.join(family.candidate_metrics)} |"
        )

    lines.extend(["", "## Strongest Target Signals", ""])
    for audit in audits:
        lines.extend(
            [
                f"### `{audit.label}`",
                "",
            ]
        )
        if not audit.target:
            lines.extend(
                ["No target column found; pass `--target` if this table uses a custom name.", ""]
            )
            continue
        if not audit.target_correlations:
            lines.extend(["No usable numeric signal columns correlate with the target.", ""])
            continue
        lines.extend(["| Column | Family | Pearson | Spearman |", "|---|---|---:|---:|"])
        for row in audit.target_correlations[:top_k]:
            lines.append(
                f"| `{row['column']}` | {_family_label(row['family'])} | "
                f"{_format_float(row['pearson'])} | {_format_float(row['spearman'])} |"
            )
        lines.append("")

    lines.extend(["## Complementary Intersections", ""])
    any_intersection = False
    for audit in audits:
        if not audit.intersections:
            continue
        any_intersection = True
        lines.extend(
            [
                f"### `{audit.label}`",
                "",
                "| Pair | Families | Pair r | Target r values |",
                "|---|---|---:|---|",
            ]
        )
        for row in audit.intersections[:top_k]:
            lines.append(
                f"| `{row['left']}` × `{row['right']}` | "
                f"{_family_label(row['left_family'])} × {_family_label(row['right_family'])} | "
                f"{_format_float(row['pearson'])} | "
                f"{_format_float(row['left_target_abs_pearson'])} / "
                f"{_format_float(row['right_target_abs_pearson'])} |"
            )
        lines.append("")
    if not any_intersection:
        lines.extend(
            [
                "No cross-family low-correlation/high-target pairs met the current thresholds.",
                "That usually means the table is too narrow, the target is missing, or the",
                "current columns are mostly redundant.",
                "",
            ]
        )

    lines.extend(["## Redundant Pairs", ""])
    any_redundant = False
    for audit in audits:
        if not audit.redundant_pairs:
            continue
        any_redundant = True
        lines.extend(
            [
                f"### `{audit.label}`",
                "",
                "| Pair | Families | Pearson |",
                "|---|---|---:|",
            ]
        )
        for row in audit.redundant_pairs[:top_k]:
            lines.append(
                f"| `{row['left']}` × `{row['right']}` | "
                f"{_family_label(row['left_family'])} × {_family_label(row['right_family'])} | "
                f"{_format_float(row['pearson'])} |"
            )
        lines.append("")
    if not any_redundant:
        lines.extend(["No redundant pairs crossed the configured threshold.", ""])

    lines.extend(["## Blind Spots", ""])
    for audit in audits:
        missing = [_family_label(key) for key in audit.missing_families]
        weak = [_family_label(key) for key in audit.weak_families]
        lines.append(f"### `{audit.label}`")
        if missing:
            lines.append(f"- Missing signal families: {', '.join(missing)}.")
        else:
            lines.append("- Missing signal families: none.")
        if weak:
            lines.append(f"- Weak signal families: {', '.join(weak)}.")
        else:
            lines.append("- Weak signal families: none.")
        lines.append("")

    lines.extend(["## Recommended Next Actions", ""])
    seen: set[str] = set()
    for audit in audits:
        for key in [*audit.missing_families, *audit.weak_families]:
            if key in seen:
                continue
            seen.add(key)
            family = next(item for item in SIGNAL_FAMILIES if item.key == key)
            lines.append(f"- **{family.label}**: {family.recommendation}")
    if not seen:
        lines.append("- No missing or weak signal families detected at the configured thresholds.")
    lines.append("")
    return "\n".join(lines)


def run_audit(
    inputs: list[str],
    *,
    target: str | None,
    redundancy_threshold: float,
    complement_threshold: float,
    min_finite_ratio: float,
) -> list[TableAudit]:
    audits: list[TableAudit] = []
    for raw in inputs:
        explicit_label, path = _parse_input(raw)
        if not path.is_file():
            raise FileNotFoundError(path)
        label = explicit_label or path.stem
        audits.append(
            audit_table(
                label,
                path,
                target=target,
                redundancy_threshold=redundancy_threshold,
                complement_threshold=complement_threshold,
                min_finite_ratio=min_finite_ratio,
            )
        )
    return audits


def main(argv: list[str] | None = None) -> int:
    parser = make_argument_parser(prog="signal_mix_audit.py")
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        help="Input table path, repeatable. Use LABEL=path to override the report label.",
    )
    parser.add_argument("--target", help="Target column. Defaults to auto-detect.")
    parser.add_argument("--out-json", type=Path, required=True)
    parser.add_argument("--out-md", type=Path, required=True)
    parser.add_argument("--top-k", type=int, default=12)
    parser.add_argument("--redundancy-threshold", type=float, default=0.95)
    parser.add_argument("--complement-threshold", type=float, default=0.70)
    parser.add_argument("--min-finite-ratio", type=float, default=0.80)
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)

    audits = run_audit(
        args.input,
        target=args.target,
        redundancy_threshold=args.redundancy_threshold,
        complement_threshold=args.complement_threshold,
        min_finite_ratio=args.min_finite_ratio,
    )

    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    write_manifest_json(
        args.out_json,
        {
            "tables": [_audit_to_jsonable(audit) for audit in audits],
            "run_provenance": build_run_provenance(
                entrypoint=SCRIPT_PATH,
                repo_root=REPO_ROOT,
                argv=raw_argv,
                args=args,
                inputs={"tables": [audit.path for audit in audits]},
                outputs={"json_report": str(args.out_json), "markdown_report": str(args.out_md)},
            ),
        },
    )
    args.out_md.write_text(render_markdown(audits, top_k=args.top_k))

    missing_count = sum(len(audit.missing_families) for audit in audits)
    weak_count = sum(len(audit.weak_families) for audit in audits)
    print(
        f"[signal-mix] tables={len(audits)} missing_families={missing_count} "
        f"weak_families={weak_count} wrote={args.out_json} {args.out_md}"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
