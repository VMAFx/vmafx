# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""argparse entry-point for ``vmaf-roi-score``.

Option C drives the ``vmaf`` binary twice (full-frame +
saliency-masked) and emits a JSON record with both pooled scores plus
the saliency-weighted blend. Synthetic-mode (``--synthetic-mask``)
keeps the combine-math smoke independent of ONNX Runtime.
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from . import ROI_RESULT_KEYS, SCHEMA_VERSION, __version__, blend_scores
from .defaultmodel import DEFAULT_MODEL
from .score import ScoreRequest, ScoreResult, run_score

# Exit status for a saliency-mask materialisation failure. Distinct from any
# `vmaf` binary exit status so a wrapper can tell the two apart.
_MASK_FAILURE_EXIT = 64
_INVALID_SCORE_EXIT = 65


def _add_source_args(parser: argparse.ArgumentParser) -> None:
    """Register the raw-YUV pair and its geometry."""
    parser.add_argument(
        "--reference",
        type=Path,
        required=True,
        help="raw reference YUV",
    )
    parser.add_argument(
        "--distorted",
        type=Path,
        required=True,
        help="raw distorted YUV",
    )
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument(
        "--pix-fmt",
        default="yuv420p",
        help="ffmpeg pix_fmt (default yuv420p)",
    )


def _add_saliency_args(parser: argparse.ArgumentParser) -> None:
    """Register the mask-source selector and the combine-math knobs."""
    parser.add_argument(
        "--saliency-model",
        type=Path,
        default=None,
        help=(
            "path to saliency ONNX (e.g. model/tiny/saliency_student_v1.onnx "
            "once PR #359 lands; falls back to mobilesal.onnx today). "
            "Mutually exclusive with --synthetic-mask."
        ),
    )
    parser.add_argument(
        "--synthetic-mask",
        type=float,
        default=None,
        metavar="FILL",
        help=(
            "skip ONNX inference; use a constant-value mask for testing. "
            "Value in [0, 1]. The combine math still runs; the masked "
            "VMAF run uses the same distorted YUV (no mask applied)."
        ),
    )
    parser.add_argument(
        "--weight",
        type=float,
        default=0.5,
        help="saliency-masked component weight in [0, 1] (default 0.5)",
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=0.3,
        help="saliency threshold for --saliency-model masking in [0, 1] (default 0.3)",
    )
    parser.add_argument(
        "--fade",
        type=float,
        default=0.1,
        help="saliency fade band above --threshold in [0, 1] (default 0.1)",
    )


def _add_runner_args(parser: argparse.ArgumentParser) -> None:
    """Register the `vmaf` binary selection and the result sink."""
    parser.add_argument(
        "--model",
        default=DEFAULT_MODEL,
        help="VMAF model version passed to the underlying vmaf CLI",
    )
    parser.add_argument(
        "--vmaf-bin",
        default="vmaf",
        help="path to the libvmaf CLI binary (default: vmaf on PATH)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="write JSON result to this file; default writes to stdout",
    )


def _build_parser() -> argparse.ArgumentParser:
    """Assemble the full parser.

    The three ``_add_*_args`` helpers are called in the order their
    options must appear in ``--help``; argparse renders options in
    registration order, so reordering these calls is a user-visible
    change.
    """
    parser = argparse.ArgumentParser(
        prog="vmaf-roi-score",
        description=(
            "Region-of-interest VMAF score (Option C). Combines "
            "a full-frame VMAF run with a saliency-masked VMAF run via a "
            "user-supplied weight. Useful for content where bad "
            "background should not penalise a good salient region. "
            "Distinct from the core/tools/vmaf_roi binary (ADR-0247), "
            "which emits encoder QP-offset sidecars."
        ),
    )
    parser.add_argument("--version", action="version", version=__version__)
    _add_source_args(parser)
    _add_saliency_args(parser)
    _add_runner_args(parser)
    return parser


def _validate(ns: argparse.Namespace) -> None:
    """Hand-rolled cross-arg validation; argparse-only is too coarse."""
    if ns.saliency_model is None and ns.synthetic_mask is None:
        raise SystemExit("vmaf-roi-score: one of --saliency-model or --synthetic-mask is required")
    if ns.saliency_model is not None and ns.synthetic_mask is not None:
        raise SystemExit(
            "vmaf-roi-score: --saliency-model and --synthetic-mask are mutually exclusive"
        )
    if ns.saliency_model is not None and not ns.saliency_model.exists():
        raise SystemExit(f"vmaf-roi-score: saliency model not found: {ns.saliency_model}")
    if not ns.reference.exists():
        raise SystemExit(f"vmaf-roi-score: reference not found: {ns.reference}")
    if not ns.distorted.exists():
        raise SystemExit(f"vmaf-roi-score: distorted not found: {ns.distorted}")
    if not (0.0 <= ns.weight <= 1.0):
        raise SystemExit(f"vmaf-roi-score: --weight must be in [0, 1], got {ns.weight}")
    if ns.synthetic_mask is not None and not (0.0 <= ns.synthetic_mask <= 1.0):
        raise SystemExit(
            f"vmaf-roi-score: --synthetic-mask must be in [0, 1], got {ns.synthetic_mask}"
        )
    if not (0.0 <= ns.threshold <= 1.0):
        raise SystemExit(f"vmaf-roi-score: --threshold must be in [0, 1], got {ns.threshold}")
    if not (0.0 <= ns.fade <= 1.0):
        raise SystemExit(f"vmaf-roi-score: --fade must be in [0, 1], got {ns.fade}")


