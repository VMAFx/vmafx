# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Pre-deploy audit for C3 (learned-filter) ONNX models.

A learned filter is an ONNX model that takes a single-channel frame
(typically luma) and emits a "cleaned" frame of the same shape. If that
filter was trained on type-A content and you feed it type-B, the output
might look plausible in spot checks but quietly destroy structure
(low SSIM) or clip at the codec's peak. This module runs the filter
over a corpus of representative frames and flags any of four failure
modes before the model is shipped.

Scope note: this is a pre-deploy Python QA tool, not a runtime vmaf
feature extractor. We decided against a core/src/feature/
learned_filter_audit.c plugin because the audit only needs to run
once per model release, not per frame in production — the cost of
wiring a new C feature extractor + dispatch integration exceeds the
value.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np

DEFAULT_SSIM_MIN = 0.6
DEFAULT_MEAN_SHIFT_MAX = 0.05  # |Δmean| > 5% of the peak is suspicious
DEFAULT_STD_RATIO_MAX = 2.0  # filter must not amplify noise > 2×
DEFAULT_CLIP_FRACTION_MAX = 0.01  # >1% of output pixels at the peak


@dataclass
class FrameStats:
    mean_input: float
    mean_output: float
    mean_shift: float  # output - input
    std_input: float
    std_output: float
    std_ratio: float  # std_output / std_input
    ssim: float
    clip_fraction: float  # fraction of output pixels at min or max
    max_abs_delta: float


@dataclass
class LearnedFilterAuditReport:
    model: Path
    n_frames: int
    peak: float
    frames: list[FrameStats] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    # Summary stats across the corpus — useful for CI gates.
    avg_ssim: float = 0.0
    max_mean_shift: float = 0.0
    max_std_ratio: float = 0.0
    max_clip_fraction: float = 0.0

    @property
    def ok(self) -> bool:
        return not self.warnings

    def to_dict(self) -> dict:
        d = asdict(self)
        d["model"] = str(self.model)
        d["ok"] = self.ok
        return d


def _ssim(a: np.ndarray, b: np.ndarray, peak: float, k1: float = 0.01, k2: float = 0.03) -> float:
    """Single-channel SSIM over the full image (mean + variance only)."""
    mu_a = a.mean()
    mu_b = b.mean()
    var_a = a.var()
    var_b = b.var()
    cov = ((a - mu_a) * (b - mu_b)).mean()
    c1 = (k1 * peak) ** 2
    c2 = (k2 * peak) ** 2
    num = (2 * mu_a * mu_b + c1) * (2 * cov + c2)
    den = (mu_a**2 + mu_b**2 + c1) * (var_a + var_b + c2)
    return float(num / den) if den > 0 else 0.0


def audit_learned_filter(
    model: Path,
    frames: list[np.ndarray],
    peak: float = 1.0,
    input_name: str = "input",
    ssim_min: float = DEFAULT_SSIM_MIN,
    mean_shift_max: float = DEFAULT_MEAN_SHIFT_MAX,
    std_ratio_max: float = DEFAULT_STD_RATIO_MAX,
    clip_fraction_max: float = DEFAULT_CLIP_FRACTION_MAX,
) -> LearnedFilterAuditReport:
    """Run @p model over @p frames and flag the four failure modes.

    Each frame must be a 2-D float32 array scaled to [0, @p peak]
    (typically peak=1.0 after dividing the 8-bit luma by 255). The
    filter is expected to accept ``(1, 1, H, W)`` and return the same
    shape.
    """
    import onnxruntime as ort

    if not frames:
        raise ValueError("audit_learned_filter needs at least one frame")

    sess = ort.InferenceSession(str(model), providers=["CPUExecutionProvider"])
    report = LearnedFilterAuditReport(model=model, n_frames=len(frames), peak=peak)

    for idx, frame in enumerate(frames):
        if frame.ndim != 2:
            raise ValueError(f"frame {idx} must be 2-D (H, W); got shape {frame.shape}")
        x = frame.astype(np.float32)[None, None, :, :]
        out = np.asarray(sess.run(None, {input_name: x})[0]).reshape(*frame.shape)

        mu_in = float(frame.mean())
        mu_out = float(out.mean())
        std_in = float(frame.std()) or 1e-9
        std_out = float(out.std())
        clip = float(np.mean((out <= 0.0) | (out >= peak)))
        ssim = _ssim(frame.astype(np.float64), out.astype(np.float64), peak=peak)

        stats = FrameStats(
            mean_input=mu_in,
            mean_output=mu_out,
            mean_shift=mu_out - mu_in,
            std_input=std_in,
            std_output=std_out,
            std_ratio=std_out / std_in,
            ssim=ssim,
            clip_fraction=clip,
            max_abs_delta=float(np.abs(out - frame).max()),
        )
        report.frames.append(stats)

        if abs(stats.mean_shift) > mean_shift_max * peak:
            report.warnings.append(
                f"frame {idx}: |Δmean| = {abs(stats.mean_shift):.3g} "
                f"> {mean_shift_max * peak:.3g}"
            )
        if stats.std_ratio > std_ratio_max:
            report.warnings.append(
                f"frame {idx}: std_ratio = {stats.std_ratio:.2f}× (filter amplifies noise)"
            )
        if stats.clip_fraction > clip_fraction_max:
            report.warnings.append(
                f"frame {idx}: {stats.clip_fraction * 100:.1f}% of output clipped "
                f"(threshold {clip_fraction_max * 100:.0f}%)"
            )
        if stats.ssim < ssim_min:
            report.warnings.append(
                f"frame {idx}: SSIM = {stats.ssim:.2f} < {ssim_min} "
                f"(filter destroying structure)"
            )

    # Summary stats for a single-line CI gate.
    ssims = [f.ssim for f in report.frames]
    report.avg_ssim = float(np.mean(ssims)) if ssims else 0.0
    report.max_mean_shift = max((abs(f.mean_shift) for f in report.frames), default=0.0)
    report.max_std_ratio = max((f.std_ratio for f in report.frames), default=0.0)
    report.max_clip_fraction = max((f.clip_fraction for f in report.frames), default=0.0)
    return report


def render_table(report: LearnedFilterAuditReport) -> str:
    lines = [
        f"model: {report.model.name}  n_frames: {report.n_frames}  peak: {report.peak:g}",
        f"avg SSIM: {report.avg_ssim:.3f}  max |Δmean|: {report.max_mean_shift:.3g}  "
        f"max std ratio: {report.max_std_ratio:.2f}×  max clip: "
        f"{report.max_clip_fraction * 100:.1f}%",
        "-" * 76,
        f"{'frame':>6} {'SSIM':>7} {'Δmean':>9} {'σ ratio':>9} {'clip %':>8}",
    ]
    for i, f in enumerate(report.frames):
        lines.append(
            f"{i:>6} {f.ssim:>7.3f} {f.mean_shift:>+9.3g} "
            f"{f.std_ratio:>9.2f} {f.clip_fraction * 100:>8.2f}"
        )
    if report.warnings:
        lines.append("")
        for w in report.warnings:
            lines.append(f"  WARN: {w}")
    return "\n".join(lines)
