# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""argparse entry-point for ``vmaf-tune``.

Phase A exposes one subcommand: ``corpus``. It expands a (preset, crf)
grid against one or more reference YUVs and emits a JSONL row per
encode. Phase B (``bisect``) and Phase C (``predict``) will register
sibling subcommands here.
"""

from __future__ import annotations

import argparse
import dataclasses
import importlib
import json
import math
import subprocess
import sys
import tempfile
from collections.abc import Callable, Mapping
from pathlib import Path
from typing import TYPE_CHECKING, Any, TextIO

if TYPE_CHECKING:
    from .report import CodecRow, CodecSweepPoint

from . import __version__
from .bisect import bisect_target_vmaf
from .codec_adapters import get_adapter, known_codecs
from .corpus import (
    CorpusJob,
    CorpusOptions,
    coarse_to_fine_search,
    iter_rows,
    write_jsonl,
)
from .defaultmodel import DEFAULT_MODEL
from .encode import iter_grid
from .fast import (
    DEFAULT_CRF_HI,
    DEFAULT_CRF_LO,
    DEFAULT_PROXY_TOLERANCE,
    PROD_N_TRIALS,
    SMOKE_N_TRIALS,
    fast_recommend,
)
from .filter_adapters import get_filter_adapter, known_filters
from .per_shot import PredicateFn as PerShotPredicateFn
from .per_shot import (
    Shot,
    detect_shots,
    merge_shots,
    plan_to_shell_script,
    tune_per_shot,
    write_concat_listing,
)
from .prefilter import DEFAULT_CRF_HI as PREFILTER_CRF_HI
from .prefilter import DEFAULT_CRF_LO as PREFILTER_CRF_LO
from .prefilter import DEFAULT_N_TRIALS as PREFILTER_N_TRIALS
from .prefilter import SMOKE_N_TRIALS as PREFILTER_SMOKE_N_TRIALS
from .prefilter import (
    PelorusFilterUnavailableError,
    ProbeResult,
    pelorus_filter_available,
    recommend_prefilter,
)
from .resolution import neg_model_for
from .score_backend import ALL_BACKENDS, BackendUnavailableError, select_backend


class _TrackedDefaultAction(argparse.Action):
    """Argparse action that records when a flag was passed explicitly.

    When the user passes the flag, sets both ``args.<dest>`` to the
    parsed value AND ``args._<dest>_was_default`` to ``False``. When the
    flag is omitted, argparse uses the registered ``default`` for
    ``<dest>`` and the sentinel stays at its default ``False`` — so
    consumers default-initialise the marker to ``False`` then opt in by
    setting it ``True`` on the parser, mirroring the inverted semantics
    we actually want: "True means default, False means user override".

    ADR-0509 / BBB e2e v7 Bug #V7-1 needs this to distinguish
    ``vmaf-tune compare --framerate 24 …`` (user pinned 24 fps,
    keep it) from ``vmaf-tune compare …`` (argparse default 24 fps;
    auto-probe container sources and replace). The class is module-
    local because ``argparse.Action`` subclasses are otherwise
    boilerplate-heavy and the use-site is narrow.
    """

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: object,
        option_string: str | None = None,
    ) -> None:
        setattr(namespace, self.dest, values)
        # When the user explicitly passes the flag, mark the sentinel
        # ``_<dest>_was_default`` as False. The default value lives on
        # the namespace as ``True`` (set by ``_stamp_tracked_default_sentinels``
        # after parsing).
        setattr(namespace, f"_{self.dest}_was_default", False)


def _resolve_vmaf_model(args: argparse.Namespace, attr: str = "vmaf_model") -> str:
    """Return the effective VMAF model string from ``args``.

    When ``--neg`` is present (``args.neg is True``), routes the model
    version through :func:`~vmaftune.resolution.neg_model_for` so the
    NEG variant is used. Handles all subcommands that carry both a
    ``--vmaf-model`` flag and the ``--neg`` flag.

    Args:
        args: Parsed argument namespace.
        attr: Name of the model attribute on ``args`` (default
            ``"vmaf_model"``; some subcommands alias to ``"model"``).

    Returns:
        The (possibly NEG-routed) model version string.
    """
    model = getattr(args, attr, DEFAULT_MODEL)
    if getattr(args, "neg", False):
        model = neg_model_for(model)
    return model


def _add_neg_flag(parser: argparse.ArgumentParser) -> None:
    """Add the ``--neg`` flag to a subcommand parser.

    The flag selects VMAF NEG (No Enhancement Gain) model variants that
    penalise encoder in-loop sharpening. Use for codec A vs. B
    comparisons; do NOT use for production quality monitoring against
    baselines. See ``docs/metrics/vmaf-neg.md`` for full guidance.
    """
    parser.add_argument(
        "--neg",
        action="store_true",
        default=False,
        help=(
            "use the VMAF NEG (No Enhancement Gain) model variant, which "
            "penalises sharpening-based score inflation. Routes "
            "``--vmaf-model vmaf_v0.6.1`` → ``vmaf_v0.6.1neg`` (or the 4K "
            "equivalent). Use for codec A vs B comparisons where encoder "
            "sharpening may mask compression differences. Do NOT use for "
            "production quality monitoring against baselines — NEG produces "
            "lower scores than standard VMAF on the same content. "
            "See docs/metrics/vmaf-neg.md. (ADR-0622)"
        ),
    )


def _stamp_tracked_default_sentinels(args: argparse.Namespace) -> None:
    """Stamp ``_<dest>_was_default = True`` for every ``_TrackedDefaultAction``.

    Argparse never invokes the ``Action.__call__`` when the user omits
    the flag, so we cannot set the sentinel inside the Action. We post-
    process the namespace after ``parse_args`` and stamp every tracked
    sentinel that isn't already set to ``False`` (i.e. the user did not
    pass the flag) to ``True``.
    """
    # Iterate only the names the compare/ladder subparsers register as tracked.
    # Hardcoded so the sentinel-stamp pass stays cheap; add to this
    # tuple when wiring a new ``_TrackedDefaultAction`` flag.
    # NOTE: ladder stores --duration to dest="duration_s" (not "duration"), so
    # both keys are listed here so the stamp covers both sub-commands (ADR-1048).
    for dest in ("framerate", "duration", "duration_s", "target_vmafs", "target_vmaf"):
        sentinel = f"_{dest}_was_default"
        if not hasattr(args, sentinel):
            setattr(args, sentinel, True)


def _add_corpus_source_args(corpus: argparse.ArgumentParser) -> None:
    """Add source geometry and timing arguments for ``corpus``."""
    corpus.add_argument(
        "--source",
        type=Path,
        action="append",
        required=True,
        help="raw YUV reference (repeat for multiple sources)",
    )
    corpus.add_argument("--width", type=int, required=True)
    corpus.add_argument("--height", type=int, required=True)
    corpus.add_argument("--pix-fmt", default="yuv420p", help="ffmpeg pix_fmt (default yuv420p)")
    corpus.add_argument("--framerate", type=float, default=24.0, help="reference framerate")
    corpus.add_argument(
        "--duration",
        type=float,
        default=0.0,
        help="reference duration in seconds (used for bitrate calc)",
    )


def _add_corpus_grid_args(corpus: argparse.ArgumentParser) -> None:
    """Add codec-grid and output arguments for ``corpus``."""
    corpus.add_argument(
        "--encoder",
        default="libx264",
        choices=list(known_codecs()),
        help="codec adapter (default libx264; any registered adapter is accepted)",
    )
    corpus.add_argument(
        "--preset",
        action="append",
        required=True,
        help="x264 preset (repeatable)",
    )
    corpus.add_argument(
        "--crf",
        type=int,
        action="append",
        default=None,
        help=(
            "x264 CRF value (repeatable). Required unless "
            "--coarse-to-fine selects the CRF axis automatically."
        ),
    )
    corpus.add_argument(
        "--output",
        type=Path,
        default=Path("corpus.jsonl"),
        help="JSONL output path (default corpus.jsonl)",
    )
    corpus.add_argument(
        "--encode-dir",
        type=Path,
        default=Path(".workingdir/cache/vmafx-tune/encodes"),
        help=(
            "scratch dir for encodes " "(default .workingdir/cache/vmafx-tune/encodes, gitignored)"
        ),
    )
    corpus.add_argument(
        "--keep-encodes",
        action="store_true",
        help="retain encoded outputs after scoring (default: delete)",
    )
    corpus.add_argument(
        "--vmaf-model",
        default=DEFAULT_MODEL,
        help="vmaf model version string (default: the fork default model)",
    )
    _add_neg_flag(corpus)


def _add_corpus_runtime_args(corpus: argparse.ArgumentParser) -> None:
    """Add executable, backend, and search-strategy arguments."""
    corpus.add_argument("--ffmpeg-bin", default="ffmpeg")
    corpus.add_argument("--vmaf-bin", default="vmaf")
    corpus.add_argument(
        "--score-backend",
        default="auto",
        choices=("auto", *ALL_BACKENDS),
        help=(
            "libvmaf scoring backend (default: auto). 'auto' picks the "
            "fastest available (cuda > sycl > hip > cpu); a specific "
            "name is honoured strictly and errors out if unavailable. "
            "Use 'hip' on AMD ROCm hosts (ADR-0726 dropped the Vulkan "
            "backend 2026-05-28)."
        ),
    )
    corpus.add_argument(
        "--no-source-hash",
        action="store_true",
        help="skip src_sha256 (faster on huge YUVs; loses provenance)",
    )
    corpus.add_argument(
        "--two-pass",
        action="store_true",
        help=(
            "Phase F (ADR-0333): run a 2-pass encode for codecs that "
            "support it (libx264 / libx265 today; libsvtav1 / libvvenc "
            "follow as sibling PRs). Default off; single-pass remains "
            "the canonical path. Adapters where supports_two_pass = "
            "False fall back to single-pass with a stderr warning."
        ),
    )
    corpus.add_argument(
        "--sample-clip-seconds",
        type=float,
        default=0.0,
        metavar="N",
        help=(
            "encode/score only the centre N-second slice of each source "
            "(default 0 = full source). Encode time scales linearly with "
            "the slice length, so e.g. 10s of a 60s source is a ~6x "
            "speedup; expect a 1-2 VMAF-point delta vs full-clip on "
            "diverse content. See ADR-0297."
        ),
    )
    _add_coarse_to_fine_flags(corpus)


def _add_corpus_hdr_args(corpus: argparse.ArgumentParser) -> None:
    """Add mutually-exclusive HDR treatment flags for ``corpus``."""
    hdr = corpus.add_mutually_exclusive_group()
    hdr.add_argument(
        "--auto-hdr",
        dest="hdr_mode",
        action="store_const",
        const="auto",
        help=(
            "(default) probe each source via ffprobe and inject HDR "
            "codec args + the HDR-VMAF model when PQ / HLG signaling "
            "is detected"
        ),
    )
    hdr.add_argument(
        "--force-sdr",
        dest="hdr_mode",
        action="store_const",
        const="force-sdr",
        help="treat all sources as SDR; skip HDR detection and flag injection",
    )
    hdr.add_argument(
        "--force-hdr-pq",
        dest="hdr_mode",
        action="store_const",
        const="force-hdr-pq",
        help="treat all sources as HDR PQ (SMPTE-2084) regardless of probe",
    )
    hdr.add_argument(
        "--force-hdr-hlg",
        dest="hdr_mode",
        action="store_const",
        const="force-hdr-hlg",
        help="treat all sources as HDR HLG (ARIB STD-B67) regardless of probe",
    )
    corpus.set_defaults(hdr_mode="auto")
    corpus.add_argument(
        "--ffprobe-bin",
        default="ffprobe",
        help="path to the ffprobe binary (default: ffprobe on PATH)",
    )


def _add_corpus_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune corpus`` flags onto the subparser group."""
    corpus = sub.add_parser("corpus", help="run the Phase A grid sweep + emit JSONL")
    _add_corpus_source_args(corpus)
    _add_corpus_grid_args(corpus)
    _add_corpus_runtime_args(corpus)
    _add_corpus_hdr_args(corpus)


def _add_predict_model_args(predict: argparse.ArgumentParser) -> None:
    """Add predictor model, target, and validation arguments."""
    predict.add_argument(
        "--source",
        type=Path,
        required=True,
        help="reference video (any FFmpeg-readable container)",
    )
    predict.add_argument(
        "--codec",
        default="libx264",
        choices=list(known_codecs()),
        help="codec adapter (default libx264)",
    )
    predict.add_argument(
        "--target-vmaf",
        type=float,
        default=93.0,
        help="target pooled-mean VMAF (default 93)",
    )
    predict.add_argument(
        "--validate-k",
        type=int,
        default=8,
        help="number of shots to verify against real libvmaf (default 8)",
    )
    predict.add_argument(
        "--residual-threshold",
        type=float,
        default=1.5,
        help="max abs(predicted - measured) VMAF before falling back (default 1.5)",
    )
    predict.add_argument(
        "--use-saliency",
        action="store_true",
        help="include saliency_student mean/variance in predictor features",
    )
    predict.add_argument(
        "--saliency-model",
        type=Path,
        default=None,
        help=(
            "path to saliency_student ONNX for --use-saliency "
            "(default: model/tiny/saliency_student_v1.onnx)"
        ),
    )


def _add_predict_runtime_args(predict: argparse.ArgumentParser) -> None:
    """Add predictor executable, source-format, and output arguments."""
    predict.add_argument(
        "--model",
        type=Path,
        default=None,
        help="path to predictor_<codec>.onnx (default: analytical fallback)",
    )
    predict.add_argument(
        "--per-shot-bin",
        default="vmaf-perShot",
        help="path to the vmaf-perShot binary (default vmaf-perShot on PATH)",
    )
    predict.add_argument(
        "--ffmpeg-bin",
        default="ffmpeg",
        help="path to the ffmpeg binary (default ffmpeg on PATH)",
    )
    predict.add_argument(
        "--ffprobe-bin",
        default="ffprobe",
        help="path to the ffprobe binary (default ffprobe on PATH)",
    )
    predict.add_argument(
        "--bitdepth",
        type=int,
        default=8,
        choices=(8, 10, 12),
        help="source bit depth (forwarded to vmaf-perShot)",
    )
    predict.add_argument(
        "--total-frames",
        type=int,
        default=0,
        help="frame count for the single-shot fallback (when vmaf-perShot is unavailable)",
    )
    predict.add_argument(
        "--report-out",
        type=Path,
        default=None,
        help="emit the validation report (verdict + residuals) to this path; default: stdout",
    )


def _add_predict_uncertainty_args(predict: argparse.ArgumentParser) -> None:
    """Add conformal-uncertainty arguments for ``predict``."""
    predict.add_argument(
        "--with-uncertainty",
        action="store_true",
        help=(
            "emit conformal prediction intervals alongside each "
            "predicted VMAF point estimate (per ADR-0279). Each "
            "residual row gains an ``interval`` field with "
            "``{low, high, alpha}``. Requires a calibration sidecar "
            "(``--calibration-sidecar``) to produce a non-trivial "
            "interval; without one the wrapper degrades to "
            "``low == high == point`` and the report is flagged "
            "uncalibrated."
        ),
    )
    predict.add_argument(
        "--calibration-sidecar",
        type=Path,
        default=None,
        help=(
            "path to a split-conformal calibration JSON produced by "
            "``vmaftune.conformal.save_split_calibration``. Loaded only "
            "when ``--with-uncertainty`` is set."
        ),
    )
    predict.add_argument(
        "--alpha",
        type=float,
        default=None,
        help=(
            "override the calibration sidecar's nominal miscoverage "
            "level (default: the value baked into the sidecar; "
            "0.05 = 95%% coverage). Ignored without "
            "``--with-uncertainty``."
        ),
    )


def _add_predict_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune predict`` flags onto the subparser group."""
    predict = sub.add_parser(
        "predict",
        help=(
            "Phase C — predict per-shot VMAF without running it. Probes-encode "
            "each shot, runs a learned ONNX predictor (or analytical fallback), "
            "validates against real VMAF on K shots, then emits the verdict."
        ),
    )
    _add_predict_model_args(predict)
    _add_predict_runtime_args(predict)
    _add_predict_uncertainty_args(predict)


def _add_per_shot_source_args(per_shot: argparse.ArgumentParser) -> None:
    """Add source geometry and quality target arguments."""
    per_shot.add_argument(
        "--src",
        type=Path,
        required=True,
        help="reference video (raw YUV or any FFmpeg-readable container)",
    )
    per_shot.add_argument(
        "--width",
        type=int,
        default=None,
        help=(
            "source width. Required for raw YUV (`.yuv`/`.raw`) sources. "
            "Auto-probed from ffprobe for container sources (mp4, mkv, mov, …) "
            "when omitted. (ADR-0542)"
        ),
    )
    per_shot.add_argument(
        "--height",
        type=int,
        default=None,
        help=(
            "source height. Required for raw YUV sources. "
            "Auto-probed from ffprobe for container sources when omitted. (ADR-0542)"
        ),
    )
    per_shot.add_argument("--pix-fmt", default="yuv420p")
    per_shot.add_argument(
        "--framerate",
        type=float,
        default=None,
        help=(
            "source framerate. Auto-probed from ffprobe for container sources when "
            "omitted; defaults to 24.0 if the probe cannot determine a rate. (ADR-0542)"
        ),
    )
    per_shot.add_argument(
        "--target-vmaf",
        type=float,
        default=92.0,
        help="target pooled-mean VMAF for the per-shot predicate (default 92)",
    )
    per_shot.add_argument(
        "--encoder",
        default="libx264",
        choices=list(known_codecs()),
        help="codec adapter (default libx264; any registered adapter is accepted)",
    )
    per_shot.add_argument(
        "--bitdepth",
        type=int,
        default=8,
        choices=(8, 10, 12),
        help="source YUV bit depth (forwarded to vmaf-perShot)",
    )


def _add_per_shot_detection_args(per_shot: argparse.ArgumentParser) -> None:
    """Add shot-detector and uniform-window arguments."""
    per_shot.add_argument(
        "--total-frames",
        type=int,
        default=0,
        help=(
            "frame count for the single-shot fallback (used when " "vmaf-perShot is unavailable)"
        ),
    )
    per_shot.add_argument(
        "--scene-threshold",
        type=float,
        default=None,
        help=(
            "override vmaf-perShot --diff-threshold (mean-absolute-luma-delta "
            "cutoff for cut classification; lower = more shots). Omit to keep "
            "the C-side compiled default (12.0 on 8-bit content). ADR-0512."
        ),
    )
    per_shot.add_argument(
        "--max-shot-duration",
        type=float,
        default=2.0,
        help=(
            "uniform-time-window splitter: any detected shot longer than this "
            "many seconds is sliced into equal-length sub-shots so the "
            "per-shot tuner sees a non-degenerate timeline even when the "
            "detector under-cuts (e.g. 5 s clips on the BBB fixtures). Set "
            "to 0 to disable; default 2.0. ADR-0512."
        ),
    )
    per_shot.add_argument(
        "--per-shot-bin",
        default="vmaf-perShot",
        help="path to the vmaf-perShot binary (default vmaf-perShot on PATH)",
    )


def _add_per_shot_search_args(per_shot: argparse.ArgumentParser) -> None:
    """Add encode-and-score search arguments."""
    per_shot.add_argument(
        "--ffmpeg-bin",
        default="ffmpeg",
        help="path to the ffmpeg binary (default ffmpeg on PATH)",
    )
    per_shot.add_argument(
        "--vmaf-bin",
        default="vmaf",
        help="path to the vmaf binary used by the per-shot bisect scorer",
    )
    per_shot.add_argument(
        "--preset",
        default=None,
        help="codec preset forwarded to the per-shot bisect backend",
    )
    per_shot.add_argument("--crf-min", type=int, default=None, help="inclusive lower CRF bound")
    per_shot.add_argument("--crf-max", type=int, default=None, help="inclusive upper CRF bound")
    per_shot.add_argument(
        "--max-iterations",
        type=int,
        default=8,
        help="maximum encode+score iterations per detected shot",
    )
    per_shot.add_argument(
        "--vmaf-model",
        default=DEFAULT_MODEL,
        help="VMAF model name forwarded to the per-shot bisect scorer",
    )
    _add_neg_flag(per_shot)


def _add_per_shot_backend_args(per_shot: argparse.ArgumentParser) -> None:
    """Add score-backend and predicate override arguments."""
    per_shot.add_argument(
        "--fast-nr",
        action="store_true",
        default=False,
        dest="fast_nr",
        help=(
            "enable NR early-elimination for each per-shot bisect "
            "(ADR-0624 / ADR-0615). At each midpoint the cheap "
            "nr_metric_v1 ONNX model scores the distorted stream; "
            "if |NR - target| > δ_fast the full-reference VMAF call "
            "is skipped and the bisect window advances in the NR-implied "
            "direction. The final accepted CRF always gets a full-reference "
            "confirmation call. Requires onnxruntime (pip install "
            "onnxruntime or onnxruntime-gpu) and numpy."
        ),
    )
    per_shot.add_argument(
        "--score-backend",
        default="auto",
        choices=("auto", *ALL_BACKENDS),
        help="libvmaf score backend for the per-shot bisect scorer",
    )
    per_shot.add_argument(
        "--predicate-module",
        default=None,
        help=(
            "advanced hook MODULE:CALLABLE matching "
            "(shot, target_vmaf, encoder) -> (crf, measured_vmaf); "
            "bypasses real bisect"
        ),
    )


def _add_per_shot_output_args(per_shot: argparse.ArgumentParser) -> None:
    """Add plan, segment, and temporary-workspace arguments."""
    per_shot.add_argument(
        "--output",
        type=Path,
        default=Path("per_shot_encode.mp4"),
        help="final concatenated encode destination (default per_shot_encode.mp4)",
    )
    per_shot.add_argument(
        "--segment-dir",
        type=Path,
        default=None,
        help="directory for per-shot segment files (default <output>.parent/segments)",
    )
    per_shot.add_argument(
        "--plan-out",
        type=Path,
        default=None,
        help="emit the JSON plan to this path; default: stdout",
    )
    per_shot.add_argument(
        "--script-out",
        type=Path,
        default=None,
        help="optional: write a copy-paste shell script of the plan",
    )
    per_shot.add_argument(
        "--workdir",
        type=Path,
        default=None,
        metavar="PATH",
        help=(
            "directory for temporary bisect encode / decode artefacts. "
            "Overrides VMAFTUNE_WORKDIR and the OS /tmp default. "
            "Ensure the volume has sufficient free space for raw YUV decodes. "
            "(ADR-0546)"
        ),
    )
    per_shot.add_argument(
        "--max-concurrent-decodes",
        type=int,
        default=1,
        metavar="N",
        dest="max_concurrent_decodes",
        help=(
            "maximum number of reference-YUV decode operations that may "
            "run simultaneously across all codec bisect threads (ADR-0577). "
            "Default 1 (serial decodes) — safest for disk-space constrained "
            "volumes. Raise to N on hosts with large --workdir volumes and "
            "sufficient IOPS to decode N streams in parallel. "
            "Encoder runs are always parallel; only the decode-to-raw-YUV "
            "step is serialized at the default."
        ),
    )


def _add_per_shot_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune tune-per-shot`` flags onto the subparser group."""
    per_shot = sub.add_parser(
        "tune-per-shot",
        help=(
            "Phase D — detect shots via vmaf-perShot/TransNet V2, run "
            "Phase-B bisect per shot, and emit an FFmpeg encoding plan."
        ),
    )
    _add_per_shot_source_args(per_shot)
    _add_per_shot_detection_args(per_shot)
    _add_per_shot_search_args(per_shot)
    _add_per_shot_backend_args(per_shot)
    _add_per_shot_output_args(per_shot)


