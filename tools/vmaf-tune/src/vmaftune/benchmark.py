# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Cross-codec corpus benchmark reporting for ``vmaf-tune``.

Phase G consumes existing Phase-A JSONL rows and answers the common
post-sweep question: "which encoder hit the target quality at the
lowest bitrate?" It deliberately does not launch ffmpeg or libvmaf;
the corpus remains the source of truth.
"""

from __future__ import annotations

import csv
import dataclasses
import io
import math
from collections import defaultdict
from collections.abc import Iterable, Sequence
from typing import Any

from .jsonio import dumps_strict


@dataclasses.dataclass(frozen=True)
class BenchmarkSummary:
    """One encoder's best matched-quality corpus point."""

    encoder: str
    status: str
    rows: int
    source_count: int
    preset_count: int
    best_row: dict[str, Any]
    target_vmaf: float
    margin: float
    bitrate_kbps: float
    bitrate_delta_pct: float | None
    encode_fps: float | None
    score_fps: float | None


def _finite_float(value: object) -> float | None:
    try:
        out = float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None
    if not math.isfinite(out):
        return None
    return out


def _eligible_rows(rows: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    """Return successful rows with finite VMAF and bitrate."""
    eligible: list[dict[str, Any]] = []
    for row in rows:
        try:
            status = int(row.get("exit_status", 0))
        except (TypeError, ValueError):
            continue
        if status != 0:
            continue
        if _finite_float(row.get("vmaf_score")) is None:
            continue
        if _finite_float(row.get("bitrate_kbps")) is None:
            continue
        eligible.append(row)
    return eligible


def _mean_positive(values: Iterable[float | None]) -> float | None:
    kept = [v for v in values if v is not None and v > 0.0 and math.isfinite(v)]
    if not kept:
        return None
    return sum(kept) / len(kept)


def _row_encode_fps(row: dict[str, Any]) -> float | None:
    duration_s = _finite_float(row.get("duration_s"))
    encode_ms = _finite_float(row.get("encode_time_ms"))
    if duration_s is None or encode_ms is None or duration_s <= 0.0 or encode_ms <= 0.0:
        return None
    return duration_s / (encode_ms / 1000.0)


def _row_score_fps(row: dict[str, Any]) -> float | None:
    duration_s = _finite_float(row.get("duration_s"))
    score_ms = _finite_float(row.get("score_time_ms"))
    framerate = _finite_float(row.get("framerate"))
    if (
        duration_s is None
        or score_ms is None
        or framerate is None
        or duration_s <= 0.0
        or score_ms <= 0.0
        or framerate <= 0.0
    ):
        return None
    return (duration_s * framerate) / (score_ms / 1000.0)


def _best_row(rows: Sequence[dict[str, Any]], target_vmaf: float) -> tuple[str, dict[str, Any]]:
    clearing = [row for row in rows if float(row["vmaf_score"]) >= target_vmaf]
    if clearing:
        return (
            "ok",
            min(
                clearing,
                key=lambda row: (
                    float(row["bitrate_kbps"]),
                    -float(row["vmaf_score"]),
                    int(row.get("crf", 0)),
                ),
            ),
        )
    return (
        "unmet",
        max(
            rows,
            key=lambda row: (
                float(row["vmaf_score"]),
                -float(row["bitrate_kbps"]),
            ),
        ),
    )


def summarize_benchmark(
    rows: Iterable[dict[str, Any]],
    *,
    target_vmaf: float,
    baseline_encoder: str | None = None,
) -> list[BenchmarkSummary]:
    """Summarise best matched-quality point per encoder.

    For every encoder, the chosen point is the lowest-bitrate row whose
    ``vmaf_score`` clears ``target_vmaf``. If the encoder never clears,
    the closest miss by VMAF is reported with ``status="unmet"``.
    """
    eligible = _eligible_rows(rows)
    if not eligible:
        raise ValueError("no successful finite corpus rows to benchmark")

    by_encoder: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in eligible:
        encoder = str(row.get("encoder", ""))
        if encoder:
            by_encoder[encoder].append(row)
    if not by_encoder:
        raise ValueError("corpus rows do not include encoder names")

    raw: list[BenchmarkSummary] = []
    for encoder, group in sorted(by_encoder.items()):
        status, row = _best_row(group, target_vmaf)
        bitrate = float(row["bitrate_kbps"])
        summary = BenchmarkSummary(
            encoder=encoder,
            status=status,
            rows=len(group),
            source_count=len({str(r.get("src", "")) for r in group}),
            preset_count=len({str(r.get("preset", "")) for r in group}),
            best_row=row,
            target_vmaf=target_vmaf,
            margin=float(row["vmaf_score"]) - target_vmaf,
            bitrate_kbps=bitrate,
            bitrate_delta_pct=None,
            encode_fps=_mean_positive(_row_encode_fps(r) for r in group),
            score_fps=_mean_positive(_row_score_fps(r) for r in group),
        )
        raw.append(summary)

    baseline = _resolve_baseline(raw, baseline_encoder)
    if baseline is None:
        return sorted(raw, key=_summary_sort_key)
    base_kbps = baseline.bitrate_kbps
    with_delta = [
        dataclasses.replace(
            item,
            bitrate_delta_pct=(
                ((item.bitrate_kbps - base_kbps) / base_kbps) * 100.0 if base_kbps > 0.0 else None
            ),
        )
        for item in raw
    ]
    return sorted(with_delta, key=_summary_sort_key)


def _resolve_baseline(
    summaries: Sequence[BenchmarkSummary],
    baseline_encoder: str | None,
) -> BenchmarkSummary | None:
    if baseline_encoder is not None:
        for item in summaries:
            if item.encoder == baseline_encoder:
                return item
        raise ValueError(f"baseline encoder {baseline_encoder!r} not present in corpus")
    ok = [item for item in summaries if item.status == "ok"]
    if not ok:
        return None
    return min(ok, key=lambda item: item.bitrate_kbps)


def _summary_sort_key(item: BenchmarkSummary) -> tuple[int, float, str]:
    return (0 if item.status == "ok" else 1, item.bitrate_kbps, item.encoder)


def summaries_to_dicts(summaries: Sequence[BenchmarkSummary]) -> list[dict[str, Any]]:
    """JSON-serialisable benchmark payload."""
    out: list[dict[str, Any]] = []
    for item in summaries:
        row = item.best_row
        out.append(
            {
                "encoder": item.encoder,
                "status": item.status,
                "target_vmaf": item.target_vmaf,
                "margin": item.margin,
                "bitrate_kbps": item.bitrate_kbps,
                "bitrate_delta_pct": item.bitrate_delta_pct,
                "rows": item.rows,
                "source_count": item.source_count,
                "preset_count": item.preset_count,
                "encode_fps": item.encode_fps,
                "score_fps": item.score_fps,
                "best": {
                    "src": row.get("src", ""),
                    "preset": row.get("preset", ""),
                    "crf": row.get("crf"),
                    "vmaf_score": row.get("vmaf_score"),
                    "bitrate_kbps": row.get("bitrate_kbps"),
                    "vmaf_model": row.get("vmaf_model", ""),
                },
            }
        )
    return out


def render_json(summaries: Sequence[BenchmarkSummary]) -> str:
    """Render benchmark summaries as stable pretty RFC 8259 JSON (ADR-0988)."""
    return dumps_strict(summaries_to_dicts(summaries)) + "\n"


def render_csv(summaries: Sequence[BenchmarkSummary]) -> str:
    """Render benchmark summaries as CSV."""
    buf = io.StringIO()
    fieldnames = (
        "encoder",
        "status",
        "target_vmaf",
        "vmaf_score",
        "margin",
        "bitrate_kbps",
        "bitrate_delta_pct",
        "preset",
        "crf",
        "rows",
        "source_count",
        "preset_count",
        "encode_fps",
        "score_fps",
    )
    writer = csv.DictWriter(buf, fieldnames=fieldnames)
    writer.writeheader()
    for item in summaries:
        row = item.best_row
        writer.writerow(
            {
                "encoder": item.encoder,
                "status": item.status,
                "target_vmaf": f"{item.target_vmaf:.3f}",
                "vmaf_score": f"{float(row['vmaf_score']):.3f}",
                "margin": f"{item.margin:.3f}",
                "bitrate_kbps": f"{item.bitrate_kbps:.3f}",
                "bitrate_delta_pct": _format_optional(item.bitrate_delta_pct),
                "preset": row.get("preset", ""),
                "crf": row.get("crf", ""),
                "rows": item.rows,
                "source_count": item.source_count,
                "preset_count": item.preset_count,
                "encode_fps": _format_optional(item.encode_fps),
                "score_fps": _format_optional(item.score_fps),
            }
        )
    return buf.getvalue()


def render_markdown(summaries: Sequence[BenchmarkSummary]) -> str:
    """Render benchmark summaries as a compact markdown table."""
    lines = [
        "| Encoder | Status | VMAF | kbps | Δ kbps | Preset | CRF | Rows | Encode fps | Score fps |",
        "| --- | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |",
    ]
    for item in summaries:
        row = item.best_row
        lines.append(
            "| "
            f"{item.encoder} | "
            f"{item.status} | "
            f"{float(row['vmaf_score']):.3f} | "
            f"{item.bitrate_kbps:.1f} | "
            f"{_format_optional(item.bitrate_delta_pct)} | "
            f"{row.get('preset', '')} | "
            f"{row.get('crf', '')} | "
            f"{item.rows} | "
            f"{_format_optional(item.encode_fps)} | "
            f"{_format_optional(item.score_fps)} |"
        )
    return "\n".join(lines) + "\n"


def render_benchmark(summaries: Sequence[BenchmarkSummary], *, fmt: str) -> str:
    """Dispatch to the requested renderer."""
    if fmt == "json":
        return render_json(summaries)
    if fmt == "csv":
        return render_csv(summaries)
    if fmt == "markdown":
        return render_markdown(summaries)
    raise ValueError(f"unknown benchmark format {fmt!r}")


def _format_optional(value: float | None) -> str:
    if value is None or not math.isfinite(value):
        return ""
    return f"{value:.3f}"
