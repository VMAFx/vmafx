# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Profile-card report renderer for a tuned video.

Takes the structured outputs of the vmaf-tune pipeline (ladder build,
codec comparison, per-shot tune) and emits a single self-contained
artifact in either Markdown (with inline PNGs) or HTML (with inline
SVGs) form. Both formats share one ``ReportData`` dataclass and one
template loader, so adding a third (e.g. PDF via WeasyPrint) is a
one-template change.

Wired up to the CLI as ``vmaf-tune report …``. The CLI orchestrates
the upstream phases (ladder + compare + optional per-shot) and hands
the structured outputs here; this module never invokes the encoder or
the libvmaf CLI directly.

The renderer is intentionally pure data → string. Matplotlib is the
only heavy dependency and is imported lazily so unrelated CLI paths
don't pay the import cost.
"""

from __future__ import annotations

import base64
import dataclasses
import html
import io
import json
import math
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from . import __version__ as TOOL_VERSION
from .jsonio import dumps_strict as _portable_json_dump  # ADR-0988: shared helper


def _html_escape(value: Any) -> str:
    """Escape user-controlled report text before embedding it in HTML."""
    return html.escape(str(value), quote=True)


def _md_cell(value: Any) -> str:
    """Escape markdown table metacharacters in user-controlled cells."""
    return str(value).replace("|", r"\|").replace("\n", "<br>")


ENCODER_PROFILE_SCHEMA = "vmaftune.encoder_profile.v1"
ENCODER_PROFILE_VERSION = 1


_SECTION_GUIDANCE: dict[str, str] = {
    "sweep": (
        "How to read this: higher is better on the vertical axis; farther left is smaller "
        "on the bitrate axis. Hollow markers are the CRFs actually selected for a target; "
        "the dashed black line is the bitrate-minimising frontier across codecs."
    ),
    "codec": (
        "How to read this: every row targets the same VMAF. The useful winner is the row "
        "with the lowest bitrate that still clears the target; failed rows remain visible "
        "so missing encoder support is not mistaken for a quality result."
    ),
    "ladder": (
        "How to read this: selected rungs are the delivery ladder. A good ladder increases "
        "quality smoothly with bitrate; isolated jumps usually mean the source or codec "
        "needs a narrower follow-up sweep."
    ),
    "shots": (
        "How to read this: CRF changes mark scenes that needed different compression "
        "pressure. Large CRF swings are useful cues for per-shot overrides and future "
        "profile training."
    ),
    "profile": (
        "The raw JSON contains encoder_profile, a stable payload that can be passed to "
        "`vmaf-tune encode-profile` to reproduce one selected recommendation."
    ),
}


_CODEC_METADATA: dict[str, dict[str, str]] = {
    "libx264": {
        "badge": "H.264",
        "label": "x264 / AVC",
        "family": "H.264 / AVC",
        "implementation": "CPU software",
        "url": "https://www.videolan.org/developers/x264.html",
        "note": "Fast and mature AVC baseline; useful for compatibility checks.",
    },
    "libx265": {
        "badge": "HEVC",
        "label": "x265 / HEVC",
        "family": "H.265 / HEVC",
        "implementation": "CPU software",
        "url": "https://www.x265.org/",
        "note": "Strong CPU compression baseline for high-quality HEVC ladders.",
    },
    "libsvtav1": {
        "badge": "AV1",
        "label": "SVT-AV1",
        "family": "AV1",
        "implementation": "CPU software",
        "url": "https://gitlab.com/AOMediaCodec/SVT-AV1",
        "note": "Production AV1 encoder; good default for CPU AV1 profile rows.",
    },
    "libaom-av1": {
        "badge": "AV1",
        "label": "AOM AV1",
        "family": "AV1",
        "implementation": "CPU reference",
        "url": "https://aomedia.googlesource.com/aom/",
        "note": "Reference AV1 encoder; valuable for quality experiments, usually slower.",
    },
    "libvpx-vp9": {
        "badge": "VP9",
        "label": "libvpx VP9",
        "family": "VP9",
        "implementation": "CPU software",
        "url": "https://chromium.googlesource.com/webm/libvpx/",
        "note": "VP9 baseline for older web delivery comparisons.",
    },
    "libvpx": {
        "badge": "VP9",
        "label": "libvpx",
        "family": "VP8 / VP9",
        "implementation": "CPU software",
        "url": "https://chromium.googlesource.com/webm/libvpx/",
        "note": "WebM codec family adapter.",
    },
    "libvvenc": {
        "badge": "VVC",
        "label": "VVenC / H.266",
        "family": "H.266 / VVC",
        "implementation": "CPU software",
        "url": "https://github.com/fraunhoferhhi/vvenc",
        "note": "VVC research/next-generation profile row.",
    },
    "h264_nvenc": {
        "badge": "NVENC",
        "label": "NVIDIA H.264 NVENC",
        "family": "H.264 / AVC",
        "implementation": "NVIDIA hardware",
        "url": "https://developer.nvidia.com/video-codec-sdk",
        "note": "NVIDIA hardware encoder; speed-oriented, device-dependent quality.",
    },
    "hevc_nvenc": {
        "badge": "NVENC",
        "label": "NVIDIA HEVC NVENC",
        "family": "H.265 / HEVC",
        "implementation": "NVIDIA hardware",
        "url": "https://developer.nvidia.com/video-codec-sdk",
        "note": "NVIDIA HEVC hardware encoder; useful for fast production ladders.",
    },
    "av1_nvenc": {
        "badge": "NVENC",
        "label": "NVIDIA AV1 NVENC",
        "family": "AV1",
        "implementation": "NVIDIA hardware",
        "url": "https://developer.nvidia.com/video-codec-sdk",
        "note": "NVIDIA AV1 hardware encoder on supported RTX generations.",
    },
    "h264_qsv": {
        "badge": "QSV",
        "label": "Intel H.264 Quick Sync",
        "family": "H.264 / AVC",
        "implementation": "Intel hardware",
        "url": "https://www.intel.com/content/www/us/en/developer/tools/oneapi/onevpl.html",
        "note": "Intel media hardware path; depends on the active render node.",
    },
    "hevc_qsv": {
        "badge": "QSV",
        "label": "Intel HEVC Quick Sync",
        "family": "H.265 / HEVC",
        "implementation": "Intel hardware",
        "url": "https://www.intel.com/content/www/us/en/developer/tools/oneapi/onevpl.html",
        "note": "Intel HEVC hardware path for Arc/iGPU profile rows.",
    },
    "av1_qsv": {
        "badge": "QSV",
        "label": "Intel AV1 Quick Sync",
        "family": "AV1",
        "implementation": "Intel hardware",
        "url": "https://www.intel.com/content/www/us/en/developer/tools/oneapi/onevpl.html",
        "note": "Intel AV1 hardware path on supported Arc/iGPU devices.",
    },
    "h264_amf": {
        "badge": "AMF",
        "label": "AMD H.264 AMF",
        "family": "H.264 / AVC",
        "implementation": "AMD hardware",
        "url": "https://gpuopen.com/advanced-media-framework/",
        "note": "AMD hardware encoder via Advanced Media Framework.",
    },
    "hevc_amf": {
        "badge": "AMF",
        "label": "AMD HEVC AMF",
        "family": "H.265 / HEVC",
        "implementation": "AMD hardware",
        "url": "https://gpuopen.com/advanced-media-framework/",
        "note": "AMD HEVC hardware encoder via Advanced Media Framework.",
    },
    "av1_amf": {
        "badge": "AMF",
        "label": "AMD AV1 AMF",
        "family": "AV1",
        "implementation": "AMD hardware",
        "url": "https://gpuopen.com/advanced-media-framework/",
        "note": "AMD AV1 hardware encoder on supported Radeon generations.",
    },
    "h264_videotoolbox": {
        "badge": "VT",
        "label": "Apple H.264 VideoToolbox",
        "family": "H.264 / AVC",
        "implementation": "Apple hardware",
        "url": "https://developer.apple.com/documentation/videotoolbox",
        "note": "Apple platform hardware encoder.",
    },
    "hevc_videotoolbox": {
        "badge": "VT",
        "label": "Apple HEVC VideoToolbox",
        "family": "H.265 / HEVC",
        "implementation": "Apple hardware",
        "url": "https://developer.apple.com/documentation/videotoolbox",
        "note": "Apple HEVC hardware encoder.",
    },
    "av1_videotoolbox": {
        "badge": "VT",
        "label": "Apple AV1 VideoToolbox",
        "family": "AV1",
        "implementation": "Apple hardware",
        "url": "https://developer.apple.com/documentation/videotoolbox",
        "note": "Apple AV1 hardware encoder where the platform exposes it.",
    },
    "prores_videotoolbox": {
        "badge": "ProRes",
        "label": "Apple ProRes VideoToolbox",
        "family": "ProRes",
        "implementation": "Apple hardware",
        "url": "https://developer.apple.com/documentation/videotoolbox",
        "note": "Mezzanine/intermediate profile row, not an ABR delivery codec.",
    },
}


@dataclasses.dataclass(frozen=True)
class CodecRow:
    """One row in the codec-comparison table."""

    codec: str
    encoder_version: str
    best_crf: int
    bitrate_kbps: float
    encode_time_ms: float
    vmaf_score: float
    ok: bool
    error: str = ""


@dataclasses.dataclass(frozen=True)
class LadderRung:
    """One rung of the final ABR ladder."""

    width: int
    height: int
    bitrate_kbps: float
    vmaf: float
    crf: int


@dataclasses.dataclass(frozen=True)
class LadderSample:
    """One sampled (resolution, vmaf, bitrate, crf) point."""

    width: int
    height: int
    bitrate_kbps: float
    vmaf: float
    crf: int


@dataclasses.dataclass(frozen=True)
class ShotRow:
    """One per-shot tune result."""

    shot_index: int
    start_frame: int
    end_frame: int
    width: int
    height: int
    best_crf: int
    vmaf: float
    bitrate_kbps: float
    duration_s: float


@dataclasses.dataclass(frozen=True)
class SourceInfo:
    """Metadata about the source video."""

    path: str
    width: int
    height: int
    fps: float
    duration_s: float
    frame_count: int
    codec: str
    size_bytes: int


@dataclasses.dataclass(frozen=True)
class BisectSamplePoint:
    """One per-iteration (CRF, bitrate, VMAF) probe from a target-VMAF bisect.

    Mirrors :class:`vmaftune.bisect.BisectSample` on the report side so
    the renderer does not import the bisect module (keeps ``report``
    free of subprocess / ffmpeg deps).
    """

    crf: int
    bitrate_kbps: float
    vmaf_score: float
    encode_time_ms: float = 0.0


@dataclasses.dataclass(frozen=True)
class CodecSweepPoint:
    """One (codec, target_vmaf) point in a rate-quality sweep.

    The renderer builds the per-codec rate-quality line from these
    points (one line per codec, ordered by ``target_vmaf``) and
    derives the pareto frontier — for each target VMAF, the codec
    with the smallest bitrate.

    Schema v2 ingestion (ADR-0516). The legacy single-target compare
    output (schema v1) ingests as :class:`CodecRow` and renders the
    historical bar+dot chart for back-compat.

    ``bisect_samples`` (ADR-0530, schema v2 additive) carries every
    encode+score probe the underlying bisect walked through. When
    populated, the rate-quality chart aggregates these per codec
    (across all targets) and draws a single monotonic R-Q curve
    instead of connecting the picked-CRF cells. Empty tuple = old v2
    JSON without samples — renderer falls back to the legacy
    connect-the-dots line with a caveat note.
    """

    codec: str
    encoder_version: str
    target_vmaf: float
    best_crf: int
    bitrate_kbps: float
    encode_time_ms: float
    vmaf_score: float
    ok: bool
    error: str = ""
    bisect_samples: tuple[BisectSamplePoint, ...] = ()


@dataclasses.dataclass(frozen=True)
class ReportData:
    """All structured inputs for one report."""

    source: SourceInfo
    target_vmaf: float
    codec_rows: tuple[CodecRow, ...] = ()
    sweep_points: tuple[CodecSweepPoint, ...] = ()
    sweep_targets: tuple[float, ...] = ()
    ladder_samples: tuple[LadderSample, ...] = ()
    ladder_rungs: tuple[LadderRung, ...] = ()
    shots: tuple[ShotRow, ...] = ()
    tool_version: str = TOOL_VERSION
    generated_at_iso: str = ""
    encoder_preset: str = ""
    pix_fmt: str = ""
    score_backend: str = ""
    ffmpeg_bin: str = ""
    vmaf_bin: str = ""

    def to_dict(self) -> dict[str, Any]:
        """Serialisable view of the structured inputs.

        Used by the HTML/MD templates and any external consumer that
        wants to re-render or diff two reports.
        """
        return {
            "source": dataclasses.asdict(self.source),
            "target_vmaf": self.target_vmaf,
            "codec_rows": [dataclasses.asdict(r) for r in self.codec_rows],
            "sweep_points": [dataclasses.asdict(p) for p in self.sweep_points],
            "sweep_targets": list(self.sweep_targets),
            "ladder_samples": [dataclasses.asdict(p) for p in self.ladder_samples],
            "ladder_rungs": [dataclasses.asdict(r) for r in self.ladder_rungs],
            "shots": [dataclasses.asdict(s) for s in self.shots],
            "tool_version": self.tool_version,
            "generated_at_iso": self.generated_at_iso,
            "encoder_preset": self.encoder_preset,
            "pix_fmt": self.pix_fmt,
            "score_backend": self.score_backend,
            "ffmpeg_bin": self.ffmpeg_bin,
            "vmaf_bin": self.vmaf_bin,
            "encoder_profile": build_encoder_profile(self),
        }


def _codecs_in_report(data: ReportData) -> tuple[str, ...]:
    codecs: set[str] = set()
    codecs.update(r.codec for r in data.codec_rows if r.codec)
    codecs.update(p.codec for p in data.sweep_points if p.codec)
    return tuple(sorted(codecs))


def codec_metadata(codec: str) -> dict[str, str]:
    """Return human/report metadata for a codec adapter token."""
    base = {
        "badge": codec.upper()[:8] or "?",
        "label": codec or "Unknown codec",
        "family": "Unknown",
        "implementation": "Unknown",
        "url": "",
        "note": "Adapter metadata is not registered yet.",
    }
    base.update(_CODEC_METADATA.get(codec, {}))
    base["codec"] = codec
    base["colour"] = _codec_colour(codec, 0)
    return base


def _codec_chip_md(codec: str) -> str:
    meta = codec_metadata(codec)
    label = f"{meta['badge']} {codec}"
    if meta["url"]:
        return f"[{_md_cell(label)}]({meta['url']})"
    return _md_cell(label)


def _codec_chip_html(codec: str) -> str:
    meta = codec_metadata(codec)
    style = f"--codec-colour:{_html_escape(meta['colour'])}"
    content = (
        f"<span class='codec-logo'>{_html_escape(meta['badge'])}</span>"
        f"<span class='codec-name'>{_html_escape(codec)}</span>"
    )
    if meta["url"]:
        return (
            f"<a class='codec-chip' href='{_html_escape(meta['url'])}' "
            f"style='{style}'>{content}</a>"
        )
    return f"<span class='codec-chip' style='{style}'>{content}</span>"


def _render_codec_guide_md(data: ReportData) -> list[str]:
    codecs = _codecs_in_report(data)
    if not codecs:
        return []
    lines = ["### Codec guide", ""]
    lines.append(
        "Codec badges are self-contained identity chips: they act like report-local logos, "
        "link to the upstream project/vendor, and show whether a row is software or hardware."
    )
    lines.append("")
    for codec in codecs:
        meta = codec_metadata(codec)
        lines.append(
            f"- {_codec_chip_md(codec)} — {_md_cell(meta['label'])}; "
            f"{_md_cell(meta['implementation'])}. {_md_cell(meta['note'])}"
        )
    return lines


def _render_codec_guide_html(data: ReportData) -> str:
    codecs = _codecs_in_report(data)
    if not codecs:
        return ""
    items = []
    for codec in codecs:
        meta = codec_metadata(codec)
        items.append(
            "<li>"
            f"{_codec_chip_html(codec)} "
            f"<span>{_html_escape(meta['label'])}; "
            f"{_html_escape(meta['implementation'])}. {_html_escape(meta['note'])}</span>"
            "</li>"
        )
    return (
        "<div class='guide codec-guide'><h3>Codec guide</h3>"
        "<p>Codec badges are self-contained identity chips: they act like report-local "
        "logos, link to the upstream project/vendor, and show whether a row is software "
        "or hardware.</p>"
        f"<ul>{''.join(items)}</ul></div>"
    )


def _sweep_row_status(points: Sequence[CodecSweepPoint | None], targets: Sequence[float]) -> str:
    failures: list[str] = []
    present_targets = {p.target_vmaf for p in points if p is not None}
    for target in targets:
        if target not in present_targets:
            failures.append(f"VMAF {target:g}: missing")
    for p in points:
        if p is None or p.ok:
            continue
        reason = p.error or "failed"
        failures.append(f"VMAF {p.target_vmaf:g}: {reason}")
    if not failures:
        return "OK"
    return "; ".join(failures)


def _sweep_crf_picks(points: Sequence[CodecSweepPoint]) -> str:
    ok_points = [p for p in points if p.ok and p.best_crf >= 0]
    if not ok_points:
        return _DASH
    ok_points.sort(key=lambda p: p.target_vmaf)
    return ", ".join(f"{p.target_vmaf:g}→{p.best_crf}" for p in ok_points)


def _recommendation_row(
    *,
    index: int,
    codec: str,
    encoder_version: str,
    target_vmaf: float,
    crf: int,
    bitrate_kbps: float,
    encode_time_ms: float,
    vmaf_score: float,
    preset: str,
    selected_pareto: bool,
    source_row: str,
) -> dict[str, Any]:
    return {
        "index": index,
        "codec": codec,
        "encoder_version": encoder_version,
        "target_vmaf": target_vmaf,
        "quality_knob": "crf",
        "quality": crf,
        "crf": crf,
        "preset": preset,
        "bitrate_kbps": bitrate_kbps,
        "achieved_vmaf": vmaf_score,
        "encode_time_ms": encode_time_ms,
        "selected_pareto": selected_pareto,
        "source_row": source_row,
    }


def build_encoder_profile(data: ReportData) -> dict[str, Any]:
    """Build the machine-readable profile payload embedded in every report."""
    codecs = _codecs_in_report(data)
    frontier_keys = {
        (p.codec, p.target_vmaf, p.best_crf)
        for p in compute_pareto_frontier(data.sweep_points)
        if p.ok
    }
    recommendations: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    preset = data.encoder_preset

    for row in data.codec_rows:
        if row.ok and not _is_missing(row.bitrate_kbps) and not _is_missing(row.vmaf_score):
            recommendations.append(
                _recommendation_row(
                    index=len(recommendations),
                    codec=row.codec,
                    encoder_version=row.encoder_version,
                    target_vmaf=data.target_vmaf,
                    crf=row.best_crf,
                    bitrate_kbps=row.bitrate_kbps,
                    encode_time_ms=row.encode_time_ms,
                    vmaf_score=row.vmaf_score,
                    preset=preset,
                    selected_pareto=True,
                    source_row="compare",
                )
            )
        else:
            failures.append(
                {
                    "codec": row.codec,
                    "target_vmaf": data.target_vmaf,
                    "error": row.error or "failed",
                    "source_row": "compare",
                }
            )

    for point in data.sweep_points:
        if point.ok and not _is_missing(point.bitrate_kbps) and not _is_missing(point.vmaf_score):
            recommendations.append(
                _recommendation_row(
                    index=len(recommendations),
                    codec=point.codec,
                    encoder_version=point.encoder_version,
                    target_vmaf=point.target_vmaf,
                    crf=point.best_crf,
                    bitrate_kbps=point.bitrate_kbps,
                    encode_time_ms=point.encode_time_ms,
                    vmaf_score=point.vmaf_score,
                    preset=preset,
                    selected_pareto=(point.codec, point.target_vmaf, point.best_crf)
                    in frontier_keys,
                    source_row="sweep",
                )
            )
        else:
            failures.append(
                {
                    "codec": point.codec,
                    "target_vmaf": point.target_vmaf,
                    "error": point.error or "failed",
                    "source_row": "sweep",
                }
            )

    recommendations.sort(
        key=lambda r: (
            not bool(r["selected_pareto"]),
            float(r["target_vmaf"]),
            float(r["bitrate_kbps"]),
            str(r["codec"]),
        )
    )
    for idx, rec in enumerate(recommendations):
        rec["index"] = idx

    return {
        "schema": ENCODER_PROFILE_SCHEMA,
        "schema_version": ENCODER_PROFILE_VERSION,
        "source": dataclasses.asdict(data.source),
        "run": {
            "target_vmaf": data.target_vmaf,
            "sweep_targets": list(data.sweep_targets),
            "tool_version": data.tool_version,
            "generated_at_iso": data.generated_at_iso,
            "preset": data.encoder_preset,
            "pix_fmt": data.pix_fmt,
            "score_backend": data.score_backend,
            "ffmpeg_bin": data.ffmpeg_bin,
            "vmaf_bin": data.vmaf_bin,
        },
        "codec_metadata": {codec: codec_metadata(codec) for codec in codecs},
        "recommendations": recommendations,
        "failures": failures,
        "ladder": {
            "samples": [dataclasses.asdict(p) for p in data.ladder_samples],
            "rungs": [dataclasses.asdict(r) for r in data.ladder_rungs],
        },
        "shots": [dataclasses.asdict(s) for s in data.shots],
        "human_guidance": dict(_SECTION_GUIDANCE),
    }


# ---------------------------------------------------------------------------
# Charts (lazy matplotlib import)
# ---------------------------------------------------------------------------


def _render_chart(width_in: float, height_in: float, plot_fn) -> bytes:
    """Render a matplotlib figure to PNG bytes.

    ``plot_fn`` is called with one ``Axes`` argument. The figure is
    closed after rendering so callers don't accumulate global state.

    Returns empty bytes when matplotlib is not importable. ADR-0498
    pairs this fallback with the Containerfile pin so the table-only
    report renders even on hosts that didn't pip-install matplotlib
    (Bug #v2-C, BBB e2e v2 2026-05-18).
    """
    try:
        import matplotlib
    except ImportError:
        return b""

    matplotlib.use("Agg", force=True)
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(width_in, height_in), dpi=110)
    try:
        plot_fn(ax)
        buf = io.BytesIO()
        fig.savefig(buf, format="png", bbox_inches="tight")
        return buf.getvalue()
    finally:
        plt.close(fig)


def _render_chart_svg(width_in: float, height_in: float, plot_fn) -> str:
    """Render a matplotlib figure to SVG string for inline-HTML use.

    Returns an empty string when matplotlib is unavailable so the
    surrounding HTML still renders without the chart panel
    (ADR-0498, Bug #v2-C fallback companion).
    """
    try:
        import matplotlib
    except ImportError:
        return ""

    matplotlib.use("Agg", force=True)
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(width_in, height_in), dpi=110)
    try:
        plot_fn(ax)
        buf = io.StringIO()
        fig.savefig(buf, format="svg", bbox_inches="tight")
        return buf.getvalue()
    finally:
        plt.close(fig)


def _ladder_plot_fn(data: ReportData):
    def _plot(ax) -> None:
        if data.ladder_samples:
            xs = [p.bitrate_kbps for p in data.ladder_samples]
            ys = [p.vmaf for p in data.ladder_samples]
            ax.scatter(xs, ys, s=12, alpha=0.45, label=f"samples ({len(xs)})")
        if data.ladder_rungs:
            xs = [r.bitrate_kbps for r in data.ladder_rungs]
            ys = [r.vmaf for r in data.ladder_rungs]
            ax.plot(xs, ys, marker="o", linewidth=2, color="#d62728", label="picked rungs")
        ax.set_xlabel("bitrate (kbps)")
        ax.set_ylabel("VMAF")
        if data.target_vmaf:
            ax.axhline(
                data.target_vmaf,
                color="#1f77b4",
                linestyle="--",
                alpha=0.5,
                label=f"target {data.target_vmaf:.1f}",
            )
        _apply_bitrate_xaxis(ax, log_scale=True)
        ax.grid(True, alpha=0.3)
        _set_padded_ylim(ax, ys + [data.target_vmaf], lower=0.0, upper=100.0, min_pad=1.0)
        ax.legend(loc="lower right", fontsize=8)
        ax.set_title("ABR ladder — higher quality at each bitrate is better")

    return _plot


def _codec_plot_fn(data: ReportData):
    def _plot(ax) -> None:
        # Drop failed AND non-finite-numeric rows so the bar / line
        # axes don't try to plot NaN (matplotlib would otherwise emit
        # an empty plot or a warning, depending on version). Bug #6.
        rows = [
            r
            for r in data.codec_rows
            if r.ok and not _is_missing(r.bitrate_kbps) and not _is_missing(r.vmaf_score)
        ]
        if not rows:
            ax.text(0.5, 0.5, "no successful codec rows", ha="center", va="center")
            ax.set_axis_off()
            return
        codecs = [r.codec for r in rows]
        bitrates = [r.bitrate_kbps for r in rows]
        vmafs = [r.vmaf_score for r in rows]
        ax.bar(codecs, bitrates, color="#2ca02c")
        ax.set_ylabel("bitrate (lower is smaller)")
        _apply_bitrate_yaxis(ax)
        ax.set_title("Codec comparison — lowest bitrate that reaches target wins")
        ax2 = ax.twinx()
        ax2.plot(codecs, vmafs, "o-", color="#d62728", label="VMAF achieved")
        ax2.set_ylabel("VMAF")
        _set_padded_ylim(ax2, vmafs, lower=0.0, upper=100.0, min_pad=0.75)
        ax2.legend(loc="lower right", fontsize=8)
        for tick in ax.get_xticklabels():
            tick.set_rotation(20)

    return _plot


# Stable codec -> colour map so the same codec keeps the same hue in
# successive reports (avoids "blue swap" diffs when comparing two HTML
# reports side by side). Matplotlib's default Tab10 + Set2 give 18
# distinct hues — enough headroom for the CPU+HW encoder matrix.
_CODEC_PALETTE: tuple[str, ...] = (
    "#1f77b4",  # libx264
    "#ff7f0e",  # libx265
    "#2ca02c",  # libsvtav1
    "#d62728",  # libvpx-vp9
    "#9467bd",  # libaom
    "#8c564b",  # libvvenc
    "#e377c2",  # h264_nvenc
    "#7f7f7f",  # hevc_nvenc
    "#bcbd22",  # av1_nvenc
    "#17becf",  # h264_qsv
    "#aec7e8",  # hevc_qsv
    "#ffbb78",  # av1_qsv
    "#98df8a",  # h264_amf
    "#ff9896",  # hevc_amf
    "#c5b0d5",  # av1_amf
)

_CODEC_COLOURS: dict[str, str] = {
    "libx264": _CODEC_PALETTE[0],
    "libx265": _CODEC_PALETTE[1],
    "libsvtav1": _CODEC_PALETTE[2],
    "libvpx-vp9": _CODEC_PALETTE[3],
    "libvpx": _CODEC_PALETTE[3],
    "libaom-av1": _CODEC_PALETTE[4],
    "libaom": _CODEC_PALETTE[4],
    "libvvenc": _CODEC_PALETTE[5],
    "h264_nvenc": _CODEC_PALETTE[6],
    "hevc_nvenc": _CODEC_PALETTE[7],
    "av1_nvenc": _CODEC_PALETTE[8],
    "h264_qsv": _CODEC_PALETTE[9],
    "hevc_qsv": _CODEC_PALETTE[10],
    "av1_qsv": _CODEC_PALETTE[11],
    "h264_amf": _CODEC_PALETTE[12],
    "hevc_amf": _CODEC_PALETTE[13],
    "av1_amf": _CODEC_PALETTE[14],
    "h264_videotoolbox": _CODEC_PALETTE[0],
    "hevc_videotoolbox": _CODEC_PALETTE[1],
    "av1_videotoolbox": _CODEC_PALETTE[2],
    "prores_videotoolbox": _CODEC_PALETTE[5],
}


def _codec_colour(codec: str, index: int) -> str:
    """Deterministic colour for ``codec`` — stable even when rows are filtered."""
    colour = _CODEC_COLOURS.get(codec)
    if colour is not None:
        return colour
    stable_idx = sum((idx + 1) * ord(ch) for idx, ch in enumerate(codec))
    return _CODEC_PALETTE[(stable_idx + index) % len(_CODEC_PALETTE)]


def _bitrate_tick_label(value: float, _pos: float | None = None) -> str:
    if value <= 0 or not math.isfinite(value):
        return ""
    if value >= 1000:
        return f"{value / 1000:g}M"
    return f"{value:g}k"


def _finite_values(values: Sequence[float | int | None]) -> list[float]:
    finite: list[float] = []
    for value in values:
        if value is None:
            continue
        f = float(value)
        if math.isfinite(f):
            finite.append(f)
    return finite


def _set_padded_ylim(
    ax,
    values: Sequence[float | int | None],
    *,
    lower: float | None = None,
    upper: float | None = None,
    min_pad: float = 1.0,
) -> None:
    finite = _finite_values(values)
    if not finite:
        return
    lo = min(finite)
    hi = max(finite)
    span = max(hi - lo, min_pad)
    pad = max(min_pad, span * 0.08)
    y0 = lo - pad
    y1 = hi + pad
    if lower is not None:
        y0 = max(lower, y0)
    if upper is not None:
        y1 = min(upper, y1)
    if y0 < y1:
        ax.set_ylim(y0, y1)


def _apply_bitrate_xaxis(ax, *, log_scale: bool = True) -> None:
    from matplotlib.ticker import FuncFormatter

    if log_scale:
        ax.set_xscale("log")
    ax.xaxis.set_major_formatter(FuncFormatter(_bitrate_tick_label))


def _apply_bitrate_yaxis(ax) -> None:
    from matplotlib.ticker import FuncFormatter

    ax.yaxis.set_major_formatter(FuncFormatter(_bitrate_tick_label))


def compute_pareto_frontier(
    points: Sequence[CodecSweepPoint],
) -> tuple[CodecSweepPoint, ...]:
    """Return the bitrate-minimising frontier across all codecs.

    For each target VMAF that appears in ``points``, pick the ok-row
    with the smallest bitrate (any codec). The frontier is ordered by
    ``target_vmaf`` ascending. Failed / non-finite rows are filtered
    out so the renderer never draws a NaN segment.
    """
    by_target: dict[float, list[CodecSweepPoint]] = {}
    for p in points:
        if not p.ok or _is_missing(p.bitrate_kbps) or _is_missing(p.vmaf_score):
            continue
        by_target.setdefault(p.target_vmaf, []).append(p)
    frontier: list[CodecSweepPoint] = []
    for target in sorted(by_target):
        # Tie-break by codec name so two-codec ties are deterministic.
        winner = min(by_target[target], key=lambda r: (r.bitrate_kbps, r.codec))
        frontier.append(winner)
    return tuple(frontier)


def _has_bisect_samples(points: Sequence[CodecSweepPoint]) -> bool:
    """True when at least one ok point carries a populated samples tuple.

    ADR-0530: gates the new chart variant. Old v2 dumps without samples
    plus all v1 dumps fall through to the legacy connect-the-dots line
    with a caveat note.
    """
    for p in points:
        if p.ok and p.bisect_samples:
            return True
    return False


def _dedup_bisect_samples(
    points: Sequence[CodecSweepPoint],
) -> dict[str, list[BisectSamplePoint]]:
    """Aggregate per-codec bisect samples, de-duplicated by CRF.

    The same codec is bisected once per target — those bisect probes
    overlap (e.g. a target-85 bisect and a target-90 bisect on libx264
    will both visit CRF 24 if that's a midpoint). Two measurements at
    the same CRF on the same source should be close to identical, so
    we keep the first occurrence and drop subsequent duplicates. Sorted
    by bitrate ascending so the rendered curve is monotonic-friendly.
    """
    by_codec: dict[str, dict[int, BisectSamplePoint]] = {}
    for p in points:
        if not p.ok:
            continue
        for s in p.bisect_samples:
            if _is_missing(s.bitrate_kbps) or _is_missing(s.vmaf_score):
                continue
            by_codec.setdefault(p.codec, {}).setdefault(int(s.crf), s)
    out: dict[str, list[BisectSamplePoint]] = {}
    for codec, by_crf in by_codec.items():
        samples = list(by_crf.values())
        samples.sort(key=lambda s: s.bitrate_kbps)
        out[codec] = samples
    return out


def _sweep_plot_fn(data: ReportData):
    """Rate-quality line plot — one curve per codec, pareto highlighted.

    Two modes (ADR-0530):

    - **From-bisect-samples** (schema-v2 with ``bisect_samples``):
      each codec's curve is built from every CRF the underlying
      target-VMAF bisects probed. Sorted by bitrate, drawn as a
      monotonic-friendly line; picked-CRF rows are highlighted as
      larger filled markers on top of the line. Pareto frontier of
      picked-CRF rows is the dashed overlay.
    - **Legacy connect-the-dots** (schema-v1, or v2 without samples):
      one line per codec connecting its picked-CRF rows by target.
      Carries a caveat note in the title because the bisect's
      target-overshoot can flip the apparent VMAF order between
      adjacent targets — that's the bug ADR-0530 fixes for the new
      mode.
    """

    def _plot(ax) -> None:
        if not data.sweep_points:
            ax.text(0.5, 0.5, "no sweep data", ha="center", va="center")
            ax.set_axis_off()
            return
        use_samples = _has_bisect_samples(data.sweep_points)
        plotted_any = False
        if use_samples:
            curves = _dedup_bisect_samples(data.sweep_points)
            picked_by_codec: dict[str, list[CodecSweepPoint]] = {}
            for p in data.sweep_points:
                if not p.ok or _is_missing(p.bitrate_kbps) or _is_missing(p.vmaf_score):
                    continue
                picked_by_codec.setdefault(p.codec, []).append(p)
            for idx, (codec, samples) in enumerate(sorted(curves.items())):
                if not samples:
                    continue
                colour = _codec_colour(codec, idx)
                first_picked = picked_by_codec.get(codec, [])
                ver = first_picked[0].encoder_version if first_picked else ""
                label = f"{codec} ({ver})" if ver else codec
                xs = [s.bitrate_kbps for s in samples]
                ys = [s.vmaf_score for s in samples]
                # Bisect-sample curve: small markers, thin line — the
                # genuine codec R-Q signal.
                ax.plot(
                    xs,
                    ys,
                    marker="o",
                    markersize=3.5,
                    linewidth=1.2,
                    color=colour,
                    alpha=0.85,
                    label=label,
                )
                # Picked-CRF overlay: larger hollow markers so the eye
                # spots them as the actual per-target winners.
                if first_picked:
                    pxs = [p.bitrate_kbps for p in first_picked]
                    pys = [p.vmaf_score for p in first_picked]
                    ax.scatter(
                        pxs,
                        pys,
                        s=80,
                        facecolors="none",
                        edgecolors=colour,
                        linewidths=1.8,
                        zorder=8,
                    )
                plotted_any = True
        else:
            by_codec: dict[str, list[CodecSweepPoint]] = {}
            for p in data.sweep_points:
                by_codec.setdefault(p.codec, []).append(p)
            for idx, (codec, pts) in enumerate(sorted(by_codec.items())):
                ok_pts = [
                    p
                    for p in pts
                    if p.ok and not _is_missing(p.bitrate_kbps) and not _is_missing(p.vmaf_score)
                ]
                if not ok_pts:
                    continue
                ok_pts.sort(key=lambda p: p.target_vmaf)
                xs = [p.bitrate_kbps for p in ok_pts]
                ys = [p.vmaf_score for p in ok_pts]
                label = codec
                ver = ok_pts[0].encoder_version
                if ver:
                    label = f"{codec} ({ver})"
                ax.plot(
                    xs,
                    ys,
                    marker="o",
                    linewidth=1.6,
                    markersize=6,
                    color=_codec_colour(codec, idx),
                    label=label,
                )
                plotted_any = True
        # Pareto frontier overlay (always from the picked-CRF rows).
        frontier = compute_pareto_frontier(data.sweep_points)
        if frontier:
            fxs = [p.bitrate_kbps for p in frontier]
            fys = [p.vmaf_score for p in frontier]
            ax.plot(
                fxs,
                fys,
                color="#000",
                linestyle="--",
                linewidth=2.4,
                alpha=0.55,
                label="pareto frontier",
                zorder=10,
            )
            for p in frontier:
                ax.annotate(
                    f"{p.codec}",
                    xy=(p.bitrate_kbps, p.vmaf_score),
                    xytext=(5, 5),
                    textcoords="offset points",
                    fontsize=7,
                    color="#000",
                    alpha=0.7,
                )
        if not plotted_any:
            ax.text(0.5, 0.5, "no successful sweep rows", ha="center", va="center")
            ax.set_axis_off()
            return
        _apply_bitrate_xaxis(ax, log_scale=True)
        ax.set_xlabel("bitrate (log scale; left is smaller)")
        ax.set_ylabel("VMAF achieved (higher is better)")
        plotted_vmafs = [
            p.vmaf_score for p in data.sweep_points if p.ok and not _is_missing(p.vmaf_score)
        ]
        plotted_vmafs.extend(float(t) for t in data.sweep_targets)
        _set_padded_ylim(ax, plotted_vmafs, lower=0.0, upper=100.0, min_pad=0.75)
        if use_samples:
            ax.set_title("Rate-quality sweep — bisect samples; picked-CRF circled")
        else:
            ax.set_title("Rate-quality sweep — picked rows only; bisect samples unavailable")
            ax.text(
                0.015,
                0.91,
                "Caveat: connect-the-dots may show overshoot.",
                transform=ax.transAxes,
                ha="left",
                va="top",
                fontsize=7,
                color="#333",
                bbox={
                    "boxstyle": "round,pad=0.25",
                    "facecolor": "white",
                    "alpha": 0.75,
                    "lw": 0,
                },
            )
        ax.grid(True, alpha=0.3, which="both")
        # Horizontal reference lines at each sweep target.
        for t in data.sweep_targets:
            ax.axhline(t, color="#888", linestyle=":", alpha=0.35, linewidth=0.8)
            ax.annotate(
                f"target {t:g}",
                xy=(0.995, t),
                xycoords=("axes fraction", "data"),
                xytext=(-4, 0),
                textcoords="offset points",
                ha="right",
                va="center",
                fontsize=7,
                color="#555",
                alpha=0.8,
            )
        ax.text(
            0.015,
            0.985,
            "Read: for the same VMAF, farther left is smaller.",
            transform=ax.transAxes,
            ha="left",
            va="top",
            fontsize=7,
            color="#333",
            bbox={"boxstyle": "round,pad=0.25", "facecolor": "white", "alpha": 0.75, "lw": 0},
        )
        ax.legend(loc="lower right", fontsize=7)

    return _plot


def _shot_plot_fn(data: ReportData):
    def _plot(ax) -> None:
        if not data.shots:
            ax.text(0.5, 0.5, "no per-shot data", ha="center", va="center")
            ax.set_axis_off()
            return
        ax2 = ax.twinx()
        # ADR-0512 Bug B: when the source resolves to a single shot the
        # historical ``ax.step([start], [crf], ...)`` produced a
        # zero-length path that the SVG backend silently dropped, leaving
        # the chart visually empty even though the axes/title rendered.
        # Render each shot as a horizontal band over its frame range so
        # the user always sees a drawable element — for the 1-shot case
        # this becomes a single full-width band labelled with the
        # tuner's pick.
        for shot in data.shots:
            x0 = shot.start_frame
            x1 = shot.end_frame
            ax.hlines(
                shot.best_crf,
                x0,
                x1,
                colors="#1f77b4",
                linewidth=2.5,
            )
            ax2.hlines(
                shot.vmaf,
                x0,
                x1,
                colors="#d62728",
                linewidth=2.5,
                alpha=0.7,
            )
            # Midpoint markers — extra insurance that the segment is
            # visible even when the band collapses on a print-at-small
            # output size.
            mid = (x0 + x1) / 2.0
            ax.plot(mid, shot.best_crf, marker="o", color="#1f77b4", markersize=4)
            ax2.plot(mid, shot.vmaf, marker="o", color="#d62728", markersize=4, alpha=0.7)
        # Provide explicit axis ranges so a single-shot dataset still
        # picks sensible bounds (matplotlib's autoscale degenerates on a
        # single point + hline pair at the same coordinate).
        #
        # ADR-0531 Bug B: use 1.05 * last_end (right side) + 0.02-span
        # left-side padding so the last shot's CRF band is always fully
        # inside the axes viewport. When last_end is exactly on the axes
        # right boundary, matplotlib's clip box trims the rightmost pixel
        # of the hlines artist, making the final shot's band appear
        # invisible at small output sizes.
        last_end = max(shot.end_frame for shot in data.shots)
        first_start = min(shot.start_frame for shot in data.shots)
        x_pad_left = max(1.0, 0.02 * (last_end - first_start))
        x_pad_right = max(1.0, 0.05 * last_end)
        ax.set_xlim(first_start - x_pad_left, last_end + x_pad_right)
        crfs = [shot.best_crf for shot in data.shots]
        vmafs = [shot.vmaf for shot in data.shots]
        if min(crfs) == max(crfs):
            ax.set_ylim(min(crfs) - 2.0, max(crfs) + 2.0)
        if min(vmafs) == max(vmafs):
            ax2.set_ylim(min(vmafs) - 1.0, min(100.0, max(vmafs) + 1.0))
        # Synthetic legend handles — matplotlib's auto-legend skips
        # hline artists in older versions; the proxies keep the legend
        # populated regardless of backend version.
        from matplotlib.lines import Line2D

        ax.legend(
            handles=[Line2D([0], [0], color="#1f77b4", lw=2.5, label="best CRF")],
            loc="upper left",
            fontsize=8,
        )
        ax2.legend(
            handles=[Line2D([0], [0], color="#d62728", lw=2.5, alpha=0.7, label="VMAF")],
            loc="upper right",
            fontsize=8,
        )
        ax.set_xlabel("frame")
        ax.set_ylabel("CRF")
        ax2.set_ylabel("VMAF")
        ax.set_title("Per-shot tuning timeline")
        ax.grid(True, alpha=0.3)

    return _plot


# ---------------------------------------------------------------------------
# Markdown rendering
# ---------------------------------------------------------------------------


def _fmt_bytes(n: int) -> str:
    size = float(n)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if size < 1024 or unit == "TiB":
            return f"{size:.1f} {unit}"
        size /= 1024.0
    return f"{size:.1f} TiB"


_DASH = "—"  # U+2014 em-dash; renderer placeholder for NaN / None.


def _is_missing(v: float | None) -> bool:
    """Return True when ``v`` is ``None`` or a non-finite float.

    Failed bisect rows (Bug #3) propagate ``NaN`` through the compare
    JSON; the renderer formats them as an em-dash so the profile-card
    table no longer prints literal ``nan kbps`` / ``nan`` text
    (Bug #6, BBB e2e 2026-05-17).
    """
    if v is None:
        return True
    return math.isnan(v) or math.isinf(v)


def _fmt_kbps(v: float | None) -> str:
    if _is_missing(v):
        return _DASH
    v = float(v)  # type: ignore[arg-type]
    if v >= 1000:
        return f"{v / 1000:.2f} Mbps"
    return f"{v:.0f} kbps"


def _fmt_duration(s: float | None) -> str:
    if _is_missing(s):
        return _DASH
    s = float(s)  # type: ignore[arg-type]
    if s < 60:
        return f"{s:.1f}s"
    m, sec = divmod(int(s), 60)
    return f"{m}m {sec}s"


def _fmt_vmaf(v: float | None) -> str:
    if v is None or _is_missing(v):
        return _DASH
    return f"{float(v):.2f}"


def _fmt_ms(v: float | None) -> str:
    if v is None or _is_missing(v):
        return _DASH
    return f"{float(v):.0f} ms"


def _fmt_crf(v: int | None) -> str:
    if v is None or v < 0:
        return _DASH
    return str(int(v))


def _quick_takeaways(data: ReportData) -> tuple[str, ...]:
    """Return plain-language report takeaways for non-expert readers."""
    lines: list[str] = []

    frontier = compute_pareto_frontier(data.sweep_points)
    if frontier:
        for point in frontier:
            lines.append(
                f"At VMAF {point.target_vmaf:g}, {point.codec} is the smallest "
                f"successful row: {_fmt_kbps(point.bitrate_kbps)} at CRF "
                f"{_fmt_crf(point.best_crf)}."
            )
    elif data.codec_rows:
        ok_rows = [
            row
            for row in data.codec_rows
            if row.ok and not _is_missing(row.bitrate_kbps) and not _is_missing(row.vmaf_score)
        ]
        if ok_rows:
            best = min(ok_rows, key=lambda row: (row.bitrate_kbps, row.codec))
            lines.append(
                f"Best single-target row: {best.codec} reaches VMAF "
                f"{_fmt_vmaf(best.vmaf_score)} at {_fmt_kbps(best.bitrate_kbps)} "
                f"(CRF {_fmt_crf(best.best_crf)})."
            )

    failed_rows = sum(1 for row in data.codec_rows if not row.ok)
    failed_rows += sum(1 for point in data.sweep_points if not point.ok)
    if failed_rows:
        lines.append(
            f"{failed_rows} encoder/target row(s) failed or were unavailable; "
            "treat the report as degraded coverage, not as a quality loss."
        )

    if data.ladder_rungs:
        rungs = sorted(data.ladder_rungs, key=lambda rung: rung.bitrate_kbps)
        lines.append(
            f"ABR ladder: {len(rungs)} rung(s), from "
            f"{rungs[0].width}x{rungs[0].height} at {_fmt_kbps(rungs[0].bitrate_kbps)} "
            f"to {rungs[-1].width}x{rungs[-1].height} at "
            f"{_fmt_kbps(rungs[-1].bitrate_kbps)}."
        )

    if data.shots:
        crfs = [shot.best_crf for shot in data.shots if shot.best_crf >= 0]
        if crfs:
            lines.append(
                f"Per-shot tuning: {len(data.shots)} shot(s), CRF range "
                f"{min(crfs)}-{max(crfs)}; wider spread means the source benefits "
                "more from shot-aware encoding."
            )
        else:
            lines.append(f"Per-shot tuning: {len(data.shots)} shot(s) detected.")

    if not lines:
        lines.append("No successful encode rows yet; this report only carries source metadata.")

    return tuple(lines)


def _per_codec_targets(
    points: Sequence[CodecSweepPoint], targets: Sequence[float]
) -> dict[str, dict[float, CodecSweepPoint]]:
    """Group sweep points by (codec, target_vmaf) for table rendering."""
    by_codec: dict[str, dict[float, CodecSweepPoint]] = {}
    for p in points:
        by_codec.setdefault(p.codec, {})[p.target_vmaf] = p
    # Ensure every codec has a slot for every requested target so the
    # summary table prints a uniform shape (missing cells render as
    # em-dashes for unavailable encoders / failed bisects).
    for codec in by_codec:
        for t in targets:
            by_codec[codec].setdefault(t, None)  # type: ignore[assignment]
    return by_codec


def _render_sweep_summary_table_md(data: ReportData) -> list[str]:
    """Render the per-codec / per-target summary table as markdown."""
    targets = list(data.sweep_targets) or sorted({p.target_vmaf for p in data.sweep_points})
    by_codec = _per_codec_targets(data.sweep_points, targets)

    header_cells = ["Codec", "Encoder"]
    header_cells.extend(f"bitrate @ VMAF {t:g}" for t in targets)
    header_cells.extend(["CRF picks", "encode time", "Status"])
    align = ["---", "---"] + ["---:" for _ in targets] + ["---", "---:", "---"]

    lines: list[str] = []
    lines.append("| " + " | ".join(header_cells) + " |")
    lines.append("|" + "|".join(align) + "|")
    for codec in sorted(by_codec):
        per_target = by_codec[codec]
        rows = [p for p in per_target.values() if p is not None]
        ok_rows = [p for p in rows if p.ok]
        encoder_version = next((p.encoder_version for p in rows if p.encoder_version), "—")
        cells: list[str] = [_codec_chip_md(codec), _md_cell(encoder_version)]
        for t in targets:
            p = per_target.get(t)
            if p is None or not p.ok or _is_missing(p.bitrate_kbps):
                cells.append(_DASH)
            else:
                cells.append(_fmt_kbps(p.bitrate_kbps))
        cells.append(_md_cell(_sweep_crf_picks(ok_rows)))
        # Encode time per frame: average across the codec's ok rows.
        if ok_rows:
            avg_ms = sum(p.encode_time_ms for p in ok_rows) / float(len(ok_rows))
            cells.append(_fmt_ms(avg_ms))
        else:
            cells.append(_DASH)
        cells.append(_md_cell(_sweep_row_status(list(per_target.values()), targets)))
        lines.append("| " + " | ".join(cells) + " |")
    # Pareto-frontier summary.
    frontier = compute_pareto_frontier(data.sweep_points)
    if frontier:
        lines.append("")
        lines.append("**Pareto frontier** (lowest bitrate at each target):")
        lines.append("")
        for p in frontier:
            lines.append(
                f"- VMAF {p.target_vmaf:g}: `{p.codec}` "
                f"({p.encoder_version or '—'}) "
                f"@ {_fmt_kbps(p.bitrate_kbps)} (CRF {_fmt_crf(p.best_crf)})"
            )
    return lines


def render_markdown(data: ReportData, *, assets_dir: Path | None = None) -> str:
    """Render the report to Markdown.

    If ``assets_dir`` is provided, PNG charts are written under it and
    referenced via relative paths. If omitted, charts are inlined as
    base64 data URLs (less diff-friendly but single-file).
    """
    lines: list[str] = []
    src = data.source

    lines.append(f"# vmaf-tune report — `{_md_cell(Path(src.path).name)}`")
    lines.append("")
    lines.append(
        f"_Generated by vmaf-tune {data.tool_version}{' on ' + data.generated_at_iso if data.generated_at_iso else ''}._"
    )
    lines.append("")
    lines.append("## Source")
    lines.append("")
    lines.append("| Field | Value |")
    lines.append("|---|---|")
    lines.append(f"| Path | `{_md_cell(src.path)}` |")
    lines.append(f"| Resolution | {src.width} × {src.height} |")
    lines.append(f"| Frame rate | {src.fps:.3f} fps |")
    lines.append(f"| Duration | {_fmt_duration(src.duration_s)} ({src.frame_count} frames) |")
    lines.append(f"| Codec | {_md_cell(src.codec)} |")
    lines.append(f"| File size | {_fmt_bytes(src.size_bytes)} |")
    lines.append("")
    lines.append(f"Target VMAF: **{data.target_vmaf:.1f}**")
    lines.append("")
    lines.append("## Quick takeaways")
    lines.append("")
    for takeaway in _quick_takeaways(data):
        lines.append(f"- {_md_cell(takeaway)}")
    lines.append("")

    # Codec comparison table + chart. Schema v2 (sweep_points) takes
    # priority — when both are present (legacy + new in the same
    # report), the sweep view is the useful one and the bar+dot chart
    # is informational only.
    if data.sweep_points:
        lines.append("## Codec rate-quality sweep")
        lines.append("")
        lines.append(_SECTION_GUIDANCE["sweep"])
        lines.append("")
        lines.extend(_render_codec_guide_md(data))
        if lines[-1:] != [""]:
            lines.append("")
        lines.extend(_render_sweep_summary_table_md(data))
        lines.append("")
        lines.append(
            _embed_png(_render_chart(8, 4.5, _sweep_plot_fn(data)), "sweep_rq", assets_dir)
        )
        lines.append("")
    elif data.codec_rows:
        lines.append("## Codec comparison")
        lines.append("")
        lines.append(_SECTION_GUIDANCE["codec"])
        lines.append("")
        lines.extend(_render_codec_guide_md(data))
        if lines[-1:] != [""]:
            lines.append("")
        lines.append("| Codec | Encoder | CRF | Bitrate | Encode time | VMAF | Status |")
        lines.append("|---|---|---:|---:|---:|---:|---|")
        for r in data.codec_rows:
            status = "OK" if r.ok else f"FAIL: {r.error}"
            lines.append(
                f"| {_codec_chip_md(r.codec)} | {_md_cell(r.encoder_version or '—')} | "
                f"{_fmt_crf(r.best_crf)} | "
                f"{_fmt_kbps(r.bitrate_kbps)} | {_fmt_ms(r.encode_time_ms)} | "
                f"{_fmt_vmaf(r.vmaf_score)} | {_md_cell(status)} |"
            )
        lines.append("")
        lines.append(
            _embed_png(_render_chart(7, 3.5, _codec_plot_fn(data)), "codec_compare", assets_dir)
        )
        lines.append("")

    # Ladder
    if data.ladder_rungs or data.ladder_samples:
        lines.append("## ABR ladder")
        lines.append("")
        lines.append(_SECTION_GUIDANCE["ladder"])
        lines.append("")
        if data.ladder_rungs:
            lines.append("Selected rungs:")
            lines.append("")
            lines.append("| Resolution | CRF | Bitrate | VMAF |")
            lines.append("|---|---:|---:|---:|")
            for rung in data.ladder_rungs:
                lines.append(
                    f"| {rung.width}×{rung.height} | {_fmt_crf(rung.crf)} | "
                    f"{_fmt_kbps(rung.bitrate_kbps)} | {_fmt_vmaf(rung.vmaf)} |"
                )
            lines.append("")
        lines.append(_embed_png(_render_chart(7, 4, _ladder_plot_fn(data)), "ladder", assets_dir))
        lines.append("")

    # Per-shot
    if data.shots:
        lines.append("## Per-shot tuning")
        lines.append("")
        lines.append(_SECTION_GUIDANCE["shots"])
        lines.append("")
        lines.append(f"{len(data.shots)} shots detected.")
        lines.append("")
        lines.append("| # | Frames | Duration | Resolution | Best CRF | VMAF | Bitrate |")
        lines.append("|---:|---:|---:|---|---:|---:|---:|")
        for s in data.shots:
            lines.append(
                f"| {s.shot_index} | {s.start_frame}–{s.end_frame} | "
                f"{_fmt_duration(s.duration_s)} | {s.width}×{s.height} | "
                f"{_fmt_crf(s.best_crf)} | {_fmt_vmaf(s.vmaf)} | "
                f"{_fmt_kbps(s.bitrate_kbps)} |"
            )
        lines.append("")
        lines.append(_embed_png(_render_chart(7, 3.5, _shot_plot_fn(data)), "shots", assets_dir))
        lines.append("")

    lines.append("---")
    lines.append("")
    lines.append("Raw JSON dump for downstream tooling:")
    lines.append("")
    lines.append(_SECTION_GUIDANCE["profile"])
    lines.append("")
    lines.append("<details><summary>report.json</summary>")
    lines.append("")
    lines.append("```json")
    lines.append(_portable_json_dump(data.to_dict()))
    lines.append("```")
    lines.append("")
    lines.append("</details>")
    return "\n".join(lines) + "\n"


def _embed_png(png: bytes, name: str, assets_dir: Path | None) -> str:
    if not png:
        # Matplotlib unavailable (ADR-0498 fallback); skip the image
        # rather than emit a broken inline data URI.
        return f"<!-- chart {name} unavailable (matplotlib not installed) -->"
    if assets_dir is None:
        b64 = base64.b64encode(png).decode("ascii")
        return f"![{name}](data:image/png;base64,{b64})"
    assets_dir.mkdir(parents=True, exist_ok=True)
    target = assets_dir / f"{name}.png"
    target.write_bytes(png)
    return f"![{name}]({target.relative_to(assets_dir.parent) if assets_dir.parent in target.parents else target.name})"


# ---------------------------------------------------------------------------
# HTML rendering
# ---------------------------------------------------------------------------


_HTML_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>vmaf-tune report — {source_name}</title>
<style>
:root {{
    --bg: #0f1115; --panel: #161823; --text: #e6e7ea; --muted: #c2c5cc;
    --accent: #f5792a; --link: #9bd2ff; --ok: #4caf50; --bad: #ef5350;
}}
@media (prefers-color-scheme: light) {{
    :root {{
        --bg: #fbfbfd; --panel: #fff; --text: #1d2433;
        --muted: #51607a; --accent: #ee5a24; --link: #005bbb;
        --ok: #2e7d32; --bad: #c62828;
    }}
}}
body {{ background: var(--bg); color: var(--text); font: 16px/1.5 -apple-system, "Segoe UI", system-ui, sans-serif;
        max-width: 1100px; margin: 0 auto; padding: 2rem 1.25rem; }}
h1 {{ color: var(--accent); border-bottom: 2px solid var(--accent); padding-bottom: .4rem; }}
h2 {{ margin-top: 2rem; }}
h3 {{ margin-bottom: .35rem; }}
.panel {{ background: var(--panel); border-radius: 8px; padding: 1.2rem; margin: 1rem 0; }}
.guide {{ border-left: 4px solid var(--accent); padding: .75rem 1rem; margin: .75rem 0 1rem; background: rgba(245,121,42,.08); }}
.guide p {{ margin: .25rem 0 .5rem; color: var(--muted); }}
.guide ul {{ margin: .5rem 0 0; padding-left: 1.1rem; }}
.note {{ color: var(--muted); margin: .25rem 0 1rem; }}
.table-scroll {{ overflow-x: auto; -webkit-overflow-scrolling: touch; }}
table {{ width: 100%; border-collapse: collapse; margin: .5rem 0; }}
th, td {{ padding: .4rem .6rem; text-align: left; border-bottom: 1px solid #2d3142; }}
th {{ color: var(--muted); font-weight: 600; font-size: .85rem; text-transform: uppercase; letter-spacing: .03em; }}
td.num, th.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
tr.failed {{ opacity: .7; }}
.codec-chip {{ --codec-colour: var(--accent); display: inline-flex; align-items: center; gap: .35rem;
    color: var(--text); text-decoration: none; white-space: nowrap; }}
.codec-logo {{ background: var(--codec-colour); color: #fff; border-radius: 4px; padding: .08rem .32rem;
    font-weight: 700; font-size: .72rem; letter-spacing: .02em; line-height: 1.35; }}
.codec-name {{ font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: .92em; }}
.tag {{ display: inline-block; padding: .15em .5em; border-radius: 3px; font-size: .85em; }}
.tag.ok {{ background: rgba(76,175,80,.18); color: var(--ok); }}
.tag.bad {{ background: rgba(239,83,80,.18); color: var(--bad); }}
.chart {{ background: var(--panel); border-radius: 8px; padding: .5rem; overflow-x: auto;
          -webkit-overflow-scrolling: touch; min-width: 0; }}
.chart svg {{ max-width: 100%; height: auto; display: block; margin: 0 auto; min-width: 320px; }}
.meta {{ color: var(--muted); font-size: .9rem; margin-bottom: 1.5rem; }}
details {{ background: var(--panel); padding: .8rem 1.2rem; border-radius: 8px; margin-top: 2rem; }}
details summary {{ cursor: pointer; color: var(--muted); }}
pre {{ background: #000; color: #ddd; padding: 1rem; border-radius: 6px; overflow-x: auto;
       overflow-y: auto; max-height: 28rem; font-size: .75rem; }}
.kv {{ display: grid; grid-template-columns: 12rem 1fr; gap: .3rem 1rem; }}
@media (max-width: 480px) {{ .kv {{ grid-template-columns: 8rem 1fr; }} }}
.kv .key {{ color: var(--muted); }}
.target {{ color: var(--accent); font-size: 1.1em; font-weight: 600; }}
</style>
</head>
<body>
<h1>vmaf-tune report — {source_name}</h1>
<div class="meta">Generated by vmaf-tune {tool_version}{generated_at}.</div>

<div class="panel">
<h2 style="margin-top:0">Source</h2>
<div class="kv">
<div class="key">Path</div><div><code>{source_path}</code></div>
<div class="key">Resolution</div><div>{width} × {height}</div>
<div class="key">Frame rate</div><div>{fps:.3f} fps</div>
<div class="key">Duration</div><div>{duration} ({frame_count} frames)</div>
<div class="key">Codec</div><div>{codec}</div>
<div class="key">File size</div><div>{size}</div>
<div class="key">Target VMAF</div><div class="target">{target_vmaf:.1f}</div>
</div>
</div>

{takeaways_section}
{codec_section}
{ladder_section}
{shots_section}

<details>
<summary>Raw JSON dump (for downstream tooling)</summary>
<p class="note">{profile_guidance}</p>
<pre>{json_dump}</pre>
</details>
</body>
</html>
"""


def _row_html(row: CodecRow) -> str:
    status = (
        '<span class="tag ok">OK</span>'
        if row.ok
        else f'<span class="tag bad">{_html_escape(row.error or "fail")}</span>'
    )
    # Apply the 'failed' CSS class so failed rows are visually dimmed in the
    # table even when no chart is present. The class sets opacity to 0.7 so
    # the row remains readable but is clearly de-emphasised.
    row_class = "" if row.ok else " class='failed'"
    return (
        f"<tr{row_class}><td>{_codec_chip_html(row.codec)}</td>"
        f"<td>{_html_escape(row.encoder_version or '—')}</td>"
        f"<td class='num'>{_fmt_crf(row.best_crf)}</td>"
        f"<td class='num'>{_fmt_kbps(row.bitrate_kbps)}</td>"
        f"<td class='num'>{_fmt_ms(row.encode_time_ms)}</td>"
        f"<td class='num'>{_fmt_vmaf(row.vmaf_score)}</td>"
        f"<td>{status}</td></tr>"
    )


def _sweep_summary_table_html(data: ReportData) -> str:
    """Render the per-codec / per-target summary table as HTML."""
    targets = list(data.sweep_targets) or sorted({p.target_vmaf for p in data.sweep_points})
    by_codec = _per_codec_targets(data.sweep_points, targets)

    head_cells = ["Codec", "Encoder"]
    head_cells.extend(f"@ VMAF {t:g}" for t in targets)
    head_cells.extend(["CRF picks", "Encode time", "Status"])
    # Apply th.num to numeric columns: per-target bitrate columns (index 2
    # through 2+len(targets)-1) and "Encode time" (second-to-last column).
    _num_head_indices: set[int] = set(range(2, 2 + len(targets))) | {len(head_cells) - 2}
    head = "".join(
        (
            f"<th class='num'>{_html_escape(c)}</th>"
            if i in _num_head_indices
            else f"<th>{_html_escape(c)}</th>"
        )
        for i, c in enumerate(head_cells)
    )

    body_rows: list[str] = []
    for codec in sorted(by_codec):
        per_target = by_codec[codec]
        rows = [p for p in per_target.values() if p is not None]
        ok_rows = [p for p in rows if p.ok]
        encoder_version = next((p.encoder_version for p in rows if p.encoder_version), "—")
        cells: list[str] = [
            f"<td>{_codec_chip_html(codec)}</td>",
            f"<td>{_html_escape(encoder_version)}</td>",
        ]
        for t in targets:
            p = per_target.get(t)
            if p is None or not p.ok or _is_missing(p.bitrate_kbps):
                cells.append(f"<td class='num'>{_DASH}</td>")
            else:
                cells.append(f"<td class='num'>{_fmt_kbps(p.bitrate_kbps)}</td>")
        cells.append(f"<td>{_html_escape(_sweep_crf_picks(ok_rows))}</td>")
        if ok_rows:
            avg_ms = sum(p.encode_time_ms for p in ok_rows) / float(len(ok_rows))
            cells.append(f"<td class='num'>{_fmt_ms(avg_ms)}</td>")
        else:
            cells.append(f"<td class='num'>{_DASH}</td>")
        status = _sweep_row_status(list(per_target.values()), targets)
        tag_class = "ok" if status == "OK" else "bad"
        cells.append(f"<td><span class='tag {tag_class}'>{_html_escape(status)}</span></td>")
        # Dim the entire row when the codec has at least one failure so the
        # reader's eye is not pulled to failed rows as potential winners.
        row_class = " class='failed'" if tag_class == "bad" else ""
        body_rows.append(f"<tr{row_class}>" + "".join(cells) + "</tr>")

    table = (
        "<div class='table-scroll'><table><thead><tr>"
        f"{head}</tr></thead><tbody>{''.join(body_rows)}</tbody></table></div>"
    )

    frontier = compute_pareto_frontier(data.sweep_points)
    frontier_html = ""
    if frontier:
        items = "".join(
            f"<li>VMAF {p.target_vmaf:g}: {_codec_chip_html(p.codec)} "
            f"({_html_escape(p.encoder_version or '—')}) "
            f"@ {_fmt_kbps(p.bitrate_kbps)} (CRF {_fmt_crf(p.best_crf)})</li>"
            for p in frontier
        )
        frontier_html = (
            "<p><strong>Pareto frontier</strong> "
            "(lowest bitrate at each target):</p>"
            f"<ul>{items}</ul>"
        )
    return table + frontier_html


def render_html(data: ReportData) -> str:
    """Render the report to a single self-contained HTML file."""
    src = data.source
    takeaways = "".join(f"<li>{_html_escape(takeaway)}</li>" for takeaway in _quick_takeaways(data))
    takeaways_section = (
        "<div class='panel'><h2 style='margin-top:0'>Quick takeaways</h2>"
        f"<ul>{takeaways}</ul></div>"
    )
    codec_section = ""
    if data.sweep_points:
        # Schema v2 — rate-quality sweep takes priority over the
        # legacy bar+dot view when both are present (the latter is at
        # most as informative as the sweep restricted to one target).
        table_html = _sweep_summary_table_html(data)
        chart = _render_chart_svg(8, 4.5, _sweep_plot_fn(data))
        codec_section = (
            f"<div class='panel'><h2 style='margin-top:0'>Codec rate-quality sweep</h2>"
            f"<p class='note'>{_html_escape(_SECTION_GUIDANCE['sweep'])}</p>"
            f"{_render_codec_guide_html(data)}"
            f"{table_html}"
            f"<div class='chart'>{chart}</div></div>"
        )
    elif data.codec_rows:
        rows = "\n".join(_row_html(r) for r in data.codec_rows)
        chart = _render_chart_svg(7, 3.5, _codec_plot_fn(data))
        codec_section = (
            f"<div class='panel'><h2 style='margin-top:0'>Codec comparison</h2>"
            f"<p class='note'>{_html_escape(_SECTION_GUIDANCE['codec'])}</p>"
            f"{_render_codec_guide_html(data)}"
            f"<div class='table-scroll'><table><thead><tr><th>Codec</th><th>Encoder</th>"
            f"<th class='num'>CRF</th><th class='num'>Bitrate</th>"
            f"<th class='num'>Encode time</th><th class='num'>VMAF</th><th>Status</th></tr></thead>"
            f"<tbody>{rows}</tbody></table></div>"
            f"<div class='chart'>{chart}</div></div>"
        )

    ladder_section = ""
    if data.ladder_rungs or data.ladder_samples:
        rungs_html = ""
        if data.ladder_rungs:
            rungs = "".join(
                f"<tr><td>{r.width}×{r.height}</td>"
                f"<td class='num'>{_fmt_crf(r.crf)}</td>"
                f"<td class='num'>{_fmt_kbps(r.bitrate_kbps)}</td>"
                f"<td class='num'>{_fmt_vmaf(r.vmaf)}</td></tr>"
                for r in data.ladder_rungs
            )
            rungs_html = (
                f"<div class='table-scroll'><table><thead><tr><th>Resolution</th><th>CRF</th>"
                f"<th>Bitrate</th><th>VMAF</th></tr></thead>"
                f"<tbody>{rungs}</tbody></table></div>"
            )
        chart = _render_chart_svg(8, 4, _ladder_plot_fn(data))
        ladder_section = (
            f"<div class='panel'><h2 style='margin-top:0'>ABR ladder</h2>"
            f"<p class='note'>{_html_escape(_SECTION_GUIDANCE['ladder'])}</p>"
            f"{rungs_html}<div class='chart'>{chart}</div></div>"
        )

    shots_section = ""
    if data.shots:
        rows = "".join(
            f"<tr><td class='num'>{s.shot_index}</td>"
            f"<td>{s.start_frame}–{s.end_frame}</td>"
            f"<td class='num'>{_fmt_duration(s.duration_s)}</td>"
            f"<td>{s.width}×{s.height}</td>"
            f"<td class='num'>{_fmt_crf(s.best_crf)}</td>"
            f"<td class='num'>{_fmt_vmaf(s.vmaf)}</td>"
            f"<td class='num'>{_fmt_kbps(s.bitrate_kbps)}</td></tr>"
            for s in data.shots
        )
        chart = _render_chart_svg(8, 3.5, _shot_plot_fn(data))
        shots_section = (
            f"<div class='panel'><h2 style='margin-top:0'>Per-shot tuning ({len(data.shots)} shots)</h2>"
            f"<p class='note'>{_html_escape(_SECTION_GUIDANCE['shots'])}</p>"
            f"<div class='table-scroll'><table><thead><tr><th>#</th><th>Frames</th><th>Duration</th>"
            f"<th>Resolution</th><th>Best CRF</th><th>VMAF</th><th>Bitrate</th></tr></thead>"
            f"<tbody>{rows}</tbody></table></div>"
            f"<div class='chart'>{chart}</div></div>"
        )

    return _HTML_TEMPLATE.format(
        source_name=_html_escape(Path(src.path).name),
        source_path=_html_escape(src.path),
        width=src.width,
        height=src.height,
        fps=src.fps,
        duration=_fmt_duration(src.duration_s),
        frame_count=src.frame_count,
        codec=_html_escape(src.codec),
        size=_fmt_bytes(src.size_bytes),
        target_vmaf=data.target_vmaf,
        tool_version=_html_escape(data.tool_version),
        generated_at=f" on {_html_escape(data.generated_at_iso)}" if data.generated_at_iso else "",
        takeaways_section=takeaways_section,
        codec_section=codec_section,
        ladder_section=ladder_section,
        shots_section=shots_section,
        profile_guidance=_html_escape(_SECTION_GUIDANCE["profile"]),
        json_dump=_html_escape(_portable_json_dump(data.to_dict())),
    )


# ---------------------------------------------------------------------------
# ffprobe helper
# ---------------------------------------------------------------------------


def probe_source(path: Path) -> SourceInfo:
    """Run ffprobe and return a :class:`SourceInfo`.

    Falls back to zero-filled values when ffprobe is unavailable; the
    caller should treat the result as best-effort.
    """
    import subprocess

    try:
        out = subprocess.check_output(
            [
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=width,height,r_frame_rate,nb_frames,codec_name,duration",
                "-show_entries",
                "format=duration,size",
                "-of",
                "json",
                str(path),
            ],
            stderr=subprocess.STDOUT,
            timeout=30,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired):
        size = path.stat().st_size if path.exists() else 0
        return SourceInfo(
            path=str(path),
            width=0,
            height=0,
            fps=0.0,
            duration_s=0.0,
            frame_count=0,
            codec="unknown",
            size_bytes=size,
        )

    info = json.loads(out)
    stream = (info.get("streams") or [{}])[0]
    fmt = info.get("format") or {}
    fps_str = stream.get("r_frame_rate", "0/1")
    try:
        num, den = fps_str.split("/")
        fps = float(num) / float(den) if float(den) else 0.0
    except (ValueError, ZeroDivisionError):
        fps = 0.0
    duration = float(stream.get("duration") or fmt.get("duration") or 0.0)
    frame_count = int(stream.get("nb_frames") or 0) or int(duration * fps)
    return SourceInfo(
        path=str(path),
        width=int(stream.get("width") or 0),
        height=int(stream.get("height") or 0),
        fps=fps,
        duration_s=duration,
        frame_count=frame_count,
        codec=stream.get("codec_name") or "unknown",
        size_bytes=int(fmt.get("size") or (path.stat().st_size if path.exists() else 0)),
    )


__all__ = [
    "ENCODER_PROFILE_SCHEMA",
    "BisectSamplePoint",
    "CodecRow",
    "CodecSweepPoint",
    "LadderRung",
    "LadderSample",
    "ReportData",
    "ShotRow",
    "SourceInfo",
    "build_encoder_profile",
    "codec_metadata",
    "compute_pareto_frontier",
    "probe_source",
    "render_html",
    "render_markdown",
]