def _add_saliency_source_args(rec_sal: argparse.ArgumentParser) -> None:
    """Add source, codec, and clip-length arguments for saliency tuning."""
    rec_sal.add_argument("--src", type=Path, required=True, help="raw YUV reference")
    rec_sal.add_argument("--width", type=int, required=True)
    rec_sal.add_argument("--height", type=int, required=True)
    rec_sal.add_argument("--pix-fmt", default="yuv420p")
    rec_sal.add_argument("--framerate", type=float, default=24.0)
    rec_sal.add_argument(
        "--encoder",
        default="libx264",
        choices=list(known_codecs()),
        help=(
            "codec adapter; saliency ROI supports libx264, libaom-av1, "
            "libx265, libsvtav1, and libvvenc"
        ),
    )
    rec_sal.add_argument("--preset", default="medium", help="encoder preset")
    rec_sal.add_argument(
        "--crf",
        type=int,
        default=None,
        help="explicit CRF; defaults to the codec adapter's quality_default",
    )
    rec_sal.add_argument(
        "--duration-frames",
        type=int,
        required=True,
        help="frame count to score saliency over (typical: full clip length)",
    )


def _add_saliency_tuning_args(rec_sal: argparse.ArgumentParser) -> None:
    """Add saliency model, reducer, fallback, and output arguments."""
    rec_sal.add_argument(
        "--saliency-aware",
        action="store_true",
        help="enable saliency biasing (no-op when off; falls back to plain encode)",
    )
    rec_sal.add_argument(
        "--saliency-offset",
        type=int,
        default=-4,
        help="QP delta applied to salient blocks (default -4; clamped to ±12)",
    )
    rec_sal.add_argument(
        "--saliency-model",
        type=Path,
        default=None,
        help="path to saliency_student_v1.onnx (default: shipped fork model)",
    )
    rec_sal.add_argument(
        "--saliency-aggregator",
        choices=("mean", "ema", "max", "motion-weighted"),
        default="mean",
        help=(
            "temporal reducer for sampled saliency masks: mean preserves "
            "the historical behaviour; ema/max/motion-weighted are "
            "video-saliency baselines"
        ),
    )
    rec_sal.add_argument(
        "--saliency-ema-alpha",
        type=float,
        default=0.6,
        help="current-frame weight for --saliency-aggregator=ema (default 0.6)",
    )
    rec_sal.add_argument(
        "--saliency-fallback-plain",
        action="store_true",
        default=False,
        help=(
            "ADR-0546: when --saliency-aware is set and the chosen encoder has no ROI "
            "dispatch (e.g. h264_nvenc, libvpx-vp9), accept a plain encode instead of "
            "exiting with code 2. An ERROR is logged. "
            "Equivalent to setting VMAFTUNE_SALIENCY_FALLBACK_OK=1 in the environment. "
            "Supported ROI encoders: libx264, libaom-av1, libx265, libsvtav1, libvvenc."
        ),
    )
    rec_sal.add_argument("--ffmpeg-bin", default="ffmpeg")
    rec_sal.add_argument(
        "--output",
        type=Path,
        required=True,
        help="encode destination (mp4 / mkv / ...)",
    )


def _add_recommend_saliency_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune recommend-saliency`` flags onto the subparser group."""
    rec_sal = sub.add_parser(
        "recommend-saliency",
        help=(
            "saliency-aware ROI encode — biases bits toward salient regions "
            "via the fork-trained ``saliency_student_v1`` ONNX model "
            "(Bucket #2 / ADR-0287)"
        ),
    )
    _add_saliency_source_args(rec_sal)
    _add_saliency_tuning_args(rec_sal)


def _add_ladder_shape_args(ladder: argparse.ArgumentParser) -> None:
    """Add ladder source, codec, resolution, and tier arguments."""
    ladder.add_argument(
        "--src",
        type=Path,
        required=True,
        help="source video (raw YUV or any FFmpeg-readable container)",
    )
    ladder.add_argument(
        "--encoder",
        default="libx264",
        choices=list(known_codecs()),
        help="codec adapter (default libx264)",
    )
    ladder.add_argument(
        "--resolutions",
        required=True,
        help="comma-separated WxH list, e.g. ``1920x1080,1280x720,854x480``",
    )
    ladder.add_argument(
        "--target-vmafs",
        required=True,
        help="comma-separated VMAF target list, e.g. ``95,90,85``",
    )
    ladder.add_argument(
        "--quality-tiers",
        type=int,
        default=5,
        help="number of ladder rungs to select from the convex hull (default 5)",
    )


def _add_ladder_output_args(ladder: argparse.ArgumentParser) -> None:
    """Add ladder output format and spacing arguments."""
    ladder.add_argument(
        "--format",
        default="hls",
        choices=("hls", "dash", "json"),
        help="manifest format (default hls)",
    )
    ladder.add_argument(
        "--spacing",
        default="log_bitrate",
        choices=("log_bitrate", "vmaf", "uniform"),
        help=(
            "knee spacing strategy on the hull: log_bitrate or vmaf "
            "(legacy alias: uniform). Default log_bitrate"
        ),
    )
    ladder.add_argument(
        "--output",
        type=Path,
        default=None,
        help="manifest destination (default: stdout)",
    )


def _add_ladder_uncertainty_args(ladder: argparse.ArgumentParser) -> None:
    """Add uncertainty-aware rung selection arguments."""
    ladder.add_argument(
        "--with-uncertainty",
        action="store_true",
        help=(
            "apply ADR-0279 uncertainty-aware rung selection: prune "
            "adjacent rungs whose conformal intervals overlap above "
            "the threshold, then insert mid-rungs in wide-interval "
            "regions. No-op without per-rung intervals from the "
            "sampler — see vmaftune.ladder.UncertaintyLadderPoint."
        ),
    )
    ladder.add_argument(
        "--uncertainty-sidecar",
        type=Path,
        default=None,
        help=(
            "calibration sidecar JSON (same schema as "
            "``recommend --uncertainty-sidecar``). Defaults to the "
            "Research-0067 floor (tight=2.0, wide=5.0 VMAF)."
        ),
    )
    ladder.add_argument(
        "--rung-overlap-threshold",
        type=float,
        default=None,
        help=(
            "fraction of the wider rung's conformal-interval width "
            "above which two adjacent rungs are treated as "
            "indistinguishable and the lower-bitrate one is dropped. "
            "Default 0.5 per Research-0067."
        ),
    )


def _add_ladder_sampler_args(ladder: argparse.ArgumentParser) -> None:
    """Add default-sampler source geometry and CRF arguments."""
    ladder.add_argument(
        "--framerate",
        type=float,
        default=24.0,
        help="source frame rate, fed to the default corpus sampler (default 24.0)",
    )
    ladder.add_argument(
        "--duration",
        type=float,
        default=1.0,
        dest="duration_s",
        help="source duration in seconds, used for bitrate math (default 1.0)",
    )
    ladder.add_argument(
        "--pix-fmt",
        default="yuv420p",
        help="source pixel format (default yuv420p)",
    )
    ladder.add_argument(
        "--crf-sweep",
        default=None,
        help=(
            "comma-separated CRF list to use instead of the canonical "
            "5-point sweep (18,23,28,33,38). Useful for smoke runs that "
            "want to exercise the ladder plumbing with a 1-2 CRF "
            "schedule (Bug #5, BBB e2e 2026-05-17)."
        ),
    )
    ladder.add_argument(
        "--src-width",
        type=int,
        default=None,
        help=(
            "actual source width when it differs from the rung target "
            "(raw YUV cross-resolution ladders). Defaults to the "
            "largest --resolutions entry (Bug #v2-B, BBB e2e 2026-05-18)."
        ),
    )
    ladder.add_argument(
        "--src-height",
        type=int,
        default=None,
        help=("actual source height when it differs from the rung target " "(see --src-width)."),
    )


def _add_ladder_backend_args(ladder: argparse.ArgumentParser) -> None:
    """Add score backend and temporary-workspace arguments."""
    ladder.add_argument(
        "--score-backend",
        default="auto",
        choices=("auto", *ALL_BACKENDS),
        help=(
            "libvmaf scoring backend used by the default corpus sampler "
            "(default: auto). 'auto' picks the fastest available "
            "(cuda > sycl > hip > cpu); a specific name is honoured "
            "strictly and errors out if unavailable. Use 'cpu' to force "
            "bit-exact CPU scoring for verification against golden data. "
            "(Bug C / ADR-0509)"
        ),
    )
    ladder.add_argument("--vmaf-bin", default="vmaf", help="path to the vmaf binary")
    ladder.add_argument(
        "--workdir",
        type=Path,
        default=None,
        metavar="PATH",
        help=(
            "directory for temporary corpus-sampler encode / decode artefacts. "
            "Overrides VMAFTUNE_WORKDIR and the OS /tmp default. "
            "Ensure the volume has sufficient free space for raw YUV decodes. "
            "(ADR-0546)"
        ),
    )
    ladder.add_argument(
        "--max-concurrent-decodes",
        type=int,
        default=1,
        metavar="N",
        dest="max_concurrent_decodes",
        help=(
            "maximum number of reference-YUV decode operations that may "
            "run simultaneously across all codec bisect threads (ADR-0577). "
            "Default 1 (serial decodes). Raise on hosts with large --workdir "
            "volumes. Encoder runs are always parallel; only the "
            "decode-to-raw-YUV step is serialized at the default."
        ),
    )
    _add_neg_flag(ladder)


def _add_ladder_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune ladder`` flags onto the subparser group."""
    ladder = sub.add_parser(
        "ladder",
        help=(
            "Phase E — build a per-title bitrate ladder (convex-hull "
            "sweep over (resolution × target-VMAF), pick K knees, "
            "emit HLS / DASH / JSON manifest)"
        ),
    )
    _add_ladder_shape_args(ladder)
    _add_ladder_output_args(ladder)
    _add_ladder_uncertainty_args(ladder)
    _add_ladder_sampler_args(ladder)
    _add_ladder_backend_args(ladder)


def _add_compare_source_args(compare: argparse.ArgumentParser) -> None:
    """Add compare source and legacy single-target arguments."""
    compare.add_argument(
        "--src",
        type=Path,
        required=True,
        help="reference video (raw YUV or any FFmpeg-readable container)",
    )
    compare.add_argument(
        "--target-vmaf",
        type=float,
        action=_TrackedDefaultAction,
        default=92.0,
        help=(
            "single VMAF target each codec aims for (default 92). "
            "Back-compat shortcut for --target-vmafs N. Ignored when "
            "--target-vmafs lists more than one target (ADR-0516). "
            "When passed explicitly and --target-vmafs is left at its "
            "default sweep, the v1 single-target schema is emitted "
            "(ADR-0530 back-compat)."
        ),
    )


def _add_compare_target_sweep_arg(compare: argparse.ArgumentParser) -> None:
    """Add the multi-target rate-quality sweep argument."""
    compare.add_argument(
        "--target-vmafs",
        action=_TrackedDefaultAction,
        default="94,96,97,98",
        help=(
            "comma-separated VMAF targets to sweep per codec. When this "
            "lists more than one value the CLI emits the v2 multi-target "
            "schema (ADR-0513) and the report renders a rate-quality "
            "curve per codec with the pareto frontier highlighted. "
            "Default (ADR-0538, supersedes ADR-0534): ``94,96,97,98`` — "
            "premium-archival operating points. VMAF 94 is the "
            "subjectively-transparent floor on 4K source; 98 is "
            "near-lossless. The bisect (ADR-0538) now starts its CRF "
            "search at the encoder's absolute floor (e.g. CRF 0 for "
            "libx264 / libx265 / libvpx-vp9 / libsvtav1) so VMAF >=95 "
            "is reachable; if the codec already overshoots the target at "
            "its lowest accepted CRF the bisect returns that CRF with "
            "ok=true and the achieved VMAF instead of an 'unreachable' "
            "failure row. Pass a single value to fall through to the "
            "legacy single-target schema. When ``--target-vmaf`` is "
            "passed explicitly and ``--target-vmafs`` is left at its "
            "default, the back-compat path activates and the legacy "
            "single-target v1 schema is emitted."
        ),
    )


def _add_compare_output_args(compare: argparse.ArgumentParser) -> None:
    """Add encoder selection, concurrency, and output arguments."""
    compare.add_argument(
        "--encoders",
        required=False,
        default=None,
        help=(
            "comma-separated list of encoders to compare "
            "(e.g. ``libx264,libx265,libsvtav1,h264_nvenc``). "
            "Use ADAPTER@VARIANT labels with --encoder-ffmpeg-bin for "
            "runtime variants that share one FFmpeg encoder name "
            "(for example libsvtav1@svt-av1-hdr). "
            "Default: the CPU encoder set ``libx265,libsvtav1`` "
            "(ADR-0641). Hardware encoders (``*_nvenc``, ``*_qsv``, ``*_amf``) "
            "are accepted; missing-encoder / no-compatible-GPU rows are "
            "skipped with a reason and do not fail the whole run."
        ),
    )
    compare.add_argument(
        "--format",
        default="markdown",
        choices=("markdown", "json", "csv", "html", "both"),
        help=(
            "report format (default markdown). html/both render the same "
            "profile-card artefacts as `vmaf-tune report`; both requires --output."
        ),
    )
    compare.add_argument(
        "--no-parallel",
        action="store_true",
        help="dispatch encoders sequentially (default: thread pool, one worker per encoder)",
    )
    compare.add_argument(
        "--max-workers",
        type=int,
        default=None,
        help="override thread-pool size (default: len(encoders))",
    )
    compare.add_argument(
        "--output",
        type=Path,
        default=None,
        help="report destination (default: stdout)",
    )
    compare.add_argument(
        "--json-sidecar",
        action="store_true",
        help="when emitting html or markdown, also write <output>.json sidecar with data.to_dict()",
    )


def _add_compare_geometry_args(compare: argparse.ArgumentParser) -> None:
    """Add source geometry, timing, and sample-window arguments."""
    compare.add_argument("--width", type=int, default=None, help="source width for real bisect")
    compare.add_argument("--height", type=int, default=None, help="source height for real bisect")
    compare.add_argument("--pix-fmt", default="yuv420p", help="source pixel format")
    # ADR-0509 / BBB e2e v7: ``--framerate`` and ``--duration`` use the
    # ``_TrackedDefaultAction`` so ``_run_compare`` can tell "user
    # explicitly passed 24" from "argparse default 24"; the container-
    # source auto-probe only replaces the latter.
    compare.add_argument(
        "--framerate",
        type=float,
        default=24.0,
        action=_TrackedDefaultAction,
        help=(
            "source framerate (default 24; auto-probed from the source "
            "when --src is a container and this flag is left at default)"
        ),
    )
    compare.add_argument(
        "--duration",
        type=float,
        default=0.0,
        action=_TrackedDefaultAction,
        help=(
            "source duration in seconds, used for bitrate math (auto-probed "
            "from the source when --src is a container and this flag is "
            "left at default)"
        ),
    )
    compare.add_argument(
        "--sample-clip-seconds",
        type=float,
        default=0.0,
        help=(
            "score a centered N-second source window per bisect iteration "
            "(ADR-0301). 0 = full source."
        ),
    )


def _add_compare_search_args(compare: argparse.ArgumentParser) -> None:
    """Add codec search-window and fast-NR arguments."""
    compare.add_argument(
        "--preset",
        default="medium",
        help=(
            "codec preset for the bisect backend (default: medium). "
            "Each adapter maps this onto its native preset vocabulary. "
            "Use this flag when all codecs under comparison should run at "
            "the same preset level; omit it to accept the per-adapter default."
        ),
    )
    compare.add_argument("--crf-min", type=int, default=None, help="inclusive lower CRF bound")
    compare.add_argument("--crf-max", type=int, default=None, help="inclusive upper CRF bound")
    compare.add_argument(
        "--max-iterations",
        type=int,
        default=8,
        help="maximum encode+score iterations per codec",
    )
    compare.add_argument(
        "--vmaf-model",
        default=DEFAULT_MODEL,
        help="VMAF model name forwarded to the bisect scorer",
    )
    _add_neg_flag(compare)
    compare.add_argument(
        "--fast-nr",
        action="store_true",
        default=False,
        dest="fast_nr",
        help=(
            "enable NR early-elimination for each per-codec bisect "
            "(ADR-0624 / ADR-0615). At each midpoint the cheap "
            "nr_metric_v1 ONNX model scores the distorted stream; "
            "if |NR - target| > δ_fast the full-reference VMAF call "
            "is skipped and the bisect window advances in the NR-implied "
            "direction. The final accepted CRF always gets a full-reference "
            "confirmation call. Requires onnxruntime and numpy. "
            "Typical wall-time reduction: 2–4x on content far from target."
        ),
    )


def _add_compare_runtime_args(compare: argparse.ArgumentParser) -> None:
    """Add scoring executables, backend, and predicate arguments."""
    compare.add_argument(
        "--score-backend",
        default=None,
        choices=(*ALL_BACKENDS, "auto"),
        help="libvmaf score backend for the bisect scorer",
    )
    compare.add_argument("--ffmpeg-bin", default="ffmpeg", help="ffmpeg binary")
    compare.add_argument(
        "--encoder-ffmpeg-bin",
        action="append",
        default=None,
        metavar="ENCODER=PATH",
        help=(
            "bind one compare encoder token to a specific FFmpeg binary. "
            "Use with ADAPTER@VARIANT tokens, for example "
            "libsvtav1@svt-av1-hdr=/opt/ffmpeg-svtav1-hdr/bin/ffmpeg. "
            "Unbound tokens use --ffmpeg-bin."
        ),
    )
    compare.add_argument("--vmaf-bin", default="vmaf", help="vmaf binary")
    compare.add_argument(
        "--predicate-module",
        default=None,
        help=(
            "advanced hook MODULE:CALLABLE matching "
            "(codec, src, target_vmaf) -> RecommendResult; bypasses real bisect"
        ),
    )


def _add_compare_mode_args(compare: argparse.ArgumentParser) -> None:
    """Add CRF-sweep mode and temporary-workspace arguments."""
    compare.add_argument(
        "--no-bisect",
        action="store_true",
        default=False,
        help=(
            "CRF sweep mode: skip target-VMAF bisect and encode each "
            "(codec, CRF) pair from --crf-sweep exactly once. Requires "
            "--crf-sweep. Output is schema-version-3 JSON. (ADR-0542)"
        ),
    )
    compare.add_argument(
        "--crf-sweep",
        default=None,
        metavar="LIST",
        help=(
            "comma-separated CRF values for --no-bisect mode "
            "(e.g. 18,23,28,33). Required when --no-bisect is passed. (ADR-0542)"
        ),
    )
    compare.add_argument(
        "--workdir",
        type=Path,
        default=None,
        metavar="PATH",
        help=(
            "directory used for temporary encoded and decoded YUV files. "
            "Overrides the VMAFTUNE_WORKDIR environment variable and the "
            "OS default (/tmp). Use a path on a volume with sufficient free "
            "space — a full 1080p60 source decode can exceed 100 GB. "
            "(ADR-0546)"
        ),
    )


def _add_compare_device_args(compare: argparse.ArgumentParser) -> None:
    """Add decode-concurrency and QSV device arguments."""
    compare.add_argument(
        "--max-concurrent-decodes",
        type=int,
        default=1,
        metavar="N",
        dest="max_concurrent_decodes",
        help=(
            "maximum number of reference-YUV decode operations that may "
            "run simultaneously across all codec bisect threads (ADR-0577). "
            "Default 1 (serial decodes) — safest for the /probes volume. "
            "A BBB 1080p source decodes to ~110 GB; 3 concurrent decodes "
            "at default would require 330 GB peak. With N=1 (default) the "
            "peak is 110 GB regardless of how many codecs run in parallel. "
            "Raise to N on hosts with large --workdir volumes and sufficient "
            "IOPS. Encoder runs are always parallel; only the "
            "decode-to-raw-YUV step is serialized at the default."
        ),
    )
    compare.add_argument(
        "--vaapi-device",
        default=None,
        metavar="PATH",
        dest="vaapi_device",
        help=(
            "VA-API DRI render-node used for Intel QSV hardware-device "
            "initialisation (e.g. /dev/dri/renderD129). Defaults to auto, "
            "which selects the first Intel render node from /sys/class/drm. "
            "Also overridable via VMAFTUNE_VAAPI_DEVICE env var "
            "(flag takes precedence). (ADR-0641)"
        ),
    )


def _add_compare_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune compare`` flags onto the subparser group."""
    compare = sub.add_parser(
        "compare",
        help=(
            "compare codec adapters at a target VMAF — runs the "
            "Phase B-lite predicate per encoder, ranks by smallest "
            "bitrate, emits a markdown / JSON / CSV report"
        ),
    )
    _add_compare_source_args(compare)
    _add_compare_target_sweep_arg(compare)
    _add_compare_output_args(compare)
    _add_compare_geometry_args(compare)
    _add_compare_search_args(compare)
    _add_compare_runtime_args(compare)
    _add_compare_mode_args(compare)
    _add_compare_device_args(compare)


def _add_auto_plan_args(auto: argparse.ArgumentParser) -> None:
    """Add adaptive-plan inputs and output arguments."""
    auto.add_argument(
        "--src",
        type=Path,
        required=True,
        help="reference video (raw YUV or any FFmpeg-readable container)",
    )
    auto.add_argument(
        "--target-vmaf",
        type=float,
        default=93.0,
        help="target pooled-mean VMAF (default 93)",
    )
    auto.add_argument(
        "--max-budget-bitrate",
        type=float,
        default=8000.0,
        help="upper bound on the picked rendition's bitrate in kbps (default 8000)",
    )
    auto.add_argument(
        "--allow-codecs",
        default="libx264",
        help=(
            "comma-separated list of codecs the tree may pick from "
            "(default libx264). When the list resolves to a single "
            "codec the compare-shortlist stage short-circuits."
        ),
    )
    auto.add_argument(
        "--codec",
        default=None,
        help=(
            "pin the codec choice (overrides --allow-codecs ranking). "
            "When set the compare-shortlist stage short-circuits."
        ),
    )
    auto.add_argument(
        "--sample-clip-seconds",
        type=float,
        default=0.0,
        help=(
            "propagate this clip length to internal sweeps rather than "
            "re-deciding per stage (ADR-0301). 0 = full source."
        ),
    )
    auto.add_argument(
        "--smoke",
        action="store_true",
        help=(
            "exercise the composition end-to-end with mocked sub-phases "
            "(no ffmpeg, no ONNX); non-smoke probes source metadata."
        ),
    )
    auto.add_argument(
        "--output",
        type=Path,
        default=None,
        help="emit the JSON plan to this path (default: stdout)",
    )


def _add_auto_execute_args(auto: argparse.ArgumentParser) -> None:
    """Add optional plan execution arguments."""
    auto.add_argument(
        "--execute",
        action="store_true",
        help=(
            "Phase F execute mode (ADR-0454): after planning, run real FFmpeg "
            "encodes and libvmaf scores for the selected cell(s). Results are "
            "written to --runs-dir/tune_results.jsonl. Default: plan-only."
        ),
    )
    auto.add_argument(
        "--runs-dir",
        type=Path,
        default=Path("runs"),
        help=(
            "output directory for encoded files and tune_results.jsonl "
            "(used with --execute; default: runs/)"
        ),
    )
    auto.add_argument(
        "--execute-all",
        action="store_true",
        help=(
            "with --execute: run every plan cell, not just the selected winner "
            "(useful for post-hoc A/B comparison)."
        ),
    )


def _add_auto_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune auto`` flags onto the subparser group."""
    auto = sub.add_parser(
        "auto",
        help=(
            "Phase F — adaptive recipe-aware tuning entry point "
            "(ADR-0364). Composes the per-phase subcommands into one "
            "deterministic decision tree with seven short-circuits "
            "and non-smoke source metadata probing."
        ),
    )
    _add_auto_plan_args(auto)
    _add_auto_execute_args(auto)


def _add_report_input_args(report: argparse.ArgumentParser) -> None:
    """Add report source and phase-output inputs."""
    report.add_argument(
        "--src",
        type=Path,
        required=True,
        help="reference video (used for ffprobe metadata in the report header)",
    )
    report.add_argument(
        "--target-vmaf",
        type=float,
        default=92.0,
        help="target VMAF (displayed in the report header)",
    )
    report.add_argument(
        "--compare-json",
        type=Path,
        default=None,
        help="`vmaf-tune compare --format json` output to ingest",
    )
    report.add_argument(
        "--ladder-json",
        type=Path,
        default=None,
        help="`vmaf-tune ladder --format json` output to ingest (raw samples and/or picked rungs)",
    )
    report.add_argument(
        "--per-shot-json",
        type=Path,
        default=None,
        help="`vmaf-tune tune-per-shot --format json` output to ingest",
    )