class _MaskError(Exception):
    """Saliency-mask materialisation failed.

    Deliberately not a ``RuntimeError``: :func:`_materialise_mask` catches
    ``RuntimeError`` from the mask backend, and a sibling base class keeps
    this wrapper from being swallowed by that handler.
    """


def _score_request(ns: argparse.Namespace, distorted: Path) -> ScoreRequest:
    """Build the scoring request for `distorted`; geometry comes from the CLI."""
    return ScoreRequest(
        reference=ns.reference,
        distorted=distorted,
        width=ns.width,
        height=ns.height,
        pix_fmt=ns.pix_fmt,
        model=ns.model,
    )


def _materialise_mask(ns: argparse.Namespace, masked_yuv: Path) -> None:
    """Write the saliency-masked distorted YUV to `masked_yuv`.

    ``vmafroiscore.mask`` is imported lazily: it lives behind the
    ``[runtime]`` extra (onnxruntime + numpy), and ``--synthetic-mask``
    must stay usable without it. Raises :class:`_MaskError` on any
    backend failure.
    """
    try:
        from .mask import MaskRequest, apply_saliency_mask, synthesise_uniform_mask

        inference = None
        saliency_model = ns.saliency_model if ns.saliency_model is not None else Path("synthetic")
        if ns.synthetic_mask is not None:
            fill = float(ns.synthetic_mask)
            saliency_model = Path("synthetic")

            def _synthetic_mask(_rgb: bytes, width: int, height: int) -> list[list[float]]:
                return synthesise_uniform_mask(width, height, fill=fill)

            inference = _synthetic_mask

        apply_saliency_mask(
            MaskRequest(
                reference=ns.reference,
                distorted=ns.distorted,
                output=masked_yuv,
                width=ns.width,
                height=ns.height,
                pix_fmt=ns.pix_fmt,
                saliency_model=saliency_model,
                threshold=ns.threshold,
                fade=ns.fade,
            ),
            inference=inference,
        )
    except (ImportError, RuntimeError, ValueError) as exc:
        raise _MaskError(f"vmaf-roi-score: saliency mask failed: {exc}") from exc


def _score_masked(ns: argparse.Namespace) -> ScoreResult:
    """Materialise the mask into a temp dir and score the distorted pair against it.

    The `vmaf` run stays inside the ``TemporaryDirectory`` block so the
    masked YUV still exists when the binary reads it; only the result
    outlives the scratch directory.
    """
    with tempfile.TemporaryDirectory(prefix="vmaf_roi_score_") as tmp:
        masked_yuv = Path(tmp) / "distorted.saliency-masked.yuv"
        _materialise_mask(ns, masked_yuv)
        return run_score(_score_request(ns, masked_yuv), vmaf_bin=ns.vmaf_bin)


def _build_payload(
    ns: argparse.Namespace, full: ScoreResult, masked: ScoreResult, roi: float
) -> dict[str, object]:
    """Assemble the JSON record, pinned to the canonical key order."""
    payload: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "vmaf_full": full.vmaf_score,
        "vmaf_masked": masked.vmaf_score,
        "weight": ns.weight,
        "vmaf_roi": roi,
        "model": ns.model,
        "saliency_model": "synthetic" if ns.synthetic_mask is not None else str(ns.saliency_model),
        "reference": str(ns.reference),
        "distorted": str(ns.distorted),
    }
    # Pin key order to the canonical schema; tests assert on this.
    return {k: payload[k] for k in ROI_RESULT_KEYS}


def _emit(ns: argparse.Namespace, payload: dict[str, object]) -> None:
    """Write the record to `--output`, or to stdout when it is unset."""
    text = json.dumps(payload, indent=2, allow_nan=False)
    if ns.output is None:
        sys.stdout.write(text + "\n")
    else:
        ns.output.write_text(text + "\n", encoding="utf-8")


def _report_run_failure(label: str, result: ScoreResult) -> int:
    """Print the failing `vmaf` run's stderr tail and hand back its exit status."""
    sys.stderr.write(
        f"vmaf-roi-score: {label} vmaf run failed (exit={result.exit_status}); "
        f"stderr tail:\n{result.stderr_tail}\n"
    )
    return result.exit_status


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    ns = parser.parse_args(argv)
    _validate(ns)

    full = run_score(_score_request(ns, ns.distorted), vmaf_bin=ns.vmaf_bin)
    if full.exit_status != 0:
        return _report_run_failure("full-frame", full)

    try:
        masked = _score_masked(ns)
    except _MaskError as exc:
        sys.stderr.write(f"{exc}\n")
        return _MASK_FAILURE_EXIT

    if masked.exit_status != 0:
        return _report_run_failure("saliency-masked", masked)

    try:
        roi = blend_scores(full.vmaf_score, masked.vmaf_score, ns.weight)
    except ValueError as exc:
        sys.stderr.write(f"vmaf-roi-score: invalid pooled score: {exc}\n")
        return _INVALID_SCORE_EXIT
    _emit(ns, _build_payload(ns, full, masked, roi))
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised by the console script
    raise SystemExit(main())