def _add_report_output_args(report: argparse.ArgumentParser) -> None:
    """Add report render and encoder-profile metadata arguments."""
    report.add_argument(
        "--format",
        default="html",
        choices=("html", "markdown", "both"),
        help="report format (default html; `both` emits .json + .html + .md next to --output)",
    )
    report.add_argument(
        "--output",
        type=Path,
        required=True,
        help="report destination (file path; .html/.md suffix matches --format)",
    )
    report.add_argument(
        "--assets-dir",
        type=Path,
        default=None,
        help="when emitting markdown, write chart PNGs into this dir (default: inline base64)",
    )
    report.add_argument("--pix-fmt", default="", help="source pix_fmt to record in encoder profile")
    report.add_argument("--preset", default="", help="encoder preset to record in encoder profile")
    report.add_argument(
        "--score-backend", default="", help="score backend to record in encoder profile"
    )
    report.add_argument(
        "--ffmpeg-bin", default="", help="ffmpeg binary to record in encoder profile"
    )
    report.add_argument("--vmaf-bin", default="", help="vmaf binary to record in encoder profile")
    report.add_argument(
        "--json-sidecar",
        action="store_true",
        help="when emitting html or markdown, also write <output>.json sidecar with data.to_dict()",
    )


def _add_report_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune report`` flags onto the subparser group."""
    report = sub.add_parser(
        "report",
        help=(
            "render an HTML/Markdown profile card for a tuned source — "
            "ingests JSON dumps of `compare`/`ladder`/`tune-per-shot` and "
            "emits a self-contained artefact with rate-distortion charts, "
            "ladder rungs, per-shot timeline, and source metadata"
        ),
    )
    _add_report_input_args(report)
    _add_report_output_args(report)


def _add_encode_profile_selection_args(encode_profile: argparse.ArgumentParser) -> None:
    """Add profile and recommendation selection arguments."""
    encode_profile.add_argument(
        "--profile",
        type=Path,
        required=True,
        help="report JSON/HTML/Markdown containing encoder_profile",
    )
    encode_profile.add_argument("--output", type=Path, required=True, help="encoded output path")
    encode_profile.add_argument(
        "--src",
        type=Path,
        default=None,
        help="override the source path stored in the profile",
    )
    encode_profile.add_argument("--codec", default=None, help="restrict selection to one codec")
    encode_profile.add_argument(
        "--target-vmaf",
        type=float,
        default=None,
        help="restrict selection to one target VMAF",
    )
    encode_profile.add_argument(
        "--recommendation-index",
        type=int,
        default=None,
        help="zero-based index after --codec/--target-vmaf filtering",
    )


def _add_encode_profile_runtime_args(encode_profile: argparse.ArgumentParser) -> None:
    """Add source overrides and FFmpeg execution arguments."""
    encode_profile.add_argument("--preset", default=None, help="override the stored/default preset")
    encode_profile.add_argument("--pix-fmt", default=None, help="override raw-source pixel format")
    encode_profile.add_argument("--framerate", type=float, default=None)
    encode_profile.add_argument("--width", type=int, default=None)
    encode_profile.add_argument("--height", type=int, default=None)
    encode_profile.add_argument("--duration", type=float, default=None)
    encode_profile.add_argument(
        "--source-kind",
        choices=("auto", "container", "raw"),
        default="auto",
        help="input interpretation (default auto; .yuv/.raw are raw)",
    )
    encode_profile.add_argument(
        "--sample-clip-seconds",
        type=float,
        default=0.0,
        help="optional input-side clip length forwarded to FFmpeg",
    )
    encode_profile.add_argument(
        "--sample-clip-start-s",
        type=float,
        default=0.0,
        help="optional input-side clip offset forwarded to FFmpeg",
    )
    encode_profile.add_argument(
        "--extra-ffmpeg-arg",
        action="append",
        default=[],
        help=(
            "append one raw FFmpeg argv token after codec args; repeat as needed "
            "(use --extra-ffmpeg-arg=-movflags for tokens beginning with '-')"
        ),
    )
    encode_profile.add_argument(
        "--ffmpeg-bin",
        default=None,
        help="override profile ffmpeg_bin (default: profile value, then ffmpeg)",
    )
    encode_profile.add_argument(
        "--dry-run",
        action="store_true",
        help="print selected recommendation and ffmpeg argv without encoding",
    )


def _add_encode_profile_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune encode-profile`` flags onto the subparser group."""
    encode_profile = sub.add_parser(
        "encode-profile",
        help=(
            "read a vmaf-tune report/profile and encode one selected recommendation " "with FFmpeg"
        ),
    )
    _add_encode_profile_selection_args(encode_profile)
    _add_encode_profile_runtime_args(encode_profile)


def _add_recommend_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune recommend`` onto the subparser group."""
    recommend = sub.add_parser(
        "recommend",
        help=(
            "find the smallest CRF whose VMAF >= --target-vmaf "
            "(coarse-to-fine, ~3.5x fewer encodes than the full grid)"
        ),
    )
    _add_recommend_args(recommend)


def _add_benchmark_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune benchmark`` onto the subparser group."""
    benchmark = sub.add_parser(
        "benchmark",
        help=(
            "Phase G — rank encoders from an existing corpus JSONL at a "
            "matched target VMAF, without running new encodes"
        ),
    )
    benchmark.add_argument(
        "--from-corpus",
        type=Path,
        required=True,
        metavar="JSONL",
        help="Phase-A corpus JSONL to benchmark",
    )
    benchmark.add_argument(
        "--target-vmaf",
        type=float,
        default=92.0,
        help="matched-quality threshold each encoder must clear (default 92)",
    )
    benchmark.add_argument(
        "--baseline-encoder",
        default=None,
        help=(
            "encoder used for bitrate-delta percentages. Default: lowest-bitrate "
            "encoder that clears the target."
        ),
    )
    benchmark.add_argument(
        "--format",
        default="markdown",
        choices=("markdown", "json", "csv"),
        help="report format (default markdown)",
    )
    benchmark.add_argument(
        "--output", type=Path, default=None, help="report destination (default: stdout)"
    )


def _add_fast_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune fast`` onto the subparser group."""
    fast = sub.add_parser(
        "fast",
        help=(
            "Phase A.5 fast-path — proxy + Bayesian + GPU-verify recommend "
            "(ADR-0276 + ADR-0304). Seconds-to-minutes alternative to the "
            "Phase A grid for the recommendation use case."
        ),
    )
    _add_fast_args(fast)


def _add_prefilter_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune prefilter`` onto the subparser group."""
    prefilter = sub.add_parser(
        "prefilter",
        help=(
            "control-plane autotune — joint TPE search over the Pelorus "
            "deband pre-filter strengths (frozen ADR-0110 contract) + CRF, "
            "with VMAF as the oracle (ADR-1116 / ADR-0106). Emits ffmpeg "
            "-vf pelorus_deband_vulkan=... strings; the live encode needs "
            "the Pelorus Vulkan filter in the ffmpeg build."
        ),
    )
    _add_prefilter_args(prefilter)


def _add_sidecar_subparser(sub: argparse._SubParsersAction) -> None:
    """Wire ``vmaf-tune sidecar`` onto the subparser group."""
    sidecar = sub.add_parser(
        "sidecar",
        help=(
            "train and inspect the local on-host predictor sidecar "
            "(ADR-0394 bias-correction model)"
        ),
    )
    _add_sidecar_args(sidecar)


def _register_subparsers(sub: argparse._SubParsersAction) -> None:
    """Register every public vmaf-tune subcommand in canonical order."""
    _add_corpus_subparser(sub)
    _add_recommend_subparser(sub)
    _add_predict_subparser(sub)
    _add_per_shot_subparser(sub)
    _add_recommend_saliency_subparser(sub)
    _add_ladder_subparser(sub)
    _add_compare_subparser(sub)
    _add_benchmark_subparser(sub)
    _add_auto_subparser(sub)
    _add_fast_subparser(sub)
    _add_prefilter_subparser(sub)
    _add_report_subparser(sub)
    _add_encode_profile_subparser(sub)
    _add_sidecar_subparser(sub)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="vmaf-tune",
        description=(
            "Quality-aware encode automation harness. Phase A drives a "
            "(preset, crf) grid through libx264 + libvmaf and emits a JSONL "
            "corpus."
        ),
    )
    parser.add_argument("--version", action="version", version=__version__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    _register_subparsers(sub)
    return parser


def _add_sidecar_common_args(p: argparse.ArgumentParser) -> None:
    """Wire the shared local-sidecar configuration flags."""
    from .sidecar import DEFAULT_PREDICTOR_VERSION

    p.add_argument(
        "--codec",
        default="libx264",
        choices=list(known_codecs()),
        help="codec bucket for the sidecar state (default libx264)",
    )
    p.add_argument(
        "--cache-dir",
        type=Path,
        default=None,
        help="sidecar cache root (default ${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar)",
    )
    p.add_argument(
        "--predictor-version",
        default=DEFAULT_PREDICTOR_VERSION,
        help=f"predictor version namespace (default {DEFAULT_PREDICTOR_VERSION})",
    )
    p.add_argument(
        "--model",
        type=Path,
        default=None,
        help="optional predictor_<codec>.onnx path; default uses analytical fallback",
    )


def _add_sidecar_args(p: argparse.ArgumentParser) -> None:
    """Wire ``vmaf-tune sidecar`` nested subcommands."""
    sub = p.add_subparsers(dest="sidecar_cmd", required=True)

    status = sub.add_parser("status", help="print sidecar state metadata")
    _add_sidecar_common_args(status)
    status.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    predict = sub.add_parser(
        "predict",
        help="predict VMAF with the sidecar correction folded in",
    )
    _add_sidecar_common_args(predict)
    predict.add_argument("--features-json", type=Path, required=True)
    predict.add_argument("--crf", type=int, required=True)
    predict.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    record = sub.add_parser(
        "record",
        help="record one observed encode result into the sidecar fit",
    )
    _add_sidecar_common_args(record)
    record.add_argument("--features-json", type=Path, required=True)
    record.add_argument("--crf", type=int, required=True)
    record.add_argument("--observed-vmaf", type=float, required=True)
    record.add_argument(
        "--no-persist",
        action="store_true",
        help="update in memory only; mainly useful for tests",
    )
    record.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    batch = sub.add_parser(
        "batch-record",
        help="record a JSONL capture file with one encode observation per row",
    )
    _add_sidecar_common_args(batch)
    batch.add_argument("--captures-jsonl", type=Path, required=True)
    batch.add_argument("--json", action="store_true", help="emit machine-readable JSON")


def _add_coarse_to_fine_flags(p: argparse.ArgumentParser) -> None:
    """Wire ``--coarse-to-fine`` + tunables onto a subparser.

    Used by both ``corpus`` (opt-in) and ``recommend`` (always on).
    """
    p.add_argument(
        "--coarse-to-fine",
        action="store_true",
        help=(
            "run a 2-pass coarse-then-fine CRF search instead of the "
            "full grid (ADR-0296). With defaults: 5 coarse + up to 10 "
            "fine = 15 encodes vs 52 for a full 0..51 sweep."
        ),
    )
    p.add_argument(
        "--coarse-step",
        type=int,
        default=10,
        help="CRF step for the coarse pass (default 10 -> [10,20,30,40,50])",
    )
    p.add_argument(
        "--fine-radius",
        type=int,
        default=5,
        help="±radius around best-coarse CRF for the fine pass (default 5)",
    )
    p.add_argument(
        "--fine-step",
        type=int,
        default=1,
        help="CRF step for the fine pass (default 1)",
    )
    p.add_argument(
        "--target-vmaf",
        type=float,
        default=None,
        help=(
            "target VMAF score; the orchestrator picks the smallest "
            "CRF whose score >= target. Optional for `corpus`, "
            "required for `recommend`."
        ),
    )


def _add_recommend_source_args(p: argparse.ArgumentParser) -> None:
    """Add optional source geometry and grid selectors."""
    p.add_argument("--source", type=Path, action="append", default=None)
    p.add_argument("--width", type=int, default=None)
    p.add_argument("--height", type=int, default=None)
    p.add_argument("--pix-fmt", default="yuv420p")
    p.add_argument("--framerate", type=float, default=24.0)
    p.add_argument("--duration", type=float, default=0.0)
    p.add_argument("--encoder", default="libx264", choices=list(known_codecs()))
    p.add_argument("--preset", action="append", default=None)


def _add_recommend_execution_args(p: argparse.ArgumentParser) -> None:
    """Add live-search output, binaries, and backend arguments."""
    p.add_argument(
        "--output",
        type=Path,
        default=Path("corpus.jsonl"),
        help="JSONL destination for the visited points",
    )
    p.add_argument(
        "--encode-dir",
        type=Path,
        default=Path(".workingdir/cache/vmafx-tune/encodes"),
    )
    p.add_argument("--keep-encodes", action="store_true")
    p.add_argument("--vmaf-model", default=DEFAULT_MODEL)
    _add_neg_flag(p)
    p.add_argument("--ffmpeg-bin", default="ffmpeg")
    p.add_argument("--vmaf-bin", default="vmaf")
    p.add_argument(
        "--score-backend",
        default="auto",
        choices=("auto", *ALL_BACKENDS),
        help=(
            "libvmaf scoring backend (default: auto; cuda > sycl > hip "
            "> cpu). See `vmaf-tune corpus --help`."
        ),
    )
    p.add_argument("--no-source-hash", action="store_true")
    p.add_argument(
        "--two-pass",
        action="store_true",
        help=(
            "Phase F (ADR-0333): run a 2-pass encode for codecs that "
            "support it. Default off; see `vmaf-tune corpus --help`."
        ),
    )
    _add_coarse_to_fine_flags(p)
    _add_recommend_uncertainty_flags(p)


def _add_recommend_existing_corpus_args(p: argparse.ArgumentParser) -> None:
    """Add existing-corpus selection and JSON output arguments."""
    p.add_argument(
        "--from-corpus",
        type=Path,
        default=None,
        metavar="JSONL",
        help=(
            "pick from an existing corpus JSONL instead of running new "
            "encodes. When set, --source / --width / --height / --preset "
            "are not required. Use --target-vmaf or --target-bitrate to "
            "select the recommendation strategy."
        ),
    )
    grp = p.add_mutually_exclusive_group()
    grp.add_argument(
        "--target-bitrate",
        type=float,
        default=None,
        metavar="KBPS",
        help=(
            "when using --from-corpus: pick the row whose bitrate is "
            "closest to this target (in kbps)."
        ),
    )
    p.add_argument(
        "--json",
        action="store_true",
        dest="json_output",
        help="emit the recommendation as a single JSON object to stdout.",
    )


def _add_recommend_args(p: argparse.ArgumentParser) -> None:
    """Mirror corpus inputs while adding existing-corpus selection."""
    _add_recommend_source_args(p)
    _add_recommend_execution_args(p)
    _add_recommend_existing_corpus_args(p)


def _add_recommend_uncertainty_flags(p: argparse.ArgumentParser) -> None:
    """Wire the ADR-0279 conformal-interval flags onto ``recommend``.

    These flags are passive when ``--with-uncertainty`` is omitted —
    the existing point-estimate recipe runs unchanged. When set, the
    recommend search loop reads conformal intervals from the
    coarse-to-fine row stream's ``vmaf_interval`` blocks (or, for
    tests, from a JSON sidecar) and short-circuits / widens search
    according to :func:`vmaftune.recommend.pick_target_vmaf_with_uncertainty`.
    """
    p.add_argument(
        "--with-uncertainty",
        action="store_true",
        help=(
            "consume conformal prediction intervals (per ADR-0279 / "
            "PR #488) when picking the recommended CRF. Tight "
            "intervals short-circuit the search early; wide "
            "intervals fall back to the full point-estimate scan "
            "with the result tagged ``(UNCERTAIN)``."
        ),
    )
    p.add_argument(
        "--uncertainty-sidecar",
        type=Path,
        default=None,
        help=(
            "path to a calibration sidecar (JSON, schema documented "
            "in vmaftune.uncertainty.load_confidence_thresholds). "
            "Defaults to the documented Research-0067 floor "
            "(tight=2.0, wide=5.0 VMAF) when absent."
        ),
    )


def _build_opts(args: argparse.Namespace) -> CorpusOptions:
    # ADR-0299 / ADR-0314: resolve --score-backend up-front so an
    # unavailable backend errors out before we burn cycles on encodes.
    # `select_backend` raises `BackendUnavailableError` (caught by the
    # caller) when a non-auto backend is requested but the host can't
    # provide it.
    selected = select_backend(prefer=args.score_backend, vmaf_bin=args.vmaf_bin)
    sys.stderr.write(f"vmaf-tune: scoring backend = {selected}\n")
    return CorpusOptions(
        encoder=args.encoder,
        output=args.output,
        encode_dir=args.encode_dir,
        vmaf_model=_resolve_vmaf_model(args),
        ffmpeg_bin=args.ffmpeg_bin,
        vmaf_bin=args.vmaf_bin,
        keep_encodes=args.keep_encodes,
        src_sha256=not args.no_source_hash,
        sample_clip_seconds=getattr(args, "sample_clip_seconds", 0.0),
        score_backend=selected,
        hdr_mode=getattr(args, "hdr_mode", "auto"),
        ffprobe_bin=getattr(args, "ffprobe_bin", "ffprobe"),
        two_pass=getattr(args, "two_pass", False),
    )


def _build_job(args: argparse.Namespace, src: Path, cells: tuple) -> CorpusJob:
    return CorpusJob(
        source=src,
        width=args.width,
        height=args.height,
        pix_fmt=args.pix_fmt,
        framerate=args.framerate,
        duration_s=args.duration,
        cells=cells,
    )


def _run_corpus(args: argparse.Namespace) -> int:
    try:
        opts = _build_opts(args)
    except BackendUnavailableError as exc:
        sys.stderr.write(f"vmaf-tune: {exc}\n")
        return 2

    if args.coarse_to_fine:
        # Coarse-to-fine ignores --crf and uses the configured grid.
        # Use a sentinel preset-only cell list so coarse_to_fine_search
        # can extract the preset axis.
        if not args.preset:
            sys.stderr.write("--preset is required\n")
            return 2
        sentinel_cells = tuple((p, 0) for p in args.preset)

        def _all_rows():
            for src in args.source:
                job = _build_job(args, src, sentinel_cells)
                yield from coarse_to_fine_search(
                    job,
                    opts,
                    target_vmaf=args.target_vmaf,
                    coarse_step=args.coarse_step,
                    fine_radius=args.fine_radius,
                    fine_step=args.fine_step,
                )

        n = write_jsonl(_all_rows(), opts.output)
        sys.stderr.write(f"coarse-to-fine: wrote {n} rows -> {opts.output}\n")
        return 0

    if not args.crf:
        sys.stderr.write("--crf is required (or use --coarse-to-fine)\n")
        return 2
    cells = tuple(iter_grid(args.preset, args.crf))

    def _all_rows():
        for src in args.source:
            job = _build_job(args, src, cells)
            yield from iter_rows(job, opts)

    n = write_jsonl(_all_rows(), opts.output)
    sys.stderr.write(f"wrote {n} rows -> {opts.output}\n")
    return 0


def _read_corpus_rows(corpus_path: Path) -> list[dict[str, Any]]:
    """Read non-empty JSONL rows from a corpus path."""
    rows: list[dict] = []
    with corpus_path.open(encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def _float_or_nan(value: object) -> float:
    """Coerce one display value, retaining infinities and mapping errors to NaN."""
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def _run_uncertainty_corpus_pick(
    args: argparse.Namespace, rows: list[dict[str, Any]], target_vmaf: float
) -> int:
    """Select and emit an uncertainty-aware target-VMAF row."""
    from .recommend import UncertaintyAwareRequest, pick_target_vmaf_with_uncertainty
    from .uncertainty import load_confidence_thresholds

    thresholds = load_confidence_thresholds(getattr(args, "uncertainty_sidecar", None))
    try:
        result = pick_target_vmaf_with_uncertainty(
            rows,
            UncertaintyAwareRequest(
                target_vmaf=target_vmaf,
                thresholds=thresholds,
                encoder=args.encoder,
                preset=args.preset[0] if args.preset else None,
            ),
        )
    except ValueError as exc:
        sys.stderr.write(f"recommend: {exc}\n")
        return 2
    if getattr(args, "json_output", False):
        sys.stdout.write(json.dumps(result.row) + "\n")
        return 0
    row = result.row
    status = "UNMET" if result.margin < 0 else "OK"
    sys.stdout.write(
        f"crf={row.get('crf', '?')}  vmaf={row.get('vmaf_score', math.nan):.3f}  "
        f"kbps={row.get('bitrate_kbps', math.nan):.0f}  predicate={result.predicate}"
        f"  decision={result.decision.value}  visited={result.visited}/{len(rows)}"
        f"  [{status}]\n"
    )
    return 0


def _run_point_corpus_pick(
    args: argparse.Namespace,
    rows: list[dict[str, Any]],
    target_vmaf: float | None,
    target_bitrate: float | None,
) -> int:
    """Select and emit the legacy point-estimate recommendation."""
    from .recommend import RecommendRequest, recommend

    try:
        pick = recommend(
            rows,
            RecommendRequest(
                target_vmaf=target_vmaf,
                target_bitrate_kbps=target_bitrate,
                encoder=args.encoder,
                preset=args.preset[0] if args.preset else None,
            ),
        )
    except ValueError as exc:
        sys.stderr.write(f"recommend: {exc}\n")
        return 2
    if getattr(args, "json_output", False):
        sys.stdout.write(json.dumps(pick.row) + "\n")
        return 0
    crf = pick.row.get("crf", "?")
    vmaf = _float_or_nan(pick.row.get("vmaf_score"))
    kbps = _float_or_nan(pick.row.get("bitrate_kbps"))
    status = "UNMET" if pick.margin < 0 else "OK"
    sys.stdout.write(
        f"crf={crf}  vmaf={vmaf:.3f}  kbps={kbps:.0f}  predicate={pick.predicate}  [{status}]\n"
    )
    return 0


def _run_recommend_from_corpus(args: argparse.Namespace) -> int:
    """Pick a recommendation from a pre-built corpus JSONL (no new encodes)."""
    corpus_path: Path = args.from_corpus
    if not corpus_path.exists():
        sys.stderr.write(f"recommend: corpus file not found: {corpus_path}\n")
        return 2
    rows = _read_corpus_rows(corpus_path)
    target_vmaf = getattr(args, "target_vmaf", None)
    target_bitrate = getattr(args, "target_bitrate", None)
    if target_vmaf is not None and target_bitrate is not None:
        sys.stderr.write("recommend: --target-vmaf and --target-bitrate are mutually exclusive\n")
        return 2
    with_uncertainty = bool(getattr(args, "with_uncertainty", False))
    if with_uncertainty and target_bitrate is not None:
        sys.stderr.write(
            "recommend: --with-uncertainty is not supported with --target-bitrate; "
            "falling back to point-estimate\n"
        )
        with_uncertainty = False
    if with_uncertainty and target_vmaf is not None:
        return _run_uncertainty_corpus_pick(args, rows, target_vmaf)
    return _run_point_corpus_pick(args, rows, target_vmaf, target_bitrate)


def _validate_live_recommend_args(args: argparse.Namespace) -> bool:
    """Validate arguments required by the encode-driven recommend path."""
    if not args.source or not args.width or not args.height or not args.preset:
        sys.stderr.write(
            "recommend: --source, --width, --height, --preset are required "
            "unless --from-corpus is used\n"
        )
        return False
    if args.target_vmaf is None:
        sys.stderr.write("recommend requires --target-vmaf\n")
        return False
    return True


def _collect_live_recommend_rows(
    args: argparse.Namespace, opts: CorpusOptions
) -> list[dict[str, Any]]:
    """Run coarse-to-fine search and persist every visited row."""
    sentinel_cells = tuple((preset, 0) for preset in args.preset)
    visited: list[dict[str, Any]] = []

    def _capture():
        for src in args.source:
            job = _build_job(args, src, sentinel_cells)
            for row in coarse_to_fine_search(
                job,
                opts,
                target_vmaf=args.target_vmaf,
                coarse_step=args.coarse_step,
                fine_radius=args.fine_radius,
                fine_step=args.fine_step,
            ):
                visited.append(row)
                yield row

    write_jsonl(_capture(), opts.output)
    return visited


def _emit_live_uncertainty_pick(
    args: argparse.Namespace, visited: list[dict[str, Any]], output: Path
) -> int:
    """Emit an uncertainty-aware result from live search rows."""
    from .recommend import UncertaintyAwareRequest, pick_target_vmaf_with_uncertainty
    from .uncertainty import load_confidence_thresholds

    thresholds = load_confidence_thresholds(getattr(args, "uncertainty_sidecar", None))
    try:
        result = pick_target_vmaf_with_uncertainty(
            visited,
            UncertaintyAwareRequest(target_vmaf=args.target_vmaf, thresholds=thresholds),
        )
    except ValueError as exc:
        sys.stderr.write(
            f"recommend: uncertainty pick failed ({exc}); "
            f"visited {len(visited)} encodes -> {output}\n"
        )
        return 1
    row = result.row
    sys.stdout.write(
        f"src={row.get('src')} preset={row.get('preset')} "
        f"crf={row.get('crf')} vmaf={float(row['vmaf_score']):.3f} "
        f"decision={result.decision.value} visited={result.visited}/{len(visited)} "
        f"predicate={result.predicate}\n"
    )
    return 0


def _emit_live_point_pick(
    args: argparse.Namespace, visited: list[dict[str, Any]], output: Path
) -> int:
    """Emit the legacy smallest-passing-CRF result."""
    pick = _smallest_passing_crf(visited, args.target_vmaf)
    if pick is None:
        sys.stderr.write(
            f"recommend: no CRF meets target VMAF >= {args.target_vmaf}; "
            f"visited {len(visited)} encodes -> {output}\n"
        )
        return 1
    src, preset, crf, score = pick
    sys.stdout.write(
        f"src={src} preset={preset} crf={crf} vmaf={score:.3f} "
        f"(visited {len(visited)} encodes)\n"
    )
    return 0


def _run_recommend(args: argparse.Namespace) -> int:
    """Run existing-corpus or encode-driven recommendation."""
    if getattr(args, "from_corpus", None) is not None:
        return _run_recommend_from_corpus(args)
    if not _validate_live_recommend_args(args):
        return 2

    try:
        opts = _build_opts(args)
    except BackendUnavailableError as exc:
        sys.stderr.write(f"vmaf-tune: {exc}\n")
        return 2
    visited = _collect_live_recommend_rows(args, opts)
    if getattr(args, "with_uncertainty", False):
        return _emit_live_uncertainty_pick(args, visited, opts.output)
    return _emit_live_point_pick(args, visited, opts.output)


def _smallest_passing_crf(
    rows: list[dict], target_vmaf: float
) -> tuple[str, str, int, float] | None:
    """Return (src, preset, crf, vmaf) for the highest-quality passing encode.

    Picks the SMALLEST CRF whose ``vmaf_score`` still meets ``target_vmaf``
    — for libx264 a smaller CRF means higher quality / larger bitrate, so
    the smallest passing CRF is the highest quality that clears the gate.
    This matches the CLI help: "find the smallest CRF whose VMAF >= --target-vmaf".
    Grouped per (src, preset); we return the first such (src, preset) pair
    in the natural row order.
    """
    best: dict[tuple[str, str], tuple[int, float]] = {}
    for r in rows:
        try:
            score = float(r.get("vmaf_score"))
        except (TypeError, ValueError):
            continue
        if score < target_vmaf:
            continue
        key = (str(r["src"]), str(r["preset"]))
        crf = int(r["crf"])
        cur = best.get(key)
        # We want the SMALLEST CRF that still meets the target — that's
        # the highest quality at acceptable cost. Tie-break on the
        # higher VMAF score for determinism.
        if cur is None or crf < cur[0] or (crf == cur[0] and score > cur[1]):
            best[key] = (crf, score)
    if not best:
        return None
    # Return the first key in row order.
    for r in rows:
        key = (str(r["src"]), str(r["preset"]))
        if key in best:
            crf, score = best[key]
            return key[0], key[1], crf, score
    return None


@dataclasses.dataclass(frozen=True)
class _PredictGeometry:
    """Source geometry shared by predict extraction and validation."""

    width: int
    height: int
    fps: float
    pix_fmt: str = "yuv420p"


def _build_predict_feature_config(args: argparse.Namespace) -> Any:
    """Build the predictor feature-extractor configuration."""
    from .predictor_features import FeatureExtractorConfig

    return FeatureExtractorConfig(
        ffmpeg_bin=args.ffmpeg_bin,
        ffprobe_bin=args.ffprobe_bin,
        use_saliency=args.use_saliency,
        saliency_model=args.saliency_model,
    )


def _probe_predict_geometry(args: argparse.Namespace, feat_cfg: Any) -> _PredictGeometry | None:
    """Probe source geometry and report unsafe missing dimensions."""
    from .predictor_features import _probe_video_geometry

    width, height, fps = _probe_video_geometry(args.source, feat_cfg, subprocess.run)
    if width <= 0 or height <= 0:
        print(
            "predict: ffprobe could not read source geometry "
            "(width/height); falling back is not safe — aborting.",
            file=sys.stderr,
        )
        return None
    return _PredictGeometry(width, height, fps)


def _detect_predict_shots(args: argparse.Namespace, geometry: _PredictGeometry) -> list[Shot]:
    """Detect validation shots using the canonical per-shot binary seam."""
    return detect_shots(
        Path(args.source),
        width=geometry.width,
        height=geometry.height,
        bitdepth=args.bitdepth,
        total_frames=args.total_frames or 0,
        per_shot_bin=args.per_shot_bin,
    )


def _build_predict_feature_extractor(args: argparse.Namespace, feat_cfg: Any) -> Callable:
    """Bind the source, codec, and feature config for predictor validation."""
    from .predictor_features import extract_features

    def _features(shot):
        return extract_features(
            shot=shot,
            source=args.source,
            codec=args.codec,
            config=feat_cfg,
        )

    return _features


def _predict_reference_command(
    args: argparse.Namespace, geometry: _PredictGeometry, shot: Shot, ref_yuv: Path
) -> list[str]:
    """Build the FFmpeg command that extracts one validation shot."""
    seek = f"{shot.start_frame / geometry.fps:.6f}" if geometry.fps > 0.0 else str(shot.start_frame)
    return [
        args.ffmpeg_bin,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-ss",
        seek,
        "-i",
        str(args.source),
        "-frames:v",
        str(shot.length),
        "-pix_fmt",
        geometry.pix_fmt,
        "-f",
        "rawvideo",
        str(ref_yuv),
    ]


def _predict_decode_command(
    args: argparse.Namespace, src: Path, dst: Path, pix_fmt: str
) -> list[str]:
    """Build the encoded-container to raw-YUV decode command."""
    return [
        args.ffmpeg_bin,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(src),
        "-f",
        "rawvideo",
        "-pix_fmt",
        pix_fmt,
        str(dst),
    ]


def _build_real_predict_scorer(
    args: argparse.Namespace, workdir: Path, geometry: _PredictGeometry
) -> Callable[[Shot, int, str], tuple[Path, float]]:
    """Bind the production encode-and-score callback for validation."""
    from .encode import EncodeRequest, run_encode
    from .score import ScoreRequest, run_score

    def _real_encode_and_score(shot: Shot, crf: int, codec: str) -> tuple[Path, float]:
        ref_yuv = workdir / f"ref_{shot.start_frame}_{shot.end_frame}.yuv"
        dist_path = workdir / f"dist_{shot.start_frame}_{shot.end_frame}.mp4"
        extract_cmd = _predict_reference_command(args, geometry, shot, ref_yuv)
        completed = subprocess.run(extract_cmd, capture_output=True, text=True, check=False)
        if completed.returncode != 0 or not ref_yuv.exists():
            return dist_path, float("nan")
        encode_req = EncodeRequest(
            source=ref_yuv,
            width=geometry.width,
            height=geometry.height,
            pix_fmt=geometry.pix_fmt,
            framerate=geometry.fps if geometry.fps > 0.0 else 24.0,
            encoder=codec,
            preset="medium",
            crf=crf,
            output=dist_path,
        )
        encode_result = run_encode(encode_req, ffmpeg_bin=args.ffmpeg_bin)
        if encode_result.exit_status != 0 or not dist_path.exists():
            return dist_path, float("nan")
        dist_yuv = workdir / f"dist_{shot.start_frame}_{shot.end_frame}.decoded.yuv"
        decode_cmd = _predict_decode_command(args, dist_path, dist_yuv, geometry.pix_fmt)
        dec = subprocess.run(decode_cmd, capture_output=True, text=True, check=False)
        dist_for_score = dist_yuv if dec.returncode == 0 and dist_yuv.exists() else dist_path
        score_req = ScoreRequest(
            reference=ref_yuv,
            distorted=dist_for_score,
            width=geometry.width,
            height=geometry.height,
            pix_fmt=geometry.pix_fmt,
        )
        score_result = run_score(score_req)
        return dist_path, float(score_result.vmaf_score)

    return _real_encode_and_score


def _load_predict_calibration(args: argparse.Namespace) -> tuple[Any | None, bool]:
    """Load optional conformal calibration and report degraded mode."""
    if not args.with_uncertainty:
        return None, False
    if args.calibration_sidecar is None:
        return None, True
    from .conformal import load_split_calibration

    return load_split_calibration(args.calibration_sidecar), False


def _predict_interval(
    args: argparse.Namespace, calibration: Any | None, predicted_vmaf: float
) -> dict[str, float | None] | None:
    """Project one point estimate through the loaded residual quantile."""
    if not args.with_uncertainty:
        return None
    if calibration is None:
        return {"low": predicted_vmaf, "high": predicted_vmaf, "alpha": None}
    cal = (
        dataclasses.replace(calibration, alpha=args.alpha)
        if args.alpha is not None
        else calibration
    )
    quantile = cal.quantile()
    return {
        "low": max(0.0, min(100.0, predicted_vmaf - quantile)),
        "high": max(0.0, min(100.0, predicted_vmaf + quantile)),
        "alpha": cal.alpha,
    }


def _predict_residual_row(args: argparse.Namespace, calibration: Any | None, residual: Any) -> dict:
    """Serialize one validation residual with optional interval."""
    row = {
        "shot_start": residual.shot.start_frame,
        "shot_end": residual.shot.end_frame,
        "crf": residual.crf_picked,
        "predicted_vmaf": residual.predicted_vmaf,
        "measured_vmaf": residual.measured_vmaf,
        "residual": residual.residual,
    }
    if args.with_uncertainty:
        row["interval"] = _predict_interval(args, calibration, residual.predicted_vmaf)
    return row


def _predict_payload(
    args: argparse.Namespace, report: Any, calibration: Any | None, uncalibrated: bool
) -> dict[str, Any]:
    """Build the stable Phase-C JSON report payload."""
    return {
        "verdict": report.verdict.value,
        "target_vmaf": report.target_vmaf,
        "residual_threshold": report.threshold_vmaf,
        "max_abs_residual": report.max_abs_residual,
        "mean_residual": report.mean_residual,
        "bias_correction": report.bias_correction,
        "k_validated": len(report.residuals),
        "uncertainty": {
            "enabled": bool(args.with_uncertainty),
            "calibrated": args.with_uncertainty and not uncalibrated,
            "alpha": (
                (args.alpha if args.alpha is not None else calibration.alpha)
                if calibration is not None
                else None
            ),
        },
        "residuals": [_predict_residual_row(args, calibration, row) for row in report.residuals],
    }


def _emit_predict_payload(args: argparse.Namespace, payload: dict[str, Any]) -> None:
    """Write the predictor report to its configured destination."""
    rendered = json.dumps(payload, indent=2)
    if args.report_out is not None:
        args.report_out.write_text(rendered + "\n", encoding="utf-8")
    else:
        sys.stdout.write(rendered + "\n")


def _run_predict(args: argparse.Namespace) -> int:
    """Run Phase-C per-shot prediction and real-score validation."""
    from .predictor import Predictor
    from .predictor_validate import Verdict, validate_predictor

    feat_cfg = _build_predict_feature_config(args)
    geometry = _probe_predict_geometry(args, feat_cfg)
    if geometry is None:
        return 1
    shots = _detect_predict_shots(args, geometry)
    if not shots:
        print("predict: no shots detected; nothing to do", file=sys.stderr)
        return 1
    predictor = Predictor(model_path=args.model)
    if predictor.is_stub:
        print(
            f"warning: predictor model '{args.model}' is a synthetic stub "
            "(not authoritative for production CRF picks)",
            file=sys.stderr,
        )
    feature_extractor = _build_predict_feature_extractor(args, feat_cfg)
    with tempfile.TemporaryDirectory(prefix="vmaf-tune-predict-") as temp_dir:
        scorer = _build_real_predict_scorer(args, Path(temp_dir), geometry)
        report = validate_predictor(
            predictor=predictor,
            shots=shots,
            target_vmaf=args.target_vmaf,
            codec=args.codec,
            feature_extractor=feature_extractor,
            real_encode_and_score=scorer,
            k=args.validate_k,
            residual_threshold_vmaf=args.residual_threshold,
        )
    calibration, uncalibrated = _load_predict_calibration(args)
    _emit_predict_payload(args, _predict_payload(args, report, calibration, uncalibrated))
    return 0 if report.verdict != Verdict.FALL_BACK else 2


def _precheck_per_shot_backend(args: argparse.Namespace) -> bool:
    """Fail fast when the requested scoring backend is unavailable."""
    try:
        resolved = select_backend(
            prefer=getattr(args, "score_backend", "auto"),
            vmaf_bin=getattr(args, "vmaf_bin", "vmaf"),
        )
    except BackendUnavailableError as exc:
        sys.stderr.write(f"vmaf-tune tune-per-shot: {exc}\n")
        return False
    sys.stderr.write(f"vmaf-tune tune-per-shot: scoring backend = {resolved}\n")
    return True


def _resolve_per_shot_geometry(args: argparse.Namespace) -> tuple[bool, int | None]:
    """Resolve container geometry, mutating args for downstream consistency."""
    width = args.width
    height = args.height
    framerate = args.framerate
    total_frames = args.total_frames if args.total_frames > 0 else None
    if not _source_needs_rawvideo_demux(args.src):
        if width is None or height is None or framerate is None:
            from .report import probe_source

            try:
                info = probe_source(args.src)
            except Exception as exc:
                sys.stderr.write(f"vmaf-tune tune-per-shot: ffprobe failed on {args.src}: {exc}\n")
                return False, None
            width = width if width is not None else info.width or None
            height = height if height is not None else info.height or None
            framerate = framerate if framerate is not None else info.fps if info.fps > 0.0 else None
            total_frames = total_frames or (info.frame_count if info.frame_count > 0 else None)
    elif width is None or height is None:
        sys.stderr.write(
            "vmaf-tune tune-per-shot: --width and --height are required "
            "for raw YUV sources. For container sources (mp4, mkv, …) "
            "these flags are optional and auto-probed via ffprobe.\n"
        )
        return False, None
    if width is None or height is None:
        sys.stderr.write(
            "vmaf-tune tune-per-shot: could not determine source width/height. "
            "Pass --width and --height explicitly.\n"
        )
        return False, None
    args.width, args.height = width, height
    args.framerate = framerate if framerate is not None else 24.0
    return True, total_frames


def _detect_tuning_shots(args: argparse.Namespace, total_frames: int | None) -> list[Shot]:
    """Detect and optionally uniformly split shots for tuning."""
    max_shot_duration = getattr(args, "max_shot_duration", None)
    if max_shot_duration is not None and max_shot_duration <= 0.0:
        max_shot_duration = None
    return detect_shots(
        args.src,
        width=args.width,
        height=args.height,
        pix_fmt=args.pix_fmt,
        bitdepth=args.bitdepth,
        total_frames=total_frames,
        per_shot_bin=args.per_shot_bin,
        diff_threshold=getattr(args, "scene_threshold", None),
        framerate=args.framerate,
        max_shot_duration_sec=max_shot_duration,
    )


def _configure_per_shot_decode_semaphore(args: argparse.Namespace) -> object:
    """Install and return the shared decode concurrency semaphore."""
    import threading

    from .bisect import set_decode_semaphore

    max_decodes = int(getattr(args, "max_concurrent_decodes", 1))
    set_decode_semaphore(max_decodes)
    return threading.Semaphore(max_decodes)


def _build_per_shot_nr_proxy(args: argparse.Namespace) -> object | None:
    """Build the opt-in NR early-elimination backend."""
    if not getattr(args, "fast_nr", False):
        return None
    from .score_backend import NRProxyBackend, NRProxyBackendError

    try:
        proxy = NRProxyBackend()
    except NRProxyBackendError as exc:
        sys.stderr.write(f"vmaf-tune tune-per-shot: --fast-nr: {exc}\n")
        raise
    sys.stderr.write(
        "vmaf-tune tune-per-shot: --fast-nr enabled; "
        f"δ_fast={proxy.calibration_threshold:.1f} VMAF (NR early-elimination)\n"
    )
    return proxy


def _run_builtin_per_shot_tuning(
    args: argparse.Namespace, shots: list[Shot]
) -> tuple[list[Any], dict[tuple[int, int], float]]:
    """Run tuning through the built-in bisect predicate."""
    from .bisect import _workdir_parent

    parent = getattr(args, "workdir", None) or _workdir_parent()
    if parent is not None:
        parent.mkdir(parents=True, exist_ok=True)
    crf_range = _parse_optional_crf_range(args.crf_min, args.crf_max)
    decode_semaphore = _configure_per_shot_decode_semaphore(args)
    nr_proxy = _build_per_shot_nr_proxy(args)
    with tempfile.TemporaryDirectory(prefix="vmaf-tune-per-shot-", dir=parent) as scratch:
        predicate, sidecar = _build_per_shot_bisect_predicate(
            args,
            scratch=Path(scratch),
            crf_range=crf_range,
            decode_semaphore=decode_semaphore,
            nr_proxy_backend=nr_proxy,
        )
        recs = tune_per_shot(
            shots, target_vmaf=args.target_vmaf, encoder=args.encoder, predicate=predicate
        )
    return recs, sidecar


def _run_per_shot_tuning(
    args: argparse.Namespace, shots: list[Shot]
) -> tuple[list[Any], str, dict[tuple[int, int], float]]:
    """Run custom or built-in per-shot tuning and return bitrate evidence."""
    if args.predicate_module:
        predicate = _load_per_shot_predicate(args.predicate_module)
        recs = tune_per_shot(
            shots, target_vmaf=args.target_vmaf, encoder=args.encoder, predicate=predicate
        )
        return recs, args.predicate_module, {}
    recs, sidecar = _run_builtin_per_shot_tuning(args, shots)
    return recs, "bisect", sidecar


def _apply_shot_bitrates(recs: list[Any], sidecar: dict[tuple[int, int], float]) -> list[Any]:
    """Copy measured bisect bitrates into frozen shot recommendations."""
    return [
        dataclasses.replace(
            rec,
            bitrate_kbps=sidecar.get(
                (rec.shot.start_frame, rec.shot.end_frame),
                rec.bitrate_kbps,
            ),
        )
        for rec in recs
    ]


def _merge_per_shot_plan(args: argparse.Namespace, recs: list[Any]) -> Any:
    """Merge tuned shots into the canonical encode plan."""
    return merge_shots(
        recs,
        source=args.src,
        output=args.output,
        framerate=args.framerate,
        encoder=args.encoder,
        segment_dir=args.segment_dir,
        ffmpeg_bin=args.ffmpeg_bin,
    )


def _shot_bitrate_json(bitrate: float) -> float | None:
    """Map non-finite shot bitrates to strict-JSON null."""
    if not isinstance(bitrate, float) or not math.isfinite(bitrate):
        return None
    return round(bitrate, 2)


def _per_shot_plan_document(args: argparse.Namespace, plan: Any, predicate_label: str) -> dict:
    """Build the strict JSON-serializable shot plan document."""
    return {
        "encoder": plan.encoder,
        "framerate": plan.framerate,
        "predicate": predicate_label,
        "target_vmaf": args.target_vmaf,
        "shots": [
            {
                "start_frame": r.shot.start_frame,
                "end_frame": r.shot.end_frame,
                "crf": r.crf,
                "predicted_vmaf": r.predicted_vmaf,
                "bitrate_kbps": _shot_bitrate_json(r.bitrate_kbps),
            }
            for r in plan.recommendations
        ],
        "segment_commands": [list(c) for c in plan.segment_commands],
        "concat_command": list(plan.concat_command),
    }


def _write_per_shot_outputs(args: argparse.Namespace, plan: Any, plan_doc: dict) -> None:
    """Write plan JSON and optional shell script."""
    rendered = json.dumps(plan_doc, indent=2, sort_keys=True)
    if args.plan_out is None:
        sys.stdout.write(rendered + "\n")
    else:
        args.plan_out.parent.mkdir(parents=True, exist_ok=True)
        args.plan_out.write_text(rendered + "\n", encoding="utf-8")
        sys.stderr.write(f"wrote plan -> {args.plan_out}\n")

    if args.script_out is not None:
        args.script_out.parent.mkdir(parents=True, exist_ok=True)
        args.script_out.write_text(plan_to_shell_script(plan), encoding="utf-8")
        sys.stderr.write(f"wrote shell script -> {args.script_out}\n")


def _write_per_shot_concat_listing(args: argparse.Namespace, plan: Any) -> None:
    """Write the convenience concat listing, warning on non-authoritative failure."""
    if args.segment_dir is not None:
        seg_dir = args.segment_dir
    elif args.plan_out is not None:
        seg_dir = args.plan_out.parent / "segments"
    else:
        seg_dir = args.output.parent / "segments"
    try:
        write_concat_listing(plan, seg_dir / "concat.txt")
    except OSError as exc:
        sys.stderr.write(
            f"WARN: segments dir {seg_dir} not writable; "
            f"skipping concat listing ({exc}). "
            f"Plan JSON still emitted at {args.plan_out or 'stdout'}.\n"
        )


def _run_tune_per_shot(args: argparse.Namespace) -> int:
    """Detect, tune, and serialize per-shot recommendations."""
    if not _precheck_per_shot_backend(args):
        return 2
    geometry_ok, total_frames = _resolve_per_shot_geometry(args)
    if not geometry_ok:
        return 2
    shots = _detect_tuning_shots(args, total_frames)
    try:
        recs, predicate_label, sidecar = _run_per_shot_tuning(args, shots)
    except (AttributeError, ImportError, RuntimeError, ValueError) as exc:
        sys.stderr.write(f"vmaf-tune tune-per-shot: {exc}\n")
        return 2
    recs = _apply_shot_bitrates(recs, sidecar)
    plan = _merge_per_shot_plan(args, recs)
    plan_doc = _per_shot_plan_document(args, plan, predicate_label)
    _write_per_shot_outputs(args, plan, plan_doc)
    _write_per_shot_concat_listing(args, plan)
    return 0


def _parse_optional_crf_range(
    crf_min: int | None,
    crf_max: int | None,
) -> tuple[int, int] | None:
    """Validate optional ``--crf-min`` / ``--crf-max`` pairs."""
    if crf_min is None and crf_max is None:
        return None
    if crf_min is None or crf_max is None:
        raise ValueError("pass both --crf-min and --crf-max")
    if crf_min > crf_max:
        raise ValueError(f"invalid CRF range [{crf_min}, {crf_max}]")
    return (int(crf_min), int(crf_max))


def _build_per_shot_bisect_predicate(
    args: argparse.Namespace,
    *,
    scratch: Path,
    crf_range: tuple[int, int] | None,
    decode_semaphore: object | None = None,
    nr_proxy_backend: object | None = None,
) -> tuple[PerShotPredicateFn, dict[tuple[int, int], float]]:
    """Build the Phase-B bisect predicate and its measured-bitrate sidecar."""
    if args.width <= 0 or args.height <= 0:
        raise ValueError("--width and --height must be positive for per-shot bisect")
    if args.framerate <= 0:
        raise ValueError("--framerate must be positive for per-shot bisect")

    scratch.mkdir(parents=True, exist_ok=True)
    refs_dir = scratch / "refs"
    work_dir = scratch / "bisect"
    refs_dir.mkdir(parents=True, exist_ok=True)
    work_dir.mkdir(parents=True, exist_ok=True)
    # ADR-0613: backend pre-resolution now happens in _run_tune_per_shot
    # before _build_per_shot_bisect_predicate is called.  The resolved value
    # passes through args.score_backend (never "auto" at this point for
    # explicit requests; "auto" means let libvmaf self-select, mapped to None).
    score_backend = None if args.score_backend in (None, "auto") else args.score_backend

    # Sidecar dict: keyed by (start_frame, end_frame), populated by _predicate
    # so the caller can attach measured bitrate_kbps to each ShotRecommendation.
    bitrate_sidecar: dict[tuple[int, int], float] = {}

    def _predicate(shot: Shot, target_vmaf: float, encoder: str) -> tuple[int, float]:
        ref_yuv = refs_dir / f"shot_{shot.start_frame}_{shot.end_frame}.yuv"
        _extract_shot_to_raw_yuv(args, shot=shot, output=ref_yuv)
        result = bisect_target_vmaf(
            ref_yuv,
            encoder,
            float(target_vmaf),
            width=args.width,
            height=args.height,
            pix_fmt=args.pix_fmt,
            framerate=args.framerate,
            duration_s=shot.length / args.framerate,
            preset=args.preset,
            crf_range=crf_range,
            max_iterations=args.max_iterations,
            vmaf_model=_resolve_vmaf_model(args),
            score_backend=score_backend,
            ffmpeg_bin=args.ffmpeg_bin,
            vmaf_bin=args.vmaf_bin,
            workdir=work_dir / f"shot_{shot.start_frame}_{shot.end_frame}",
            decode_semaphore=decode_semaphore,  # ADR-0577
            nr_proxy_backend=nr_proxy_backend,  # ADR-0624 / ADR-0615
        )
        if not result.ok:
            raise RuntimeError(
                "bisect failed for shot " f"[{shot.start_frame}, {shot.end_frame}): {result.error}"
            )
        bitrate_sidecar[(shot.start_frame, shot.end_frame)] = result.bitrate_kbps
        return (result.best_crf, result.measured_vmaf)

    return _predicate, bitrate_sidecar


def _extract_shot_to_raw_yuv(
    args: argparse.Namespace,
    *,
    shot: Shot,
    output: Path,
) -> None:
    """Extract one half-open shot range to raw YUV for Phase-B scoring."""
    output.parent.mkdir(parents=True, exist_ok=True)
    start_seconds = shot.start_frame / args.framerate
    cmd = [
        args.ffmpeg_bin,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
    ]
    if _source_needs_rawvideo_demux(args.src):
        cmd.extend(
            [
                "-f",
                "rawvideo",
                "-pix_fmt",
                args.pix_fmt,
                "-s",
                f"{args.width}x{args.height}",
                "-r",
                str(args.framerate),
            ]
        )
    cmd.extend(
        [
            "-ss",
            f"{start_seconds:.6f}",
            "-i",
            str(args.src),
            "-frames:v",
            str(shot.length),
            "-pix_fmt",
            args.pix_fmt,
            "-f",
            "rawvideo",
            str(output),
        ]
    )
    completed = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if completed.returncode != 0 or not output.exists():
        tail = (completed.stderr or "").strip().splitlines()
        detail = tail[-1] if tail else "no stderr"
        raise RuntimeError(
            f"ffmpeg shot extraction failed for "
            f"[{shot.start_frame}, {shot.end_frame}) (exit={completed.returncode}): {detail}"
        )


def _source_needs_rawvideo_demux(src: Path) -> bool:
    """Return True for extension-only raw YUV inputs."""
    return src.suffix.lower() in {".yuv", ".raw"}


def _saliency_output_paths(output: Path) -> tuple[Path, Path | None]:
    """Resolve video output and optional sibling JSON report paths."""
    if output.suffix.lower() == ".json":
        return output.with_name(output.stem + "_encoded.mp4"), output
    return output, None


def _build_saliency_request(args: argparse.Namespace, output: Path) -> Any:
    """Build one saliency or plain encode request."""
    from .encode import EncodeRequest

    adapter = get_adapter(args.encoder)
    crf = args.crf if args.crf is not None else adapter.quality_default
    return EncodeRequest(
        source=args.src,
        width=args.width,
        height=args.height,
        pix_fmt=args.pix_fmt,
        framerate=args.framerate,
        encoder=args.encoder,
        preset=args.preset,
        crf=crf,
        output=output,
    )


def _run_saliency_encode(args: argparse.Namespace, request: Any) -> Any:
    """Run the requested plain or saliency-aware encode path."""
    if not args.saliency_aware:
        from .encode import run_encode

        return run_encode(request, ffmpeg_bin=args.ffmpeg_bin)
    from .saliency import SaliencyConfig, saliency_aware_encode

    config = SaliencyConfig(
        foreground_offset=args.saliency_offset,
        temporal_aggregator=args.saliency_aggregator,
        ema_alpha=args.saliency_ema_alpha,
        allow_unsupported_encoder_fallback=args.saliency_fallback_plain,
    )
    return saliency_aware_encode(
        request,
        duration_frames=args.duration_frames,
        model_path=args.saliency_model,
        config=config,
        ffmpeg_bin=args.ffmpeg_bin,
    )


def _saliency_payload(args: argparse.Namespace, result: Any) -> dict[str, Any]:
    """Build the stable saliency encode result payload."""
    return {
        "encoder": result.request.encoder,
        "preset": result.request.preset,
        "crf": result.request.crf,
        "output": str(result.request.output),
        "encode_size_bytes": result.encode_size_bytes,
        "encode_time_ms": result.encode_time_ms,
        "ffmpeg_version": result.ffmpeg_version,
        "encoder_version": result.encoder_version,
        "saliency_aware": bool(args.saliency_aware),
        "saliency_aggregator": args.saliency_aggregator,
        "exit_status": result.exit_status,
    }


def _emit_saliency_payload(payload: dict[str, Any], report_path: Path | None) -> None:
    """Emit saliency JSON to stdout or the requested report path."""
    payload_text = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if report_path is None:
        sys.stdout.write(payload_text)
        return
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(payload_text, encoding="utf-8")
    sys.stdout.write(str(report_path) + "\n")


def _run_recommend_saliency(args: argparse.Namespace) -> int:
    """Run one plain or saliency-aware encode and emit its result."""
    output, report_path = _saliency_output_paths(args.output)
    result = _run_saliency_encode(args, _build_saliency_request(args, output))
    _emit_saliency_payload(_saliency_payload(args, result), report_path)
    return result.exit_status


def _parse_resolutions(raw: str) -> list[tuple[int, int]]:
    """Parse ``--resolutions`` ``WxH,WxH,...`` into a list of int pairs."""
    out: list[tuple[int, int]] = []
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        if "x" not in token:
            raise SystemExit(f"vmaf-tune ladder: bad resolution {token!r}; expected WxH")
        w_str, _, h_str = token.partition("x")
        out.append((int(w_str), int(h_str)))
    return out


def _parse_target_vmafs(raw: str) -> list[float]:
    """Parse ``--target-vmafs`` ``95,90,85`` into a list of floats."""
    out: list[float] = []
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        out.append(float(token))
    return out


def _parse_ladder_crf_sweep(raw: str | None) -> tuple[int, ...] | None:
    """Parse the optional CRF sweep, rejecting malformed or empty values."""
    if not raw:
        return None
    try:
        sweep = tuple(int(token.strip()) for token in raw.split(",") if token.strip())
    except ValueError as exc:
        raise ValueError(f"invalid --crf-sweep: {exc}") from exc
    if not sweep:
        raise ValueError("--crf-sweep produced an empty list")
    return sweep


def _resolve_ladder_source_dimensions(
    args: argparse.Namespace, resolutions: list[tuple[int, int]]
) -> tuple[int, int]:
    """Resolve raw source dimensions, defaulting to the largest rung."""
    src_w = getattr(args, "src_width", None)
    src_h = getattr(args, "src_height", None)
    if src_w is None or src_h is None:
        max_w, max_h = max(resolutions, key=lambda wh: wh[0] * wh[1])
        src_w = max_w if src_w is None else src_w
        src_h = max_h if src_h is None else src_h
    return int(src_w), int(src_h)


def _resolve_ladder_backend(args: argparse.Namespace) -> str | None:
    """Resolve the score backend before any ladder encodes start."""
    raw_backend = getattr(args, "score_backend", "auto")
    resolved_backend = select_backend(
        prefer=raw_backend, vmaf_bin=getattr(args, "vmaf_bin", "vmaf")
    )
    sys.stderr.write(f"vmaf-tune ladder: scoring backend = {resolved_backend}\n")
    return None if raw_backend == "auto" and resolved_backend == "cpu" else resolved_backend


def _build_ladder_manifest(
    args: argparse.Namespace,
    resolutions: list[tuple[int, int]],
    target_vmafs: list[float],
    thresholds: Any | None,
) -> str:
    """Bind the production sampler and build one ladder manifest."""
    from .ladder import LadderPoint, build_and_emit, make_default_sampler

    src_w, src_h = _resolve_ladder_source_dimensions(args, resolutions)
    cloud_sink: list[LadderPoint] = []
    sampler = make_default_sampler(
        pix_fmt=getattr(args, "pix_fmt", "yuv420p"),
        framerate=float(getattr(args, "framerate", 24.0)),
        duration_s=float(getattr(args, "duration_s", 1.0)),
        crf_sweep=_parse_ladder_crf_sweep(getattr(args, "crf_sweep", None)),
        src_width=src_w,
        src_height=src_h,
        cloud_sink=cloud_sink,
        score_backend=_resolve_ladder_backend(args),
        vmaf_model=_resolve_vmaf_model(args),
    )
    return build_and_emit(
        src=args.src,
        encoder=args.encoder,
        resolutions=resolutions,
        target_vmafs=target_vmafs,
        quality_tiers=args.quality_tiers,
        format=args.format,
        spacing=args.spacing,
        sampler=sampler,
        with_uncertainty=bool(getattr(args, "with_uncertainty", False)),
        uncertainty_thresholds=thresholds,
        rung_overlap_threshold=getattr(args, "rung_overlap_threshold", None),
        extra_samples=cloud_sink,
    )


def _emit_ladder_manifest(output: Path | None, manifest: str) -> None:
    """Write a ladder manifest to a file or stdout."""
    if output is None:
        sys.stdout.write(manifest)
        if not manifest.endswith("\n"):
            sys.stdout.write("\n")
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(manifest, encoding="utf-8")
    sys.stderr.write(f"wrote ladder manifest -> {output}\n")


def _run_ladder(args: argparse.Namespace) -> int:
    """Build and emit a Phase-E per-title bitrate ladder."""
    from .uncertainty import load_confidence_thresholds

    thresholds = None
    if getattr(args, "with_uncertainty", False):
        thresholds = load_confidence_thresholds(getattr(args, "uncertainty_sidecar", None))
    try:
        manifest = _build_ladder_manifest(
            args,
            _parse_resolutions(args.resolutions),
            _parse_target_vmafs(args.target_vmafs),
            thresholds,
        )
    except (BackendUnavailableError, RuntimeError, ValueError, OSError) as exc:
        sys.stderr.write(f"vmaf-tune ladder: {exc}\n")
        return 2
    _emit_ladder_manifest(args.output, manifest)
    return 0


def _resolve_compare_source_geometry(
    src: Path,
    *,
    width: int | None,
    height: int | None,
    framerate: float,
    duration_s: float,
    framerate_was_default: bool,
    duration_was_default: bool,
    probe_fn: Callable[[Path], Any] | None = None,
    warn_stream: TextIO | None = None,
) -> tuple[int | None, int | None, float, float]:
    """Merge explicit geometry with best-effort container probe values."""
    from .score import VMAF_RAW_SUFFIXES

    if probe_fn is None:
        from .report import probe_source

        probe_fn = probe_source
    stream = warn_stream if warn_stream is not None else sys.stderr
    if Path(src).suffix.lower() in VMAF_RAW_SUFFIXES:
        return width, height, framerate, duration_s
    try:
        info = probe_fn(Path(src))
    except Exception as exc:
        stream.write(
            f"vmaf-tune compare: ffprobe of {src} failed ({exc}); "
            "using user-supplied geometry verbatim.\n"
        )
        return width, height, framerate, duration_s
    probed_fps = float(getattr(info, "fps", 0.0) or 0.0)
    probed_dur = float(getattr(info, "duration_s", 0.0) or 0.0)
    probed_w = int(getattr(info, "width", 0) or 0)
    probed_h = int(getattr(info, "height", 0) or 0)

    out_w = probed_w if width is None and probed_w > 0 else width
    out_h = probed_h if height is None and probed_h > 0 else height
    out_fr = framerate
    out_dur = duration_s
    if framerate_was_default and probed_fps > 0.0:
        out_fr = probed_fps
    elif not framerate_was_default and probed_fps > 0.0 and abs(probed_fps - framerate) > 0.01:
        stream.write(
            f"vmaf-tune compare: --framerate {framerate:g} disagrees with the "
            f"probed source rate {probed_fps:g} fps for {src}; using user "
            "override but frame-skip/cnt math may misalign reference vs. "
            "distorted YUV — pass --framerate to match the source if scores "
            "look wrong.\n"
        )
    if duration_was_default and probed_dur > 0.0:
        out_dur = probed_dur
    return out_w, out_h, out_fr, out_dur


_COMPARE_PROFILE_FORMATS = ("html", "both")


@dataclasses.dataclass(frozen=True)
class _CompareRuntime:
    """Resolved encoder runtime labels and per-token binaries."""

    vaapi_device: str
    specs: list[Any]
    encoders: list[str]
    by_token: dict[str, Any]


@dataclasses.dataclass(frozen=True)
class _CompareBisectConfig:
    """Inputs shared by every real-bisect predicate closure."""

    width: int
    height: int
    framerate: float
    duration_s: float
    crf_range: tuple[int, int] | None
    score_backend: str | None
    workdir: Path | None
    decode_semaphore: object
    nr_proxy: object | None


def _resolve_compare_runtime(args: argparse.Namespace) -> _CompareRuntime:
    """Resolve encoder tokens, runtime variants, and QSV device path."""
    import os

    from .compare import DEFAULT_CPU_ENCODERS, DEFAULT_VAAPI_DEVICE
    from .encoder_runtime import resolve_encoder_runtime_specs

    vaapi_device = (
        getattr(args, "vaapi_device", None)
        or os.environ.get("VMAFTUNE_VAAPI_DEVICE", "")
        or DEFAULT_VAAPI_DEVICE
    )
    raw = args.encoders if args.encoders is not None else ",".join(DEFAULT_CPU_ENCODERS)
    tokens = [token.strip() for token in raw.split(",") if token.strip()]
    if not tokens:
        raise ValueError("--encoders is empty")
    specs = resolve_encoder_runtime_specs(
        tokens,
        ffmpeg_bin=args.ffmpeg_bin,
        encoder_ffmpeg_bins=getattr(args, "encoder_ffmpeg_bin", ()),
    )
    return _CompareRuntime(
        vaapi_device=vaapi_device,
        specs=specs,
        encoders=[spec.token for spec in specs],
        by_token={spec.token: spec for spec in specs},
    )


def _annotate_compare_runtime(result: Any, spec: Any) -> Any:
    """Attach runtime identity fields to one comparison result."""
    return dataclasses.replace(
        result,
        codec=spec.token,
        adapter=spec.adapter,
        runtime_variant=spec.variant,
        ffmpeg_bin=spec.ffmpeg_bin,
    )


def _compare_row_metadata(runtime: _CompareRuntime) -> Callable[[str], dict[str, str]]:
    """Bind the runtime metadata callback consumed by compare workers."""

    def _metadata(codec: str) -> dict[str, str]:
        spec = runtime.by_token[codec]
        return {
            "adapter": spec.adapter,
            "runtime_variant": spec.variant,
            "ffmpeg_bin": spec.ffmpeg_bin,
        }

    return _metadata


def _validate_compare_format(args: argparse.Namespace) -> bool:
    """Validate compare output-format constraints."""
    from .compare import supported_formats

    supported = (*supported_formats(), *_COMPARE_PROFILE_FORMATS)
    if args.format not in supported:
        sys.stderr.write(
            f"vmaf-tune compare: unsupported --format {args.format!r}; expected one of {supported}\n"
        )
        return False
    if args.format == "both" and args.output is None:
        sys.stderr.write("vmaf-tune compare: --format both requires --output PATH\n")
        return False
    return True


def _precheck_compare_backend(args: argparse.Namespace) -> bool:
    """Fail before worker startup when a requested score backend is unavailable."""
    try:
        resolved = select_backend(
            prefer=getattr(args, "score_backend", None) or "auto",
            vmaf_bin=getattr(args, "vmaf_bin", "vmaf"),
        )
    except BackendUnavailableError as exc:
        sys.stderr.write(f"vmaf-tune compare: {exc}\n")
        return False
    sys.stderr.write(f"vmaf-tune compare: scoring backend = {resolved}\n")
    return True


def _parse_compare_targets(args: argparse.Namespace) -> list[float]:
    """Resolve legacy single-target versus default multi-target semantics."""
    use_legacy = getattr(args, "_target_vmafs_was_default", True) and not getattr(
        args, "_target_vmaf_was_default", True
    )
    if not args.target_vmafs or use_legacy:
        return [float(args.target_vmaf)]
    try:
        targets = sorted(
            {float(token.strip()) for token in args.target_vmafs.split(",") if token.strip()}
        )
    except ValueError as exc:
        raise ValueError(f"invalid --target-vmafs: {exc}") from exc
    if not targets:
        raise ValueError("--target-vmafs is empty")
    return targets


def _build_compare_nr_proxy(args: argparse.Namespace) -> object | None:
    """Build and announce the opt-in compare NR proxy backend."""
    if not getattr(args, "fast_nr", False):
        return None
    from .score_backend import NRProxyBackend, NRProxyBackendError

    try:
        proxy = NRProxyBackend()
    except NRProxyBackendError as exc:
        raise ValueError(f"--fast-nr: {exc}") from exc
    sys.stderr.write(
        "vmaf-tune compare: --fast-nr enabled; "
        f"δ_fast={proxy.calibration_threshold:.1f} VMAF (NR early-elimination)\n"
    )
    return proxy


def _resolve_compare_bisect_config(args: argparse.Namespace) -> _CompareBisectConfig:
    """Resolve geometry and resources shared by real bisect closures."""
    import threading

    from .bisect import set_decode_semaphore

    width, height, framerate, duration = _resolve_compare_source_geometry(
        Path(args.src),
        width=args.width,
        height=args.height,
        framerate=args.framerate,
        duration_s=args.duration,
        framerate_was_default=getattr(args, "_framerate_was_default", False),
        duration_was_default=getattr(args, "_duration_was_default", False),
    )
    if width is None or height is None:
        raise ValueError(
            "--width and --height are required for the real bisect backend. "
            "Use --predicate-module MODULE:CALLABLE to provide a custom predicate."
        )
    crf_range = _parse_optional_crf_range(args.crf_min, args.crf_max)
    max_decodes = int(getattr(args, "max_concurrent_decodes", 1))
    set_decode_semaphore(max_decodes)
    return _CompareBisectConfig(
        width=width,
        height=height,
        framerate=framerate,
        duration_s=duration,
        crf_range=crf_range,
        score_backend=None if args.score_backend in (None, "auto") else args.score_backend,
        workdir=getattr(args, "workdir", None),
        decode_semaphore=threading.Semaphore(max_decodes),
        nr_proxy=_build_compare_nr_proxy(args),
    )


def _build_real_compare_dispatcher(
    args: argparse.Namespace, runtime: _CompareRuntime, config: _CompareBisectConfig
) -> Callable[[str, Path, float], Any]:
    """Build a target-aware, memoized real-bisect dispatcher."""
    from .bisect import make_bisect_predicate

    cache: dict[tuple[str, float], Callable[[str, Path, float], Any]] = {}

    def _dispatcher(codec: str, src: Path, target: float) -> Any:
        spec = runtime.by_token[codec]
        target_value = float(target)
        key = (spec.ffmpeg_bin, target_value)
        if key not in cache:
            cache[key] = make_bisect_predicate(
                target_vmaf=target_value,
                width=config.width,
                height=config.height,
                pix_fmt=args.pix_fmt,
                framerate=config.framerate,
                duration_s=config.duration_s,
                sample_clip_seconds=args.sample_clip_seconds,
                preset=args.preset,
                crf_range=config.crf_range,
                max_iterations=args.max_iterations,
                vmaf_model=_resolve_vmaf_model(args),
                score_backend=config.score_backend,
                ffmpeg_bin=spec.ffmpeg_bin,
                vmaf_bin=args.vmaf_bin,
                workdir=config.workdir,
                decode_semaphore=config.decode_semaphore,
                nr_proxy_backend=config.nr_proxy,
            )
        result = cache[key](spec.adapter, src, target)
        return _annotate_compare_runtime(result, spec)

    return _dispatcher


def _build_compare_predicates(
    args: argparse.Namespace, runtime: _CompareRuntime
) -> tuple[Callable, Callable, float]:
    """Build custom or production compare predicates and resolved duration."""
    if args.predicate_module:
        try:
            loaded = _load_compare_predicate(args.predicate_module)
        except (AttributeError, ImportError, ValueError) as exc:
            raise ValueError(f"invalid --predicate-module: {exc}") from exc

        def _dispatcher(codec: str, src: Path, target: float) -> Any:
            spec = runtime.by_token[codec]
            return _annotate_compare_runtime(loaded(codec, src, target), spec)

        return _dispatcher, _dispatcher, float(args.duration)
    config = _resolve_compare_bisect_config(args)
    dispatcher = _build_real_compare_dispatcher(args, runtime, config)
    return dispatcher, dispatcher, config.duration_s


def _prepare_shared_compare_reference(
    args: argparse.Namespace, runtime: _CompareRuntime, resolved_duration: float
) -> Path | None:
    """Decode one shared raw reference for real container-source bisects."""
    from .bisect import _workdir_parent
    from .score import VMAF_RAW_SUFFIXES, _decode_to_raw_yuv

    src = Path(args.src)
    if src.suffix.lower() in VMAF_RAW_SUFFIXES or args.predicate_module:
        return None
    workdir = getattr(args, "workdir", None) or _workdir_parent()
    if workdir is None:
        workdir = Path(tempfile.mkdtemp(prefix="vmaftune-compare-"))
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    decoded = workdir / f"{src.stem}.shared-ref.yuv"
    if decoded.exists():
        return decoded
    sys.stderr.write(
        f"vmaf-tune compare: decoding shared reference YUV once "
        f"({src.name} -> {decoded.name}) ...\n"
    )
    duration = resolved_duration if resolved_duration > 0.0 else None
    rc = _decode_to_raw_yuv(
        src,
        decoded,
        pix_fmt=getattr(args, "pix_fmt", "yuv420p"),
        ffmpeg_bin=getattr(args, "ffmpeg_bin", "ffmpeg"),
        duration_s=duration,
    )
    if rc != 0 or not decoded.exists():
        sys.stderr.write(
            f"vmaf-tune compare: shared reference decode failed (rc={rc}); "
            "falling back to per-worker decode.\n"
        )
        return None
    gib = decoded.stat().st_size / 1024**3
    sys.stderr.write(
        f"vmaf-tune compare: shared reference decoded ({gib:.1f} GB) — "
        f"all {len(runtime.encoders)} workers will share it.\n"
    )
    return decoded


def _cleanup_shared_compare_reference(decoded: Path | None) -> None:
    """Delete the owned shared reference after all workers stop."""
    if decoded is None or not decoded.exists():
        return
    decoded.unlink()
    sys.stderr.write(f"vmaf-tune compare: deleted shared reference YUV ({decoded.name}).\n")


def _compare_availability_probe(
    args: argparse.Namespace, runtime: _CompareRuntime
) -> Callable[[str], tuple[bool, str]]:
    """Build the real encoder probe or custom-predicate no-op probe."""
    if args.predicate_module:
        return lambda _codec: (True, "")
    from .compare import probe_encoder_available

    def _probe(codec: str) -> tuple[bool, str]:
        spec = runtime.by_token[codec]
        return probe_encoder_available(
            spec.adapter, ffmpeg_bin=spec.ffmpeg_bin, vaapi_device=runtime.vaapi_device
        )

    return _probe


def _emit_compare_rendered(args: argparse.Namespace, rendered: str, label: str) -> None:
    """Emit one compare report and announce file output."""
    if args.output is None:
        sys.stdout.write(rendered)
        if not rendered.endswith("\n"):
            sys.stdout.write("\n")
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    kind = f"{label} " if label else ""
    sys.stderr.write(f"wrote compare {kind}report -> {args.output}\n")


def _run_compare_sweep_result(
    args: argparse.Namespace,
    runtime: _CompareRuntime,
    targets: list[float],
    predicate: Callable,
    decoded_ref: Path | None,
) -> int:
    """Run and emit a multi-target comparison sweep."""
    from .compare import compare_codecs_sweep, emit_sweep_report

    sweep = compare_codecs_sweep(
        src=args.src,
        target_vmafs=targets,
        encoders=runtime.encoders,
        parallel=not args.no_parallel,
        max_workers=args.max_workers,
        predicate=predicate,
        availability_probe=_compare_availability_probe(args, runtime),
        pre_decoded_ref=decoded_ref,
        row_metadata=_compare_row_metadata(runtime),
    )
    if args.format in _COMPARE_PROFILE_FORMATS:
        outputs = _write_compare_profile_report(args, sweep_report=sweep)
        if outputs:
            sys.stderr.write(
                "wrote compare profile report -> " + ", ".join(str(path) for path in outputs) + "\n"
            )
    else:
        _emit_compare_rendered(args, emit_sweep_report(sweep, format=args.format), "sweep")
    return 0 if any(row.ok for row in sweep.rows) else 1


def _run_compare_single_result(
    args: argparse.Namespace,
    runtime: _CompareRuntime,
    predicate: Callable,
    decoded_ref: Path | None,
) -> int:
    """Run and emit the legacy single-target comparison."""
    from .compare import compare_codecs, emit_report

    report = compare_codecs(
        src=args.src,
        target_vmaf=args.target_vmaf,
        encoders=runtime.encoders,
        parallel=not args.no_parallel,
        max_workers=args.max_workers,
        predicate=predicate,
        pre_decoded_ref=decoded_ref,
        row_metadata=_compare_row_metadata(runtime),
    )
    if args.format in _COMPARE_PROFILE_FORMATS:
        outputs = _write_compare_profile_report(args, comparison_report=report)
        if outputs:
            sys.stderr.write(
                "wrote compare profile report -> " + ", ".join(str(path) for path in outputs) + "\n"
            )
    else:
        _emit_compare_rendered(args, emit_report(report, format=args.format), "")
    return 0 if report.best() is not None else 1


def _execute_compare(
    args: argparse.Namespace,
    runtime: _CompareRuntime,
    targets: list[float],
    predicate: Callable,
    sweep_predicate: Callable,
    decoded_ref: Path | None,
) -> int:
    """Dispatch the single-target or multi-target comparison path."""
    if len(targets) > 1:
        return _run_compare_sweep_result(args, runtime, targets, sweep_predicate, decoded_ref)
    return _run_compare_single_result(args, runtime, predicate, decoded_ref)


def _run_compare(args: argparse.Namespace) -> int:
    """Compare codec runtimes through custom or production predicates."""
    try:
        runtime = _resolve_compare_runtime(args)
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune compare: {exc}\n")
        return 2
    if not _validate_compare_format(args) or not _precheck_compare_backend(args):
        return 2
    if getattr(args, "no_bisect", False):
        if args.format in _COMPARE_PROFILE_FORMATS:
            sys.stderr.write(
                "vmaf-tune compare --no-bisect: --format html/both is not supported; "
                "emit --format json and pass it to vmaf-tune report.\n"
            )
            return 2
        return _run_compare_crf_sweep(args, runtime.encoders, runtime_specs=runtime.specs)
    try:
        targets = _parse_compare_targets(args)
        predicate, sweep_predicate, duration = _build_compare_predicates(args, runtime)
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune compare: {exc}\n")
        return 2
    decoded_ref = _prepare_shared_compare_reference(args, runtime, duration)
    try:
        return _execute_compare(args, runtime, targets, predicate, sweep_predicate, decoded_ref)
    finally:
        _cleanup_shared_compare_reference(decoded_ref)


def _compare_source_info(args: argparse.Namespace):
    """Return source metadata for inline compare profile rendering."""
    from .report import SourceInfo, probe_source

    src = Path(args.src)
    probed = probe_source(src)
    try:
        size_bytes = src.stat().st_size
    except OSError:
        size_bytes = probed.size_bytes
    width = probed.width or int(getattr(args, "width", 0) or 0)
    height = probed.height or int(getattr(args, "height", 0) or 0)
    fps = probed.fps or float(getattr(args, "framerate", 0.0) or 0.0)
    duration_s = probed.duration_s or float(getattr(args, "duration", 0.0) or 0.0)
    frame_count = probed.frame_count or (int(duration_s * fps) if duration_s > 0 and fps > 0 else 0)
    return SourceInfo(
        path=probed.path,
        width=width,
        height=height,
        fps=fps,
        duration_s=duration_s,
        frame_count=frame_count,
        codec=probed.codec,
        size_bytes=size_bytes,
    )


def _build_compare_report_data(
    args: argparse.Namespace,
    comparison_report: Any | None,
    sweep_report: Any | None,
) -> Any:
    """Convert comparison results into profile-renderer data."""
    from datetime import datetime, timezone

    from .report import ReportData

    if comparison_report is None and sweep_report is None:
        raise ValueError("comparison_report or sweep_report is required")
    codec_rows: tuple[Any, ...] = ()
    sweep_points: tuple[Any, ...] = ()
    sweep_targets: tuple[float, ...] = ()
    if sweep_report is not None:
        rows = [
            result.to_row(target)
            for target, result in zip(sweep_report.row_targets, sweep_report.rows, strict=True)
        ]
        sweep_points = tuple(_sweep_point_from_json(row) for row in rows)
        sweep_targets = tuple(float(target) for target in sweep_report.target_vmafs)
        target_vmaf = float(sweep_targets[0]) if sweep_targets else float(args.target_vmaf)
    else:
        if comparison_report is None:
            raise ValueError("comparison_report is required")
        codec_rows = tuple(
            _codec_row_from_json(row.to_row(float(comparison_report.target_vmaf)))
            for row in comparison_report.rows
        )
        target_vmaf = float(comparison_report.target_vmaf)
    return ReportData(
        source=_compare_source_info(args),
        target_vmaf=target_vmaf,
        codec_rows=codec_rows,
        sweep_points=sweep_points,
        sweep_targets=sweep_targets,
        generated_at_iso=datetime.now(timezone.utc).isoformat(timespec="seconds"),
        encoder_preset=str(getattr(args, "preset", "") or ""),
        pix_fmt=str(getattr(args, "pix_fmt", "") or ""),
        score_backend=str(getattr(args, "score_backend", "") or ""),
        ffmpeg_bin=str(getattr(args, "ffmpeg_bin", "") or ""),
        vmaf_bin=str(getattr(args, "vmaf_bin", "") or ""),
    )


def _write_compare_profile_files(args: argparse.Namespace, data: Any, output: Path) -> list[Path]:
    """Write requested profile artifacts and return their paths."""
    from .report import render_html, render_markdown

    output.parent.mkdir(parents=True, exist_ok=True)
    outputs: list[Path] = []
    if args.format == "both" or getattr(args, "json_sidecar", False):
        json_path = output.with_suffix(".json")
        json_path.write_text(json.dumps(data.to_dict(), indent=2) + "\n", encoding="utf-8")
        outputs.append(json_path)
    if args.format in ("html", "both"):
        html_path = output if args.format == "html" else output.with_suffix(".html")
        html_path.write_text(render_html(data), encoding="utf-8")
        outputs.append(html_path)
    if args.format in ("markdown", "both"):
        markdown_path = output if args.format == "markdown" else output.with_suffix(".md")
        markdown_path.write_text(render_markdown(data), encoding="utf-8")
        outputs.append(markdown_path)
    return outputs


def _write_compare_profile_report(
    args: argparse.Namespace,
    *,
    comparison_report: Any | None = None,
    sweep_report: Any | None = None,
) -> list[Path]:
    """Render compare results through the profile-card renderer."""
    from .report import render_html

    data = _build_compare_report_data(args, comparison_report, sweep_report)
    output = getattr(args, "output", None)
    if output is not None:
        return _write_compare_profile_files(args, data, Path(output))
    if getattr(args, "json_sidecar", False):
        raise ValueError("--json-sidecar requires --output PATH")
    if args.format != "html":
        raise ValueError("--format both requires --output PATH")
    rendered = render_html(data)
    sys.stdout.write(rendered)
    if not rendered.endswith("\n"):
        sys.stdout.write("\n")
    return []


@dataclasses.dataclass(frozen=True)
class _CompareSweepContext:
    """Resolved inputs shared by each CRF-sweep cell."""

    specs: tuple[Any, ...]
    by_token: dict[str, Any]
    width: int
    height: int
    framerate: float
    duration_s: float
    score_backend: str | None
    vaapi_device: str


def _parse_compare_crf_values(args: argparse.Namespace) -> list[int]:
    """Parse the required no-bisect CRF list."""
    raw = getattr(args, "crf_sweep", None)
    if not raw:
        raise ValueError("--crf-sweep LIST is required. Example: --crf-sweep 18,23,28,33")
    try:
        values = [int(token.strip()) for token in raw.split(",") if token.strip()]
    except ValueError as exc:
        raise ValueError(f"invalid --crf-sweep: {exc}") from exc
    if not values:
        raise ValueError("--crf-sweep produced an empty list")
    return values


def _resolve_compare_sweep_context(
    args: argparse.Namespace,
    encoders: list[str],
    runtime_specs: tuple[Any, ...] | list[Any] | None,
) -> _CompareSweepContext:
    """Resolve runtime variants, device path, geometry, and score backend."""
    import os

    from .compare import DEFAULT_VAAPI_DEVICE
    from .encoder_runtime import resolve_encoder_runtime_specs

    specs = tuple(
        runtime_specs
        or resolve_encoder_runtime_specs(
            encoders,
            ffmpeg_bin=getattr(args, "ffmpeg_bin", "ffmpeg"),
            encoder_ffmpeg_bins=getattr(args, "encoder_ffmpeg_bin", ()),
        )
    )
    width, height, framerate, duration = _resolve_compare_source_geometry(
        Path(args.src),
        width=args.width,
        height=args.height,
        framerate=args.framerate,
        duration_s=args.duration,
        framerate_was_default=getattr(args, "_framerate_was_default", False),
        duration_was_default=getattr(args, "_duration_was_default", False),
    )
    if width is None or height is None:
        raise ValueError("--width and --height are required.")
    return _CompareSweepContext(
        specs=specs,
        by_token={spec.token: spec for spec in specs},
        width=width,
        height=height,
        framerate=framerate,
        duration_s=duration,
        score_backend=None if args.score_backend in (None, "auto") else args.score_backend,
        vaapi_device=(
            getattr(args, "vaapi_device", None)
            or os.environ.get("VMAFTUNE_VAAPI_DEVICE", "")
            or DEFAULT_VAAPI_DEVICE
        ),
    )


def _compare_sweep_availability(
    context: _CompareSweepContext,
) -> dict[str, tuple[bool, str]]:
    """Probe every configured encoder once."""
    from .compare import probe_encoder_available

    return {
        spec.token: probe_encoder_available(
            spec.adapter,
            ffmpeg_bin=spec.ffmpeg_bin,
            vaapi_device=context.vaapi_device,
        )
        for spec in context.specs
    }


def _unavailable_compare_sweep_row(spec: Any, crf: int, reason: str) -> dict[str, Any]:
    """Build the stable failed-cell schema for an unavailable encoder."""
    return {
        "codec": spec.token,
        "adapter": spec.adapter,
        "runtime_variant": spec.variant,
        "ffmpeg_bin": spec.ffmpeg_bin,
        "crf": crf,
        "bitrate_kbps": float("nan"),
        "vmaf_score": float("nan"),
        "encode_time_ms": 0.0,
        "encoder_version": "",
        "ok": False,
        "error": reason,
    }


def _compare_sweep_parent(args: argparse.Namespace) -> Path | None:
    """Resolve the sweep scratch root with CLI-over-environment precedence."""
    from .bisect import _workdir_parent

    parent = getattr(args, "workdir", None) or _workdir_parent()
    if parent is not None:
        parent = Path(parent)
        parent.mkdir(parents=True, exist_ok=True)
    return parent


def _encode_compare_sweep_cell(
    args: argparse.Namespace,
    context: _CompareSweepContext,
    availability: dict[str, tuple[bool, str]],
    codec: str,
    crf: int,
) -> dict[str, Any]:
    """Encode and score one codec/CRF cell."""
    from .bisect import _encode_and_score

    spec = context.by_token[codec]
    available, reason = availability[codec]
    if not available:
        return _unavailable_compare_sweep_row(spec, crf, reason)
    with tempfile.TemporaryDirectory(
        prefix="vmaf-tune-crf-sweep-", dir=_compare_sweep_parent(args)
    ) as workdir:
        result = _encode_and_score(
            src=Path(args.src),
            codec=spec.adapter,
            adapter=get_adapter(spec.adapter),
            preset=args.preset,
            crf=crf,
            width=context.width,
            height=context.height,
            pix_fmt=args.pix_fmt,
            framerate=context.framerate,
            duration_s=context.duration_s,
            sample_clip_seconds=getattr(args, "sample_clip_seconds", 0.0),
            vmaf_model=_resolve_vmaf_model(args),
            score_backend=context.score_backend,
            ffmpeg_bin=spec.ffmpeg_bin,
            vmaf_bin=args.vmaf_bin,
            workdir=Path(workdir),
        )
    return {
        "codec": codec,
        "adapter": spec.adapter,
        "runtime_variant": spec.variant,
        "ffmpeg_bin": spec.ffmpeg_bin,
        "crf": crf,
        "bitrate_kbps": result.bitrate_kbps,
        "vmaf_score": result.measured_vmaf,
        "encode_time_ms": result.encode_time_ms,
        "encoder_version": result.encoder_version,
        "ok": result.ok,
        "error": result.error,
    }


def _run_compare_sweep_cells(
    args: argparse.Namespace,
    cells: list[tuple[str, int]],
    encode_one: Callable[[str, int], dict[str, Any]],
) -> list[dict[str, Any]]:
    """Run sweep cells serially or in parallel while retaining input order."""
    from concurrent.futures import ThreadPoolExecutor, as_completed

    if getattr(args, "no_parallel", False) or len(cells) == 1:
        return [encode_one(codec, crf) for codec, crf in cells]
    worker_limit = getattr(args, "max_workers", None)
    worker_count = worker_limit if worker_limit is not None else len(cells)
    with ThreadPoolExecutor(max_workers=worker_count) as pool:
        futures = {pool.submit(encode_one, codec, crf): (codec, crf) for codec, crf in cells}
        ordered = {futures[future]: future.result() for future in as_completed(futures)}
    return [ordered[cell] for cell in cells]


def _clean_compare_sweep_rows(
    rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Replace non-finite floats with JSON null values."""

    def _clean(value: object) -> object:
        if isinstance(value, float) and not math.isfinite(value):
            return None
        return value

    return [
        {key: _clean(value) if isinstance(value, float) else value for key, value in row.items()}
        for row in rows
    ]


def _installed_vmaftune_version() -> str:
    """Return installed package metadata or the documented fallback."""
    import importlib.metadata

    try:
        return importlib.metadata.version("vmaf-tune")
    except importlib.metadata.PackageNotFoundError:
        return "unknown"


def _emit_compare_sweep_payload(args: argparse.Namespace, payload: dict[str, Any]) -> None:
    """Emit one CRF-sweep JSON payload."""
    rendered = json.dumps(payload, indent=2)
    if args.output is None:
        sys.stdout.write(rendered)
        if not rendered.endswith("\n"):
            sys.stdout.write("\n")
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered + "\n", encoding="utf-8")
    sys.stderr.write(f"wrote compare crf-sweep report -> {args.output}\n")


def _run_compare_crf_sweep(
    args: argparse.Namespace,
    encoders: list[str],
    *,
    runtime_specs: tuple[Any, ...] | list[Any] | None = None,
) -> int:
    """Encode each requested codec/CRF pair exactly once."""
    import time

    try:
        crf_values = _parse_compare_crf_values(args)
        context = _resolve_compare_sweep_context(args, encoders, runtime_specs)
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune compare --no-bisect: {exc}\n")
        return 2
    availability = _compare_sweep_availability(context)
    encode_one = lambda codec, crf: _encode_compare_sweep_cell(
        args, context, availability, codec, crf
    )
    cells = [(codec, crf) for codec in encoders for crf in crf_values]
    started = time.monotonic()
    rows = _run_compare_sweep_cells(args, cells, encode_one)
    payload = {
        "schema_version": 3,
        "mode": "crf_sweep",
        "src": str(Path(args.src)),
        "crf_sweep": crf_values,
        "target_vmaf": float(args.target_vmaf),
        "tool_version": _installed_vmaftune_version(),
        "wall_time_ms": round((time.monotonic() - started) * 1000.0, 1),
        "rows": _clean_compare_sweep_rows(rows),
    }
    _emit_compare_sweep_payload(args, payload)
    return 0


def _run_benchmark(args: argparse.Namespace) -> int:
    """Phase G — cross-codec report from an existing corpus JSONL."""
    from .benchmark import render_benchmark, summarize_benchmark
    from .recommend import load_corpus_jsonl

    corpus_path: Path = args.from_corpus
    if not corpus_path.exists():
        sys.stderr.write(f"vmaf-tune benchmark: corpus file not found: {corpus_path}\n")
        return 2
    try:
        summaries = summarize_benchmark(
            load_corpus_jsonl(corpus_path),
            target_vmaf=args.target_vmaf,
            baseline_encoder=args.baseline_encoder,
        )
        rendered = render_benchmark(summaries, fmt=args.format)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        sys.stderr.write(f"vmaf-tune benchmark: {exc}\n")
        return 2

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
        sys.stderr.write(f"wrote benchmark report -> {args.output}\n")
    else:
        sys.stdout.write(rendered)
    return 0


def _load_compare_predicate(spec: str):
    """Load ``MODULE:CALLABLE`` for ``vmaf-tune compare``."""
    if ":" not in spec:
        raise ValueError("expected MODULE:CALLABLE")
    module_name, attr_name = spec.split(":", 1)
    if not module_name or not attr_name:
        raise ValueError("expected MODULE:CALLABLE")
    module = importlib.import_module(module_name)
    predicate = getattr(module, attr_name)
    if not callable(predicate):
        raise ValueError(f"{spec!r} is not callable")
    return predicate


def _load_per_shot_predicate(spec: str) -> PerShotPredicateFn:
    """Load ``MODULE:CALLABLE`` for ``vmaf-tune tune-per-shot``."""
    if ":" not in spec:
        raise ValueError("expected MODULE:CALLABLE")
    module_name, attr_name = spec.split(":", 1)
    if not module_name or not attr_name:
        raise ValueError("expected MODULE:CALLABLE")
    module = importlib.import_module(module_name)
    predicate = getattr(module, attr_name)
    if not callable(predicate):
        raise ValueError(f"{spec!r} is not callable")
    return predicate


def _run_auto(args: argparse.Namespace) -> int:
    """Plan an automatic tune and optionally execute its selected cells."""
    from .auto import emit_plan_json, run_auto

    allow = tuple(token.strip() for token in args.allow_codecs.split(",") if token.strip())
    if not allow:
        sys.stderr.write("vmaf-tune auto: --allow-codecs is empty\n")
        return 2
    try:
        plan = run_auto(
            src=args.src,
            target_vmaf=args.target_vmaf,
            max_budget_kbps=args.max_budget_bitrate,
            allow_codecs=allow,
            user_pinned_codec=args.codec,
            sample_clip_seconds=args.sample_clip_seconds,
            smoke=args.smoke,
        )
    except NotImplementedError as exc:
        sys.stderr.write(f"vmaf-tune auto: {exc}\n")
        return 2
    rendered = emit_plan_json(plan)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
        sys.stderr.write(f"wrote auto plan -> {args.output}\n")
    else:
        sys.stdout.write(rendered)
        if not rendered.endswith("\n"):
            sys.stdout.write("\n")

    execute = getattr(args, "execute", False)
    if execute:
        from .executor import run_plan

        runs_dir: Path = getattr(args, "runs_dir", Path("runs"))
        execute_all: bool = getattr(args, "execute_all", False)
        sys.stderr.write(f"vmaf-tune auto: execute mode — runs dir: {runs_dir}\n")
        results = run_plan(
            plan,
            args.src,
            runs_dir,
            execute_all=execute_all,
        )
        n_ok = sum(1 for r in results if r.score is not None and r.score.exit_status == 0)
        sys.stderr.write(
            f"vmaf-tune auto: executed {len(results)} cell(s), "
            f"{n_ok} scored successfully → {runs_dir / 'tune_results.jsonl'}\n"
        )
        if n_ok == 0 and results:
            return 1

    return 0


def _add_fast_source_args(parser: argparse.ArgumentParser) -> None:
    """Add fast-path source and geometry flags."""
    parser.add_argument(
        "--src",
        type=Path,
        default=None,
        help=(
            "source video (raw YUV or any FFmpeg-readable container). "
            "Required for production mode; optional for ``--smoke``."
        ),
    )
    parser.add_argument(
        "--width",
        type=int,
        default=0,
        help="raw-YUV reference width (required when ``--src`` is a raw YUV)",
    )
    parser.add_argument(
        "--height",
        type=int,
        default=0,
        help="raw-YUV reference height (required when ``--src`` is a raw YUV)",
    )
    parser.add_argument("--pix-fmt", default="yuv420p", help="ffmpeg pix_fmt (default yuv420p)")
    parser.add_argument("--framerate", type=float, default=24.0, help="reference framerate")


def _add_fast_target_args(parser: argparse.ArgumentParser) -> None:
    """Add fast-path quality and encoder flags."""
    parser.add_argument(
        "--target-vmaf",
        type=float,
        required=True,
        help="quality target on the standard VMAF [0, 100] scale",
    )
    parser.add_argument(
        "--encoder",
        default="libx264",
        choices=list(known_codecs()),
        help="codec adapter (must be in ENCODER_VOCAB_V2 for production mode)",
    )
    parser.add_argument(
        "--preset",
        default="medium",
        help="encoder preset for the probe + verify encodes (default medium)",
    )


def _add_fast_search_args(parser: argparse.ArgumentParser) -> None:
    """Add fast-path search-budget flags."""
    parser.add_argument(
        "--crf-min",
        type=int,
        default=DEFAULT_CRF_LO,
        help=f"minimum CRF in the TPE search range (default {DEFAULT_CRF_LO})",
    )
    parser.add_argument(
        "--crf-max",
        type=int,
        default=DEFAULT_CRF_HI,
        help=f"maximum CRF in the TPE search range (default {DEFAULT_CRF_HI})",
    )
    parser.add_argument(
        "--n-trials",
        type=int,
        default=None,
        help=(
            f"TPE trial budget. Default: {PROD_N_TRIALS} in production mode, "
            f"{SMOKE_N_TRIALS} in --smoke mode."
        ),
    )
    parser.add_argument(
        "--time-budget-s",
        type=int,
        default=300,
        help=(
            "soft wall-clock cap in seconds for the Optuna TPE loop "
            "(default 300; in-flight trials are allowed to finish)"
        ),
    )
    parser.add_argument(
        "--proxy-tolerance",
        type=float,
        default=DEFAULT_PROXY_TOLERANCE,
        help=(
            "max absolute proxy/verify VMAF gap before the result is flagged "
            f"out-of-distribution (default {DEFAULT_PROXY_TOLERANCE}). When "
            "exceeded the CLI exits non-zero so callers can fall back to "
            "the slow Phase A grid."
        ),
    )
    parser.add_argument(
        "--sample-chunk-seconds",
        type=float,
        default=5.0,
        help=(
            "duration in seconds of the proxy probe-encode slice per TPE trial "
            "(default 5.0). Shorter = faster TPE iterations, longer = more "
            "stable canonical-6 features."
        ),
    )


def _add_fast_runtime_args(parser: argparse.ArgumentParser) -> None:
    """Add fast-path execution and output flags."""
    parser.add_argument(
        "--smoke",
        action="store_true",
        help=(
            "use the deterministic synthetic CRF->VMAF curve; no ffmpeg, no "
            "ONNX, no GPU verify. Intended for CI on hosts without the "
            "[fast] extras."
        ),
    )
    parser.add_argument(
        "--score-backend",
        default="auto",
        choices=("auto", *ALL_BACKENDS),
        help=(
            "libvmaf scoring backend for the verify pass (default: auto; "
            "cuda > sycl > hip > cpu). See ``vmaf-tune corpus --help``."
        ),
    )
    parser.add_argument(
        "--ffmpeg-bin",
        default="ffmpeg",
        help="path to the ffmpeg binary (default ffmpeg on PATH)",
    )
    parser.add_argument(
        "--vmaf-bin",
        default="vmaf",
        help="path to the libvmaf CLI binary (default vmaf on PATH)",
    )
    parser.add_argument(
        "--vmaf-model",
        default=DEFAULT_MODEL,
        help="vmaf model version string (default: the fork default model)",
    )
    parser.add_argument(
        "--encode-dir",
        type=Path,
        default=Path(".workingdir/cache/vmafx-tune/fast"),
        help="scratch dir for probe + verify encodes (default .workingdir/cache/vmafx-tune/fast, gitignored)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="JSON destination for the recommendation payload (default: stdout)",
    )


def _add_fast_args(parser: argparse.ArgumentParser) -> None:
    """Wire the fast recommendation surface."""
    _add_fast_source_args(parser)
    _add_fast_target_args(parser)
    _add_fast_search_args(parser)
    _add_fast_runtime_args(parser)


def _cleanup_decoded_distorted(decoded: Path, encoded: Path) -> None:
    """Delete a temporary raw decode owned by the caller."""
    if decoded != encoded and decoded.exists():
        decoded.unlink()


def _encode_fast_sample(
    args: argparse.Namespace, workdir: Path, src: Path, crf: int, encoder: str
) -> tuple[Path, float]:
    """Encode one short fast-path proxy sample."""
    from .encode import EncodeRequest, run_encode

    slot = workdir / f"probe_{encoder}_crf{crf}.mp4"
    request = EncodeRequest(
        source=src,
        width=args.width,
        height=args.height,
        pix_fmt=args.pix_fmt,
        framerate=args.framerate,
        encoder=encoder,
        preset=args.preset,
        crf=crf,
        output=slot,
        sample_clip_seconds=args.sample_chunk_seconds,
        sample_clip_start_s=0.0,
    )
    result = run_encode(request, ffmpeg_bin=args.ffmpeg_bin)
    if result.exit_status != 0 or not slot.exists():
        raise RuntimeError(
            f"fast sample_extractor: encode failed (CRF {crf}, "
            f"encoder {encoder}): {result.stderr_tail[-300:]}"
        )
    kbps = (
        (result.encode_size_bytes * 8.0 / 1000.0) / max(args.sample_chunk_seconds, 1e-3)
        if result.encode_size_bytes > 0
        else 0.0
    )
    return slot, kbps


def _score_fast_sample(
    args: argparse.Namespace, workdir: Path, src: Path, slot: Path
) -> list[float]:
    """Score a proxy sample and return normalized canonical features."""
    from .proxy import normalise_features
    from .score import ScoreRequest, build_vmaf_command, maybe_decode_distorted

    request = ScoreRequest(
        reference=src,
        distorted=slot,
        width=args.width,
        height=args.height,
        pix_fmt=args.pix_fmt,
        model=_resolve_vmaf_model(args),
        duration_s=args.sample_chunk_seconds,
        frame_cnt=int(args.sample_chunk_seconds * args.framerate),
    )
    request, decode_rc = maybe_decode_distorted(
        request, workdir=workdir, ffmpeg_bin=args.ffmpeg_bin
    )
    if decode_rc != 0:
        raise RuntimeError(
            "fast sample_extractor: failed to decode distorted container "
            f"{slot} to raw YUV (rc={decode_rc})"
        )
    try:
        with tempfile.TemporaryDirectory(prefix="fast-score-") as score_tmp:
            json_path = Path(score_tmp) / "vmaf.json"
            command = build_vmaf_command(request, json_path, vmaf_bin=args.vmaf_bin, backend=None)
            completed = subprocess.run(command, capture_output=True, text=True, check=False)
            if completed.returncode != 0:
                raise RuntimeError(
                    "fast sample_extractor: vmaf score failed "
                    f"(rc={completed.returncode}): {completed.stderr[-300:]}"
                )
            if not json_path.exists():
                raise RuntimeError(
                    "fast sample_extractor: vmaf completed with rc=0 but output "
                    f"{json_path} was not created"
                )
            payload = json.loads(json_path.read_text(encoding="utf-8"))
            return normalise_features(_parse_canonical6_means(payload))
    finally:
        _cleanup_decoded_distorted(request.distorted, slot)


def _build_fast_sample_extractor(
    args: argparse.Namespace,
    workdir: Path,
) -> Callable[[Path, int, str], tuple[list[float], float]]:
    """Build the production fast-path sample extractor."""
    workdir.mkdir(parents=True, exist_ok=True)

    def _extract(src: Path, crf: int, encoder: str) -> tuple[list[float], float]:
        slot, observed_kbps = _encode_fast_sample(args, workdir, src, crf, encoder)
        return _score_fast_sample(args, workdir, src, slot), observed_kbps

    return _extract


_CANONICAL_6_KEYS: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)


def _parse_canonical6_means(
    payload: dict,
    *,
    normalise: bool = False,
    model_id: str | None = None,
) -> list[float]:
    """Pull canonical-6 per-feature means from libvmaf JSON output.

    Tries ``pooled_metrics.<feature>.mean`` first (checking both
    ``integer_<feature>`` and bare ``<feature>``), then falls back to
    averaging ``frames[].metrics.<feature>``. Missing features fill 0.0 —
    the fr_regressor_v2 proxy sees a zero feature rather than NaN, which is
    in-distribution for content where libvmaf's model omits a metric.
    """
    pooled = payload.get("pooled_metrics") or {}
    frames = payload.get("frames") or []
    out: list[float] = []
    for key in _CANONICAL_6_KEYS:
        int_key = f"integer_{key}"
        # 1. pooled integer_key
        block = pooled.get(int_key) or {}
        if "mean" in block:
            out.append(float(block["mean"]))
            continue
        # 2. pooled bare key
        block = pooled.get(key) or {}
        if "mean" in block:
            out.append(float(block["mean"]))
            continue
        # 3. per-frame fallback
        vals = [
            float(fr["metrics"][int_key])
            for fr in frames
            if fr.get("metrics") and int_key in fr["metrics"]
        ]
        if not vals:
            vals = [
                float(fr["metrics"][key])
                for fr in frames
                if fr.get("metrics") and key in fr["metrics"]
            ]
        out.append(sum(vals) / len(vals) if vals else 0.0)

    if normalise:
        from .proxy import DEFAULT_PROXY_MODEL_ID, normalise_features

        mid = model_id or DEFAULT_PROXY_MODEL_ID
        return normalise_features(out, model_id=mid)
    return out


def _fast_source_duration(args: argparse.Namespace, src: Path) -> float:
    """Derive exact raw-source duration from file geometry."""
    sample_bytes = 2 if args.pix_fmt.endswith(("10le", "12le", "16le")) else 1
    frame_bytes = (
        args.width * args.height * sample_bytes
        + (args.width // 2) * (args.height // 2) * 2 * sample_bytes
    )
    source_bytes = src.stat().st_size if src.exists() and frame_bytes > 0 else 0
    frames = source_bytes // frame_bytes if source_bytes > 0 else 0
    return frames / max(args.framerate, 1e-3) if frames > 0 else 0.0


def _encode_fast_verify(
    args: argparse.Namespace,
    workdir: Path,
    src: Path,
    encoder: str,
    crf: int,
) -> tuple[Path, float] | None:
    """Run the final real encode and return its bitrate."""
    from .encode import EncodeRequest, run_encode

    slot = workdir / f"verify_{encoder}_crf{crf}.mp4"
    result = run_encode(
        EncodeRequest(
            source=src,
            width=args.width,
            height=args.height,
            pix_fmt=args.pix_fmt,
            framerate=args.framerate,
            encoder=encoder,
            preset=args.preset,
            crf=crf,
            output=slot,
        ),
        ffmpeg_bin=args.ffmpeg_bin,
    )
    if result.exit_status != 0 or not slot.exists():
        return None
    duration = _fast_source_duration(args, src)
    kbps = (
        result.encode_size_bytes * 8.0 / 1000.0 / duration
        if result.encode_size_bytes > 0 and duration > 0.0
        else 0.0
    )
    return slot, kbps


def _score_fast_verify(
    args: argparse.Namespace,
    workdir: Path,
    backend: str,
    src: Path,
    slot: Path,
) -> float:
    """Score the final real encode."""
    from .score import ScoreRequest, maybe_decode_distorted, run_score

    request = ScoreRequest(
        reference=src,
        distorted=slot,
        width=args.width,
        height=args.height,
        pix_fmt=args.pix_fmt,
        model=_resolve_vmaf_model(args),
    )
    request, decode_rc = maybe_decode_distorted(
        request, workdir=workdir, ffmpeg_bin=args.ffmpeg_bin
    )
    try:
        if decode_rc != 0:
            return float("nan")
        result = run_score(
            request,
            vmaf_bin=args.vmaf_bin,
            backend=backend if backend != "cpu" else None,
        )
        return float(result.vmaf_score)
    finally:
        _cleanup_decoded_distorted(request.distorted, slot)


def _build_fast_encode_runner(
    args: argparse.Namespace,
    workdir: Path,
    backend: str,
) -> Callable[[Path, str, int, str], tuple[float, float]]:
    """Build the mandatory production verify-pass callable."""
    workdir.mkdir(parents=True, exist_ok=True)

    def _runner(src: Path, encoder: str, crf: int, _backend_advisory: str) -> tuple[float, float]:
        encoded = _encode_fast_verify(args, workdir, src, encoder, crf)
        if encoded is None:
            return 0.0, float("nan")
        slot, observed_kbps = encoded
        return observed_kbps, _score_fast_verify(args, workdir, backend, src, slot)

    return _runner


def _prepare_fast_runtime(
    args: argparse.Namespace,
) -> tuple[Callable | None, Callable | None, str | None]:
    """Validate production inputs and construct live fast-path callables."""
    if args.crf_min < 0 or args.crf_max < args.crf_min:
        raise ValueError(f"invalid CRF range [{args.crf_min}, {args.crf_max}]")
    if args.smoke:
        return None, None, None
    if args.src is None:
        raise ValueError("--src is required in production mode")
    if args.width <= 0 or args.height <= 0:
        raise ValueError("--width / --height are required in production mode " "(raw-YUV geometry)")
    backend = select_backend(prefer=args.score_backend, vmaf_bin=args.vmaf_bin)
    sys.stderr.write(f"vmaf-tune fast: scoring backend = {backend}\n")
    workdir = args.encode_dir
    return (
        _build_fast_sample_extractor(args, workdir / "probes"),
        _build_fast_encode_runner(args, workdir / "verify", backend),
        backend,
    )


def _emit_fast_result(args: argparse.Namespace, result: dict[str, Any]) -> None:
    """Emit one fast recommendation payload."""
    rendered = json.dumps(result, indent=2, sort_keys=True)
    if args.output is None:
        sys.stdout.write(rendered + "\n")
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered + "\n", encoding="utf-8")
    sys.stderr.write(f"wrote fast recommendation -> {args.output}\n")


def _run_fast(args: argparse.Namespace) -> int:
    """Drive fast recommendation and mandatory production verification."""
    try:
        sample_extractor, encode_runner, backend = _prepare_fast_runtime(args)
        result = fast_recommend(
            src=args.src,
            target_vmaf=args.target_vmaf,
            encoder=args.encoder,
            time_budget_s=args.time_budget_s,
            crf_range=(args.crf_min, args.crf_max),
            n_trials=args.n_trials,
            smoke=args.smoke,
            sample_extractor=sample_extractor,
            encode_runner=encode_runner,
            proxy_tolerance=args.proxy_tolerance,
        )
    except (BackendUnavailableError, RuntimeError, ValueError) as exc:
        sys.stderr.write(f"vmaf-tune fast: {exc}\n")
        return 2
    if backend is not None:
        result["score_backend"] = backend
    _emit_fast_result(args, result)
    gap = result.get("proxy_verify_gap")
    return 3 if gap is not None and gap > args.proxy_tolerance else 0


def _add_prefilter_source_args(parser: argparse.ArgumentParser) -> None:
    """Add prefilter source and geometry flags."""
    parser.add_argument(
        "--src",
        type=Path,
        default=None,
        help=(
            "source video (raw YUV or any FFmpeg-readable container). "
            "Required for the live loop; optional for ``--smoke``."
        ),
    )
    parser.add_argument(
        "--width",
        type=int,
        default=0,
        help="raw-YUV reference width (required for the live loop on raw YUV)",
    )
    parser.add_argument(
        "--height",
        type=int,
        default=0,
        help="raw-YUV reference height (required for the live loop on raw YUV)",
    )
    parser.add_argument("--pix-fmt", default="yuv420p", help="ffmpeg pix_fmt (default yuv420p)")
    parser.add_argument("--framerate", type=float, default=24.0, help="reference framerate")
    parser.add_argument(
        "--duration",
        dest="duration_s",
        type=float,
        default=0.0,
        help=(
            "clip duration in seconds; used to report achieved kbps and to "
            "weight the bitrate term in the objective. When 0 (default), the "
            "search optimises VMAF only and the reported bitrate is 0 "
            "(bitrate is undefined without a duration)."
        ),
    )


def _add_prefilter_target_args(parser: argparse.ArgumentParser) -> None:
    """Add prefilter quality, encoder, and filter flags."""
    parser.add_argument(
        "--target-vmaf",
        type=float,
        required=True,
        help="quality target on the standard VMAF [0, 100] scale",
    )
    parser.add_argument(
        "--encoder",
        default="libx264",
        choices=list(known_codecs()),
        help="HW/SW codec adapter that performs the post-deband encode (default libx264)",
    )
    parser.add_argument(
        "--preset", default="medium", help="encoder preset for the probe encodes (default medium)"
    )
    parser.add_argument(
        "--filter",
        dest="filter_name",
        default="pelorus_deband",
        choices=list(known_filters()),
        help="pre-encode filter adapter to autotune (default pelorus_deband)",
    )
    parser.add_argument(
        "--sweep-knob",
        action="append",
        default=None,
        dest="sweep_knobs",
        metavar="KNOB",
        help=(
            "restrict the deband search to this knob (repeatable). Omit to "
            "sweep all 10 contract knobs. Valid knobs: range, thry, thrc, "
            "grainy, grainc, softness, detail, dither, dynamic, protect."
        ),
    )


def _add_prefilter_search_args(parser: argparse.ArgumentParser) -> None:
    """Add prefilter search-budget flags."""
    parser.add_argument(
        "--crf-min",
        type=int,
        default=PREFILTER_CRF_LO,
        help=f"minimum CRF in the joint TPE search range (default {PREFILTER_CRF_LO})",
    )
    parser.add_argument(
        "--crf-max",
        type=int,
        default=PREFILTER_CRF_HI,
        help=f"maximum CRF in the joint TPE search range (default {PREFILTER_CRF_HI})",
    )
    parser.add_argument(
        "--n-trials",
        type=int,
        default=None,
        help=(
            f"TPE trial budget. Default: {PREFILTER_N_TRIALS} in the live loop, "
            f"{PREFILTER_SMOKE_N_TRIALS} in --smoke mode."
        ),
    )
    parser.add_argument(
        "--time-budget-s",
        type=float,
        default=600.0,
        help="soft wall-clock cap in seconds for the Optuna TPE loop (default 600)",
    )
    parser.add_argument(
        "--seed", type=int, default=0, help="TPE sampler seed for a reproducible search (default 0)"
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help=(
            "use the synthetic deband+CRF surface; no ffmpeg, no Vulkan, no "
            "GPU. Intended for CI on hosts without a pelorus-enabled ffmpeg."
        ),
    )


def _add_prefilter_runtime_args(parser: argparse.ArgumentParser) -> None:
    """Add prefilter backend and output flags."""
    parser.add_argument(
        "--score-backend",
        default="auto",
        choices=("auto", *ALL_BACKENDS),
        help=(
            "libvmaf scoring backend for the probe scores (default: auto; "
            "cuda > sycl > hip > cpu). vmafx scores the deband output; the "
            "deband filter itself runs in ffmpeg, not in vmafx."
        ),
    )
    parser.add_argument(
        "--ffmpeg-bin", default="ffmpeg", help="ffmpeg binary (default ffmpeg on PATH)"
    )
    parser.add_argument(
        "--vmaf-bin", default="vmaf", help="libvmaf CLI binary (default vmaf on PATH)"
    )
    parser.add_argument(
        "--vmaf-model",
        default=DEFAULT_MODEL,
        help="vmaf model version string (default: the fork default model)",
    )
    _add_neg_flag(parser)
    parser.add_argument(
        "--encode-dir",
        type=Path,
        default=Path(".workingdir/cache/vmafx-tune/prefilter"),
        help="scratch dir for probe encodes (default .workingdir/cache/vmafx-tune/prefilter, gitignored)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="JSON destination for the recommendation payload (default: stdout)",
    )


def _add_prefilter_args(parser: argparse.ArgumentParser) -> None:
    """Wire the joint prefilter recommendation surface."""
    _add_prefilter_source_args(parser)
    _add_prefilter_target_args(parser)
    _add_prefilter_search_args(parser)
    _add_prefilter_runtime_args(parser)


def _run_prefilter_probe(
    args: argparse.Namespace,
    workdir: Path,
    backend: str,
    is_container: bool,
    deband: Mapping[str, float],
    crf: int,
) -> ProbeResult:
    """Encode and score one live prefilter candidate."""
    from .encode import EncodeRequest, bitrate_kbps, run_encode
    from .score import ScoreRequest, run_score

    fragment = get_filter_adapter(args.filter_name).vf_fragment(deband)
    slot = workdir / f"probe_crf{crf}_{abs(hash(fragment)) & 0xFFFFFF:06x}.mp4"
    result = run_encode(
        EncodeRequest(
            source=args.src,
            width=args.width,
            height=args.height,
            pix_fmt=args.pix_fmt,
            framerate=args.framerate,
            duration_s=args.duration_s,
            encoder=args.encoder,
            preset=args.preset,
            crf=crf,
            output=slot,
            extra_params=("-vf", fragment),
            source_is_container=is_container,
        ),
        ffmpeg_bin=args.ffmpeg_bin,
    )
    if result.exit_status != 0 or not slot.exists():
        return ProbeResult(vmaf=0.0, kbps=0.0, vf_fragment=fragment)
    kbps = (
        bitrate_kbps(result.encode_size_bytes, args.duration_s)
        if result.encode_size_bytes > 0
        else 0.0
    )
    score = run_score(
        ScoreRequest(
            reference=args.src,
            distorted=slot,
            width=args.width,
            height=args.height,
            pix_fmt=args.pix_fmt,
            model=_resolve_vmaf_model(args),
        ),
        vmaf_bin=args.vmaf_bin,
        backend=backend if backend != "cpu" else None,
    )
    return ProbeResult(
        vmaf=float(score.vmaf_score),
        kbps=float(kbps),
        vf_fragment=fragment,
    )


def _build_prefilter_probe(
    args: argparse.Namespace,
    workdir: Path,
    backend: str,
) -> Callable[[Mapping[str, float], int], ProbeResult]:
    """Build the live prefilter probe callable."""
    workdir.mkdir(parents=True, exist_ok=True)
    is_container = args.src.suffix.lower() not in {".yuv", ".raw", ".y4m", ""}

    def _probe(deband: Mapping[str, float], crf: int) -> ProbeResult:
        return _run_prefilter_probe(args, workdir, backend, is_container, deband, crf)

    return _probe


def _prepare_prefilter_probe(
    args: argparse.Namespace,
) -> Callable[[Mapping[str, float], int], ProbeResult] | None:
    """Validate live-loop inputs and build its probe."""
    if args.crf_min < 0 or args.crf_max < args.crf_min:
        raise ValueError(f"invalid CRF range [{args.crf_min}, {args.crf_max}]")
    if args.smoke:
        return None
    if args.src is None:
        raise ValueError("--src is required for the live loop")
    if args.width <= 0 or args.height <= 0:
        raise ValueError("--width / --height are required for the live loop " "(raw-YUV geometry)")
    if not pelorus_filter_available(args.ffmpeg_bin):
        raise ValueError(
            "the Pelorus deband filter ('pelorus_deband_vulkan') is not "
            f"available in this ffmpeg build ({args.ffmpeg_bin}). Build ffmpeg "
            "with the Pelorus Vulkan filter, or use --smoke to exercise the "
            "search loop without a live encode. (ADR-1116 / pelorus ADR-0110)"
        )
    backend = select_backend(prefer=args.score_backend, vmaf_bin=args.vmaf_bin)
    sys.stderr.write(f"vmaf-tune prefilter: scoring backend = {backend}\n")
    return _build_prefilter_probe(args, args.encode_dir / "probes", backend)


def _emit_prefilter_result(args: argparse.Namespace, result: dict[str, Any]) -> None:
    """Emit one prefilter recommendation payload."""
    rendered = json.dumps(result, indent=2, sort_keys=True)
    if args.output is None:
        sys.stdout.write(rendered + "\n")
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered + "\n", encoding="utf-8")
    sys.stderr.write(f"wrote prefilter recommendation -> {args.output}\n")


def _run_prefilter(args: argparse.Namespace) -> int:
    """Drive the joint prefilter recommendation loop."""
    try:
        probe = _prepare_prefilter_probe(args)
        result = recommend_prefilter(
            src=args.src,
            target_vmaf=args.target_vmaf,
            encoder=args.encoder,
            filter_name=args.filter_name,
            crf_range=(args.crf_min, args.crf_max),
            sweep_knobs=tuple(args.sweep_knobs) if args.sweep_knobs else None,
            n_trials=args.n_trials,
            time_budget_s=args.time_budget_s,
            smoke=args.smoke,
            probe=probe,
            seed=args.seed,
        )
    except (
        BackendUnavailableError,
        RuntimeError,
        ValueError,
        KeyError,
        PelorusFilterUnavailableError,
    ) as exc:
        sys.stderr.write(f"vmaf-tune prefilter: {exc}\n")
        return 2
    _emit_prefilter_result(args, result)
    return 0


_SIDECAR_REQUIRED_FEATURE_KEYS: tuple[str, ...] = (
    "probe_bitrate_kbps",
    "probe_i_frame_avg_bytes",
    "probe_p_frame_avg_bytes",
    "probe_b_frame_avg_bytes",
)


def _read_json_object(path: Path) -> dict[str, object]:
    """Read a JSON object from ``path`` or raise ``ValueError``."""
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path} is not valid JSON: {exc}") from exc
    if not isinstance(doc, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return doc


def _sidecar_features_from_mapping(row: dict[str, object]):
    """Build ``ShotFeatures`` from a JSON object or a ``features`` wrapper."""
    from .predictor import ShotFeatures

    raw = row.get("features", row)
    if not isinstance(raw, dict):
        raise ValueError("'features' must be a JSON object")
    missing = [key for key in _SIDECAR_REQUIRED_FEATURE_KEYS if key not in raw]
    if missing:
        raise ValueError(f"features missing required keys: {', '.join(missing)}")

    kwargs: dict[str, object] = {}
    for field in dataclasses.fields(ShotFeatures):
        if field.name in raw:
            kwargs[field.name] = raw[field.name]
    try:
        return ShotFeatures(
            probe_bitrate_kbps=float(kwargs["probe_bitrate_kbps"]),
            probe_i_frame_avg_bytes=float(kwargs["probe_i_frame_avg_bytes"]),
            probe_p_frame_avg_bytes=float(kwargs["probe_p_frame_avg_bytes"]),
            probe_b_frame_avg_bytes=float(kwargs["probe_b_frame_avg_bytes"]),
            saliency_mean=float(kwargs.get("saliency_mean", 0.0)),
            saliency_var=float(kwargs.get("saliency_var", 0.0)),
            frame_diff_mean=float(kwargs.get("frame_diff_mean", 0.0)),
            y_avg=float(kwargs.get("y_avg", 0.0)),
            y_var=float(kwargs.get("y_var", 0.0)),
            shot_length_frames=int(kwargs.get("shot_length_frames", 0)),
            fps=float(kwargs.get("fps", 0.0)),
            width=int(kwargs.get("width", 0)),
            height=int(kwargs.get("height", 0)),
        )
    except (TypeError, ValueError, KeyError) as exc:
        raise ValueError(f"invalid sidecar feature value: {exc}") from exc


def _build_sidecar_predictor(args: argparse.Namespace):
    """Construct the configured ``SidecarPredictor`` for CLI handlers."""
    from .predictor import Predictor
    from .sidecar import SidecarConfig, SidecarPredictor

    cfg_kwargs: dict[str, object] = {
        "predictor_version": args.predictor_version,
    }
    if args.cache_dir is not None:
        cfg_kwargs["cache_dir"] = args.cache_dir
    cfg = SidecarConfig(**cfg_kwargs)
    predictor = Predictor(model_path=args.model)
    if predictor.is_stub:
        print(
            f"warning: predictor model '{args.model}' is a synthetic stub "
            "(not authoritative for production CRF picks)",
            file=sys.stderr,
        )
    return SidecarPredictor.for_codec(predictor, codec=args.codec, config=cfg)


def _sidecar_status_payload(sp) -> dict[str, object]:
    """Return the machine-readable status payload for a sidecar."""
    return {
        "schema": "vmaf-tune-sidecar-status/v1",
        "codec": sp.codec,
        "host_uuid": sp.host_uuid,
        "state_path": str(sp.state_path),
        "predictor_version": sp.model.config.predictor_version,
        "schema_version": sp.model.to_dict()["schema_version"],
        "n_updates": sp.model.n_updates,
        "recent_residual_rms": sp.model.recent_residual_rms,
    }


def _emit_sidecar_status(payload: dict[str, object], as_json: bool) -> None:
    """Write a sidecar status payload to stdout."""
    if as_json:
        sys.stdout.write(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        return
    sys.stdout.write(
        "codec={codec} predictor_version={predictor_version} "
        "updates={n_updates} residual_rms={recent_residual_rms:.6f} "
        "state={state_path}\n".format(**payload)
    )


def _load_sidecar_cli_features(args: argparse.Namespace, command: str) -> Any | None:
    """Load feature JSON and report a command-scoped user error."""
    try:
        return _sidecar_features_from_mapping(_read_json_object(args.features_json))
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune sidecar {command}: {exc}\n")
        return None


def _run_sidecar_predict(args: argparse.Namespace, sidecar: Any) -> int:
    """Run one sidecar prediction."""
    features = _load_sidecar_cli_features(args, "predict")
    if features is None:
        return 2
    base = sidecar.predictor.predict_vmaf(features, args.crf, args.codec)
    payload = {
        "schema": "vmaf-tune-sidecar-predict/v1",
        "codec": args.codec,
        "crf": args.crf,
        "base_vmaf": base,
        "correction": sidecar.model.predict_correction(features, args.crf),
        "sidecar_vmaf": sidecar.predict_vmaf(features, args.crf),
        "n_updates": sidecar.model.n_updates,
    }
    if args.json:
        sys.stdout.write(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    else:
        sys.stdout.write(
            "base={base_vmaf:.6f} correction={correction:.6f} "
            "sidecar={sidecar_vmaf:.6f} updates={n_updates}\n".format(**payload)
        )
    return 0


def _run_sidecar_record(args: argparse.Namespace, sidecar: Any) -> int:
    """Record one observed sidecar capture."""
    features = _load_sidecar_cli_features(args, "record")
    if features is None:
        return 2
    base = sidecar.predictor.predict_vmaf(features, args.crf, args.codec)
    sidecar.record_capture(
        features,
        crf=args.crf,
        observed_vmaf=args.observed_vmaf,
        persist=not args.no_persist,
    )
    payload = _sidecar_status_payload(sidecar)
    payload.update(
        {
            "schema": "vmaf-tune-sidecar-record/v1",
            "crf": args.crf,
            "observed_vmaf": args.observed_vmaf,
            "base_vmaf": base,
            "residual": args.observed_vmaf - base,
        }
    )
    if args.json:
        sys.stdout.write(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    else:
        sys.stdout.write(
            "recorded updates={n_updates} residual={residual:.6f} "
            "state={state_path}\n".format(**payload)
        )
    return 0


def _parse_sidecar_capture(line: str) -> tuple[Any, int, float]:
    """Parse one batch-record JSONL row."""
    row = json.loads(line)
    if not isinstance(row, dict):
        raise ValueError("row is not an object")
    return (
        _sidecar_features_from_mapping(row),
        int(row["crf"]),
        float(row["observed_vmaf"]),
    )


def _read_sidecar_captures(args: argparse.Namespace, sidecar: Any) -> tuple[int, int]:
    """Read and apply valid batch-record rows, warning on each invalid row."""
    recorded = 0
    skipped = 0
    with args.captures_jsonl.open(encoding="utf-8") as handle:
        for lineno, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line:
                continue
            try:
                features, crf, observed = _parse_sidecar_capture(line)
            except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
                skipped += 1
                sys.stderr.write(f"vmaf-tune sidecar batch-record: skip line {lineno}: {exc}\n")
                continue
            sidecar.record_capture(features, crf=crf, observed_vmaf=observed, persist=False)
            recorded += 1
    return recorded, skipped


def _run_sidecar_batch_record(args: argparse.Namespace, sidecar: Any) -> int:
    """Apply a JSONL batch to one sidecar."""
    try:
        recorded, skipped = _read_sidecar_captures(args, sidecar)
    except OSError as exc:
        sys.stderr.write(f"vmaf-tune sidecar batch-record: cannot read input: {exc}\n")
        return 2
    if recorded:
        sidecar.save()
    payload = _sidecar_status_payload(sidecar)
    payload.update(
        {
            "schema": "vmaf-tune-sidecar-batch-record/v1",
            "rows_recorded": recorded,
            "rows_skipped": skipped,
        }
    )
    if args.json:
        sys.stdout.write(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    else:
        sys.stdout.write(
            "recorded={rows_recorded} skipped={rows_skipped} "
            "updates={n_updates} state={state_path}\n".format(**payload)
        )
    return 0


def _run_sidecar(args: argparse.Namespace) -> int:
    """Run the vmaf-tune sidecar operator surface."""
    try:
        sidecar = _build_sidecar_predictor(args)
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        sys.stderr.write(f"vmaf-tune sidecar: {exc}\n")
        return 2
    if args.sidecar_cmd == "status":
        _emit_sidecar_status(_sidecar_status_payload(sidecar), args.json)
        return 0
    handlers = {
        "predict": _run_sidecar_predict,
        "record": _run_sidecar_record,
        "batch-record": _run_sidecar_batch_record,
    }
    handler = handlers.get(args.sidecar_cmd)
    if handler is None:
        sys.stderr.write(f"vmaf-tune sidecar: unknown subcommand {args.sidecar_cmd!r}\n")
        return 2
    return handler(args, sidecar)


def _coerce_finite_float(value: Any, default: float = math.nan) -> float:
    """Parse a JSON numeric field into a finite float or ``NaN``.

    The compare-report JSON output uses ``null`` for failed-row
    numerics (Bug #2 fix). Round-tripping through ``float(x or 0.0)``
    silently coerces ``None`` to ``0.0`` and leaves NaN as NaN — both
    masquerade as legitimate values in the rendered profile card
    (Bug #6, BBB e2e 2026-05-17). This helper returns ``NaN`` for
    ``None`` / missing / non-finite inputs so the renderer can apply
    its em-dash placeholder.
    """
    if value is None:
        return default
    try:
        v = float(value)
    except (TypeError, ValueError):
        return default
    if math.isnan(v) or math.isinf(v):
        return default
    return v


def _sweep_point_from_json(r: dict[str, Any]) -> CodecSweepPoint:
    """Build a :class:`vmaftune.report.CodecSweepPoint` from a v2 row.

    The compare-sweep JSON row carries ``target_vmaf`` as a top-level
    field (set by :meth:`vmaftune.compare.RecommendResult.to_row`); the
    sweep ingester treats that as authoritative because the schema-v2
    contract is "one row per (codec, target_vmaf) pair" (ADR-0513).
    Missing / non-finite numerics map to ``NaN`` so the chart renderer
    drops them rather than drawing a broken segment.

    ``bisect_samples`` (ADR-0530, optional) is read when present and
    populated; absent or empty means an old v2 dump pre-dating the
    bisect-samples plumb, which renders via the legacy connect-the-
    dots path with a caveat note.
    """
    from .report import BisectSamplePoint, CodecSweepPoint

    ok = bool(r.get("ok", True))
    raw_samples = r.get("bisect_samples") or ()
    samples: tuple[BisectSamplePoint, ...] = ()
    if isinstance(raw_samples, (list, tuple)):
        parsed: list[BisectSamplePoint] = []
        for s in raw_samples:
            if not isinstance(s, dict):
                continue
            try:
                parsed.append(
                    BisectSamplePoint(
                        crf=int(s.get("crf") if s.get("crf") is not None else -1),
                        bitrate_kbps=_coerce_finite_float(s.get("bitrate_kbps")),
                        vmaf_score=_coerce_finite_float(s.get("vmaf_score")),
                        encode_time_ms=_coerce_finite_float(s.get("encode_time_ms")),
                    )
                )
            except (TypeError, ValueError):
                # Skip malformed sample entries — never crash the renderer.
                continue
        samples = tuple(parsed)
    return CodecSweepPoint(
        codec=str(r.get("codec", "")),
        encoder_version=str(r.get("encoder_version", "")),
        target_vmaf=float(r.get("target_vmaf") or 0.0),
        best_crf=int(r.get("best_crf") if r.get("best_crf") is not None else -1),
        bitrate_kbps=_coerce_finite_float(r.get("bitrate_kbps")),
        encode_time_ms=_coerce_finite_float(r.get("encode_time_ms")),
        vmaf_score=_coerce_finite_float(r.get("vmaf_score")),
        ok=ok,
        error=str(r.get("error", "")),
        bisect_samples=samples,
    )


def _codec_row_from_json(r: dict[str, Any]) -> CodecRow:
    """Build a :class:`vmaftune.report.CodecRow` from a compare JSON row.

    Coerces ``null`` / NaN numerics to ``NaN`` (which the renderer
    formats as an em-dash) and falls back to the row-level ``ok``
    flag instead of treating missing ``best_crf`` as 0.
    """
    from .report import CodecRow

    ok = bool(r.get("ok", True))
    return CodecRow(
        codec=str(r.get("codec", "")),
        encoder_version=str(r.get("encoder_version", "")),
        best_crf=int(r.get("best_crf") if r.get("best_crf") is not None else -1),
        bitrate_kbps=_coerce_finite_float(r.get("bitrate_kbps")),
        encode_time_ms=_coerce_finite_float(r.get("encode_time_ms")),
        vmaf_score=_coerce_finite_float(r.get("vmaf_score")),
        ok=ok,
        error=str(r.get("error", "")),
    )


def _read_report_json(path: Path, flag: str) -> dict[str, Any]:
    """Load one report input object with a flag-scoped error."""
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load {flag}: {exc}") from exc
    if not isinstance(payload, dict):
        raise ValueError(f"cannot load {flag}: root must be a JSON object")
    return payload


def _load_compare_report(
    path: Path,
) -> tuple[tuple[Any, ...], tuple[Any, ...], tuple[float, ...]]:
    """Load legacy comparison rows or a v2 sweep."""
    from .compare import detect_schema_version

    payload = _read_report_json(path, "--compare-json")
    rows = payload.get("rows") or payload.get("results") or []
    if detect_schema_version(payload) < 2:
        return tuple(_codec_row_from_json(row) for row in rows), (), ()
    points = tuple(_sweep_point_from_json(row) for row in rows)
    targets = tuple(float(target) for target in payload.get("target_vmafs") or ())
    if not targets:
        targets = tuple(sorted({point.target_vmaf for point in points}))
    return (), points, targets


def _load_ladder_report(path: Path) -> tuple[tuple[Any, ...], tuple[Any, ...]]:
    """Load ladder samples and selected rungs."""
    from .report import LadderRung, LadderSample

    payload = _read_report_json(path, "--ladder-json")
    samples = tuple(
        LadderSample(
            width=int(row.get("width") or 0),
            height=int(row.get("height") or 0),
            bitrate_kbps=(
                float(row["bitrate_kbps"]) if row.get("bitrate_kbps") is not None else float("nan")
            ),
            vmaf=float(row["vmaf"]) if row.get("vmaf") is not None else float("nan"),
            crf=int(row.get("crf") or 0),
        )
        for row in payload.get("samples") or payload.get("points") or []
    )
    rungs = tuple(
        LadderRung(
            width=int(row.get("width") or 0),
            height=int(row.get("height") or 0),
            bitrate_kbps=(
                float(row["bitrate_kbps"]) if row.get("bitrate_kbps") is not None else float("nan")
            ),
            vmaf=float(row["vmaf"]) if row.get("vmaf") is not None else float("nan"),
            crf=int(row.get("crf") or 0),
        )
        for row in payload.get("renditions") or payload.get("rungs") or []
    )
    return samples, rungs


def _shot_row_from_json(
    row: dict[str, Any], index: int, payload: dict[str, Any], source: Any
) -> Any:
    """Convert one per-shot plan row for profile rendering."""
    from .report import ShotRow

    start_frame = int(row.get("start_frame") or row.get("start") or 0)
    end_frame = int(row.get("end_frame") or row.get("end") or 0)
    frame_count = int(row.get("frames") or max(0, end_frame - start_frame))
    fps = float(row.get("framerate") or payload.get("framerate") or source.fps or 0.0)
    duration = frame_count / fps if fps > 0 else 0.0
    return ShotRow(
        shot_index=int(row.get("shot_id", row.get("index", index))),
        start_frame=start_frame,
        end_frame=end_frame,
        width=int(row.get("width") or source.width),
        height=int(row.get("height") or source.height),
        best_crf=int(row.get("best_crf") or row.get("crf") or row.get("predicted_crf") or 0),
        vmaf=float(row.get("vmaf") or row.get("predicted_vmaf") or float("nan")),
        bitrate_kbps=float(row.get("bitrate_kbps") or float("nan")),
        duration_s=float(row.get("duration_s") or duration),
    )


def _load_shot_report(path: Path, source: Any) -> tuple[Any, ...]:
    """Load per-shot rows in either plan schema."""
    payload = _read_report_json(path, "--per-shot-json")
    return tuple(
        _shot_row_from_json(row, index, payload, source)
        for index, row in enumerate(payload.get("shots") or payload.get("plan") or [])
    )


def _build_profile_report_data(
    args: argparse.Namespace,
    source: Any,
    codec_rows: tuple[Any, ...],
    sweep_points: tuple[Any, ...],
    sweep_targets: tuple[float, ...],
    ladder_samples: tuple[Any, ...],
    ladder_rungs: tuple[Any, ...],
    shots: tuple[Any, ...],
) -> Any:
    """Assemble renderer inputs with stable run metadata."""
    from datetime import datetime, timezone

    from .report import ReportData

    return ReportData(
        source=source,
        target_vmaf=float(args.target_vmaf),
        codec_rows=codec_rows,
        sweep_points=sweep_points,
        sweep_targets=sweep_targets,
        ladder_samples=ladder_samples,
        ladder_rungs=ladder_rungs,
        shots=shots,
        generated_at_iso=datetime.now(timezone.utc).isoformat(timespec="seconds"),
        encoder_preset=str(getattr(args, "preset", "") or ""),
        pix_fmt=str(getattr(args, "pix_fmt", "") or ""),
        score_backend=str(getattr(args, "score_backend", "") or ""),
        ffmpeg_bin=str(getattr(args, "ffmpeg_bin", "") or ""),
        vmaf_bin=str(getattr(args, "vmaf_bin", "") or ""),
    )


def _write_profile_report_outputs(args: argparse.Namespace, data: Any) -> list[Path]:
    """Write requested report artifacts."""
    from .report import render_html, render_markdown

    args.output.parent.mkdir(parents=True, exist_ok=True)
    outputs: list[Path] = []
    if args.format == "both" or getattr(args, "json_sidecar", False):
        json_path = args.output.with_suffix(".json")
        json_path.write_text(json.dumps(data.to_dict(), indent=2) + "\n", encoding="utf-8")
        outputs.append(json_path)
    if args.format in ("html", "both"):
        html_path = args.output if args.format == "html" else args.output.with_suffix(".html")
        html_path.write_text(render_html(data), encoding="utf-8")
        outputs.append(html_path)
    if args.format in ("markdown", "both"):
        markdown_path = args.output if args.format == "markdown" else args.output.with_suffix(".md")
        markdown_path.write_text(
            render_markdown(data, assets_dir=args.assets_dir), encoding="utf-8"
        )
        outputs.append(markdown_path)
    return outputs


def _profile_report_status(
    outputs: list[Path],
    codec_rows: tuple[Any, ...],
    sweep_points: tuple[Any, ...],
    ladder_samples: tuple[Any, ...],
    ladder_rungs: tuple[Any, ...],
    shots: tuple[Any, ...],
) -> dict[str, Any]:
    """Aggregate row status without treating unavailable encoders as regressions."""
    unavailable_prefixes = (
        "encoder unavailable",
        "hardware encoder not available",
    )
    aggregate = list(codec_rows) + list(sweep_points)
    failures = [row for row in aggregate if not row.ok]
    unavailable = [row for row in failures if row.error.startswith(unavailable_prefixes)]
    real_failures = [row for row in failures if not row.error.startswith(unavailable_prefixes)]
    any_ok = any(row.ok for row in aggregate) if aggregate else True
    return {
        "ok": bool(any_ok and not real_failures),
        "degraded": bool(unavailable),
        "outputs": [str(path) for path in outputs],
        "codec_rows": len(codec_rows),
        "codec_rows_ok": sum(1 for row in codec_rows if row.ok),
        "codec_rows_failed": sum(1 for row in codec_rows if not row.ok),
        "codec_rows_unavailable": sum(
            1 for row in codec_rows if not row.ok and row.error.startswith(unavailable_prefixes)
        ),
        "sweep_points": len(sweep_points),
        "sweep_points_ok": sum(1 for row in sweep_points if row.ok),
        "sweep_points_failed": sum(1 for row in sweep_points if not row.ok),
        "ladder_samples": len(ladder_samples),
        "ladder_rungs": len(ladder_rungs),
        "shots": len(shots),
    }


def _run_report(args: argparse.Namespace) -> int:
    """Render a vmaf-tune profile card from JSON inputs."""
    from .report import probe_source

    source = probe_source(args.src)
    codec_rows: tuple[Any, ...] = ()
    sweep_points: tuple[Any, ...] = ()
    sweep_targets: tuple[float, ...] = ()
    ladder_samples: tuple[Any, ...] = ()
    ladder_rungs: tuple[Any, ...] = ()
    shots: tuple[Any, ...] = ()
    try:
        if args.compare_json is not None:
            codec_rows, sweep_points, sweep_targets = _load_compare_report(args.compare_json)
        if args.ladder_json is not None:
            ladder_samples, ladder_rungs = _load_ladder_report(args.ladder_json)
        if args.per_shot_json is not None:
            shots = _load_shot_report(args.per_shot_json, source)
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune report: {exc}\n")
        return 2
    data = _build_profile_report_data(
        args,
        source,
        codec_rows,
        sweep_points,
        sweep_targets,
        ladder_samples,
        ladder_rungs,
        shots,
    )
    outputs = _write_profile_report_outputs(args, data)
    status = _profile_report_status(
        outputs, codec_rows, sweep_points, ladder_samples, ladder_rungs, shots
    )
    sys.stdout.write(json.dumps(status) + "\n")
    return 0


def _resolve_encode_profile(args: argparse.Namespace) -> tuple[Any, Any, Any, str]:
    """Load a profile, select a row, and build its encode request."""
    from .encoder_profile import (
        build_encode_request,
        load_profile_payload,
        select_recommendation,
    )

    profile = load_profile_payload(args.profile)
    recommendation = select_recommendation(
        profile,
        codec=args.codec,
        target_vmaf=args.target_vmaf,
        recommendation_index=args.recommendation_index,
    )
    request = build_encode_request(
        profile,
        recommendation,
        output=args.output,
        source_override=args.src,
        preset_override=args.preset,
        pix_fmt_override=args.pix_fmt,
        framerate_override=args.framerate,
        width_override=args.width,
        height_override=args.height,
        duration_override=args.duration,
        source_kind=args.source_kind,
        sample_clip_seconds=args.sample_clip_seconds,
        sample_clip_start_s=args.sample_clip_start_s,
        extra_params=tuple(args.extra_ffmpeg_arg or ()),
    )
    run_metadata = profile.get("run") or {}
    ffmpeg_bin = str(args.ffmpeg_bin or run_metadata.get("ffmpeg_bin") or "ffmpeg")
    return profile, recommendation, request, ffmpeg_bin


def _encode_profile_result_payload(
    args: argparse.Namespace,
    recommendation: Any,
    argv: list[str],
    result: Any,
) -> dict[str, Any]:
    """Build the executed encode-profile result schema."""
    return {
        "ok": result.exit_status == 0,
        "profile": str(args.profile),
        "selected": recommendation,
        "ffmpeg_argv": argv,
        "output": str(args.output),
        "exit_status": result.exit_status,
        "encode_size_bytes": result.encode_size_bytes,
        "encode_time_ms": result.encode_time_ms,
        "encoder_version": result.encoder_version,
        "ffmpeg_version": result.ffmpeg_version,
        "stderr_tail": result.stderr_tail,
    }


def _emit_json_object(payload: dict[str, Any]) -> None:
    """Emit a sorted, indented JSON object."""
    sys.stdout.write(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def _run_encode_profile(args: argparse.Namespace) -> int:
    """Encode one recommendation from an embedded vmaf-tune profile."""
    from .encode import build_ffmpeg_command, run_encode

    try:
        _profile, recommendation, request, ffmpeg_bin = _resolve_encode_profile(args)
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune encode-profile: {exc}\n")
        return 2
    argv = build_ffmpeg_command(request, ffmpeg_bin=ffmpeg_bin)
    if args.dry_run:
        _emit_json_object(
            {
                "ok": True,
                "dry_run": True,
                "profile": str(args.profile),
                "selected": recommendation,
                "ffmpeg_argv": argv,
                "output": str(args.output),
            }
        )
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = run_encode(request, ffmpeg_bin=ffmpeg_bin)
    _emit_json_object(_encode_profile_result_payload(args, recommendation, argv, result))
    return int(result.exit_status)


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    # ADR-0509 / BBB e2e v7: stamp ``_<dest>_was_default = True`` on
    # every ``_TrackedDefaultAction`` flag the user did NOT pass, so
    # ``_run_compare`` can distinguish "argparse default" from "user
    # override" when auto-probing container-source geometry.
    _stamp_tracked_default_sentinels(args)
    if args.cmd == "corpus":
        return _run_corpus(args)
    if args.cmd == "recommend":
        return _run_recommend(args)
    if args.cmd == "predict":
        return _run_predict(args)
    if args.cmd == "tune-per-shot":
        return _run_tune_per_shot(args)
    if args.cmd == "recommend-saliency":
        return _run_recommend_saliency(args)
    if args.cmd == "ladder":
        return _run_ladder(args)
    if args.cmd == "compare":
        return _run_compare(args)
    if args.cmd == "benchmark":
        return _run_benchmark(args)
    if args.cmd == "auto":
        return _run_auto(args)
    if args.cmd == "fast":
        return _run_fast(args)
    if args.cmd == "prefilter":
        return _run_prefilter(args)
    if args.cmd == "sidecar":
        return _run_sidecar(args)
    if args.cmd == "report":
        return _run_report(args)
    if args.cmd == "encode-profile":
        return _run_encode_profile(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
