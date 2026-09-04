# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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
from typing import Any, TextIO

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

    def __init__(self, *args: object, **kwargs: object) -> None:
        super().__init__(*args, **kwargs)  # type: ignore[arg-type]

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

    corpus = sub.add_parser("corpus", help="run the Phase A grid sweep + emit JSONL")
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
        default=Path(".workingdir2/encodes"),
        help="scratch dir for encodes (default .workingdir2/encodes, gitignored)",
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

    # HDR mode (Bucket #9 / ADR-0300). Mutually-exclusive group on the
    # corpus subparser; default ``--auto-hdr`` keeps the SDR path
    # untouched until ffprobe detects PQ / HLG signaling on a source.
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

    recommend = sub.add_parser(
        "recommend",
        help=(
            "find the smallest CRF whose VMAF >= --target-vmaf "
            "(coarse-to-fine, ~3.5x fewer encodes than the full grid)"
        ),
    )
    _add_recommend_args(recommend)

    predict = sub.add_parser(
        "predict",
        help=(
            "Phase C — predict per-shot VMAF without running it. Probes-encode "
            "each shot, runs a learned ONNX predictor (or analytical fallback), "
            "validates against real VMAF on K shots, then emits the verdict."
        ),
    )
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

    per_shot = sub.add_parser(
        "tune-per-shot",
        help=(
            "Phase D — detect shots via vmaf-perShot/TransNet V2, run "
            "Phase-B bisect per shot, and emit an FFmpeg encoding plan."
        ),
    )
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

    rec_sal = sub.add_parser(
        "recommend-saliency",
        help=(
            "saliency-aware ROI encode — biases bits toward salient regions "
            "via the fork-trained ``saliency_student_v1`` ONNX model "
            "(Bucket #2 / ADR-0287)"
        ),
    )
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

    ladder = sub.add_parser(
        "ladder",
        help=(
            "Phase E — build a per-title bitrate ladder (convex-hull "
            "sweep over (resolution × target-VMAF), pick K knees, "
            "emit HLS / DASH / JSON manifest)"
        ),
    )
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
    # Source-shape flags symmetric with ``compare`` / ``tune-per-shot``
    # (Bug #4, BBB e2e 2026-05-17). The default sampler used to hardcode
    # framerate=24.0 / duration_s=1.0 / pix_fmt="yuv420p" — any real-
    # world 1080p30 input therefore ran the corpus sweep at the wrong
    # framerate and computed bitrate against 1 s of encoded output.
    # These flags thread the actual source shape through
    # ``make_default_sampler`` so bitrate math and encode timing match.
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
    # ADR-0498 / Bug #v2-B: explicit source dimensions for raw YUV
    # sources when cross-resolution rungs are requested. When omitted
    # the largest resolution in --resolutions is used as the source
    # geometry (matches the historic single-resolution behaviour).
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

    compare = sub.add_parser(
        "compare",
        help=(
            "compare codec adapters at a target VMAF — runs the "
            "Phase B-lite predicate per encoder, ranks by smallest "
            "bitrate, emits a markdown / JSON / CSV report"
        ),
    )
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
        "--output",
        type=Path,
        default=None,
        help="report destination (default: stdout)",
    )

    auto = sub.add_parser(
        "auto",
        help=(
            "Phase F — adaptive recipe-aware tuning entry point "
            "(ADR-0364). Composes the per-phase subcommands into one "
            "deterministic decision tree with seven short-circuits "
            "and non-smoke source metadata probing."
        ),
    )
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

    fast = sub.add_parser(
        "fast",
        help=(
            "Phase A.5 fast-path — proxy + Bayesian + GPU-verify recommend "
            "(ADR-0276 + ADR-0304). Seconds-to-minutes alternative to the "
            "Phase A grid for the recommendation use case."
        ),
    )
    _add_fast_args(fast)

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

    report = sub.add_parser(
        "report",
        help=(
            "render an HTML/Markdown profile card for a tuned source — "
            "ingests JSON dumps of `compare`/`ladder`/`tune-per-shot` and "
            "emits a self-contained artefact with rate-distortion charts, "
            "ladder rungs, per-shot timeline, and source metadata"
        ),
    )
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
    report.add_argument(
        "--format",
        default="html",
        choices=("html", "markdown", "both"),
        help="report format (default html; `both` emits .html + .md next to --output)",
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

    encode_profile = sub.add_parser(
        "encode-profile",
        help=(
            "read a vmaf-tune report/profile and encode one selected recommendation " "with FFmpeg"
        ),
    )
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

    sidecar = sub.add_parser(
        "sidecar",
        help=(
            "train and inspect the local on-host predictor sidecar "
            "(ADR-0394 bias-correction model)"
        ),
    )
    _add_sidecar_args(sidecar)

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


def _add_recommend_args(p: argparse.ArgumentParser) -> None:
    """Mirror the corpus subparser's source/encode flags for ``recommend``.

    ``recommend`` always runs coarse-to-fine — keeping the flag surface
    aligned with ``corpus`` means downstream scripts can swap one for
    the other without re-learning the CLI.
    """
    # --source / --width / --height / --preset are only required when
    # not using --from-corpus. Validation happens in the handler.
    p.add_argument("--source", type=Path, action="append", default=None)
    p.add_argument("--width", type=int, default=None)
    p.add_argument("--height", type=int, default=None)
    p.add_argument("--pix-fmt", default="yuv420p")
    p.add_argument("--framerate", type=float, default=24.0)
    p.add_argument("--duration", type=float, default=0.0)
    p.add_argument("--encoder", default="libx264", choices=list(known_codecs()))
    p.add_argument("--preset", action="append", default=None)
    p.add_argument(
        "--output",
        type=Path,
        default=Path("corpus.jsonl"),
        help="JSONL destination for the visited points",
    )
    p.add_argument(
        "--encode-dir",
        type=Path,
        default=Path(".workingdir2/encodes"),
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


def _run_recommend_from_corpus(args: argparse.Namespace) -> int:
    """Pick a recommendation from a pre-built corpus JSONL (no new encodes)."""
    import json as _json

    from .recommend import RecommendRequest, recommend

    corpus_path: Path = args.from_corpus
    if not corpus_path.exists():
        sys.stderr.write(f"recommend: corpus file not found: {corpus_path}\n")
        return 2

    rows: list[dict] = []
    with corpus_path.open(encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(_json.loads(line))

    target_vmaf: float | None = getattr(args, "target_vmaf", None)
    target_bitrate: float | None = getattr(args, "target_bitrate", None)

    if target_vmaf is not None and target_bitrate is not None:
        sys.stderr.write("recommend: --target-vmaf and --target-bitrate are mutually exclusive\n")
        return 2

    with_uncertainty: bool = getattr(args, "with_uncertainty", False)

    # --with-uncertainty is only meaningful for --target-vmaf; emit a
    # diagnostic and fall through to the point-estimate path when combined
    # with --target-bitrate (no interval-aware bitrate predicate exists).
    if with_uncertainty and target_bitrate is not None:
        sys.stderr.write(
            "recommend: --with-uncertainty is not supported with --target-bitrate; "
            "falling back to point-estimate\n"
        )
        with_uncertainty = False

    if with_uncertainty and target_vmaf is not None:
        from .recommend import (
            UncertaintyAwareRequest,
            pick_target_vmaf_with_uncertainty,
        )
        from .uncertainty import load_confidence_thresholds

        thresholds = load_confidence_thresholds(getattr(args, "uncertainty_sidecar", None))
        preset_filter = args.preset[0] if args.preset else None
        try:
            ua_result = pick_target_vmaf_with_uncertainty(
                rows,
                UncertaintyAwareRequest(
                    target_vmaf=target_vmaf,
                    thresholds=thresholds,
                    encoder=args.encoder,
                    preset=preset_filter,
                ),
            )
        except ValueError as exc:
            sys.stderr.write(f"recommend: {exc}\n")
            return 2

        json_output: bool = getattr(args, "json_output", False)
        if json_output:
            sys.stdout.write(_json.dumps(ua_result.row) + "\n")
        else:
            row = ua_result.row
            crf = row.get("crf", "?")
            vmaf = row.get("vmaf_score", float("nan"))
            kbps = row.get("bitrate_kbps", float("nan"))
            status = "UNMET" if ua_result.margin < 0 else "OK"
            sys.stdout.write(
                f"crf={crf}  vmaf={vmaf:.3f}  kbps={kbps:.0f}"
                f"  predicate={ua_result.predicate}"
                f"  decision={ua_result.decision.value}"
                f"  visited={ua_result.visited}/{len(rows)}"
                f"  [{status}]\n"
            )
        return 0

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

    json_output = getattr(args, "json_output", False)
    if json_output:
        sys.stdout.write(_json.dumps(pick.row) + "\n")
    else:
        crf = pick.row.get("crf", "?")
        try:
            vmaf = float(pick.row.get("vmaf_score", float("nan")))
        except (TypeError, ValueError):
            vmaf = float("nan")
        try:
            kbps = float(pick.row.get("bitrate_kbps", float("nan")))
        except (TypeError, ValueError):
            kbps = float("nan")
        predicate = pick.predicate
        status = "UNMET" if pick.margin < 0 else "OK"
        sys.stdout.write(
            f"crf={crf}  vmaf={vmaf:.3f}  kbps={kbps:.0f}" f"  predicate={predicate}  [{status}]\n"
        )
    return 0


def _run_recommend(args: argparse.Namespace) -> int:
    if getattr(args, "from_corpus", None) is not None:
        return _run_recommend_from_corpus(args)

    # Encode-driven path: validate required args.
    if not args.source or not args.width or not args.height or not args.preset:
        sys.stderr.write(
            "recommend: --source, --width, --height, --preset are required "
            "unless --from-corpus is used\n"
        )
        return 2

    if args.target_vmaf is None:
        sys.stderr.write("recommend requires --target-vmaf\n")
        return 2

    try:
        opts = _build_opts(args)
    except BackendUnavailableError as exc:
        sys.stderr.write(f"vmaf-tune: {exc}\n")
        return 2
    sentinel_cells = tuple((p, 0) for p in args.preset)

    visited: list[dict] = []

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
    if getattr(args, "with_uncertainty", False):
        from .recommend import (
            UncertaintyAwareRequest,
            pick_target_vmaf_with_uncertainty,
        )
        from .uncertainty import load_confidence_thresholds

        thresholds = load_confidence_thresholds(getattr(args, "uncertainty_sidecar", None))
        try:
            ua_result = pick_target_vmaf_with_uncertainty(
                visited,
                UncertaintyAwareRequest(
                    target_vmaf=args.target_vmaf,
                    thresholds=thresholds,
                ),
            )
        except ValueError as exc:
            sys.stderr.write(
                f"recommend: uncertainty pick failed ({exc}); "
                f"visited {len(visited)} encodes -> {opts.output}\n"
            )
            return 1
        row = ua_result.row
        sys.stdout.write(
            f"src={row.get('src')} preset={row.get('preset')} "
            f"crf={row.get('crf')} vmaf={float(row['vmaf_score']):.3f} "
            f"decision={ua_result.decision.value} "
            f"visited={ua_result.visited}/{len(visited)} "
            f"predicate={ua_result.predicate}\n"
        )
        return 0
    pick = _smallest_passing_crf(visited, args.target_vmaf)
    if pick is None:
        sys.stderr.write(
            f"recommend: no CRF meets target VMAF >= {args.target_vmaf}; "
            f"visited {len(visited)} encodes -> {opts.output}\n"
        )
        return 1
    src, preset, crf, score = pick
    sys.stdout.write(
        f"src={src} preset={preset} crf={crf} vmaf={score:.3f} "
        f"(visited {len(visited)} encodes)\n"
    )
    return 0


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


def _run_predict(args: argparse.Namespace) -> int:
    """Phase C — per-shot VMAF prediction + validation harness.

    Pipeline:

    1.  Detect shots via :func:`per_shot.detect_shots` (TransNet V2
        binary if available; one-shot fallback otherwise).
    2.  Build a :class:`predictor.Predictor` (ONNX or analytical
        fallback).
    3.  Validate the predictor on K stratified shots — for each, run
        the real ffmpeg encode at the predictor-picked CRF + libvmaf
        score, compute residuals.
    4.  Emit the verdict + residuals + recommended per-shot CRFs as a
        JSON report.
    """
    import subprocess
    import tempfile

    from .encode import EncodeRequest, run_encode
    from .per_shot import detect_shots
    from .predictor import Predictor
    from .predictor_features import (
        FeatureExtractorConfig,
        _probe_video_geometry,
        extract_features,
    )
    from .predictor_validate import Verdict, validate_predictor
    from .score import ScoreRequest, run_score

    feat_cfg = FeatureExtractorConfig(
        ffmpeg_bin=args.ffmpeg_bin,
        ffprobe_bin=args.ffprobe_bin,
        use_saliency=args.use_saliency,
        saliency_model=args.saliency_model,
    )

    # Probe geometry first — detect_shots needs width/height/pix_fmt and
    # geometry is also required for the encode/score loop below.
    width, height, fps = _probe_video_geometry(args.source, feat_cfg, subprocess.run)
    if width <= 0 or height <= 0:
        print(
            "predict: ffprobe could not read source geometry "
            "(width/height); falling back is not safe — aborting.",
            file=sys.stderr,
        )
        return 1

    shots = detect_shots(
        Path(args.source),
        width=width,
        height=height,
        bitdepth=args.bitdepth,
        total_frames=args.total_frames or 0,
        per_shot_bin=args.per_shot_bin,
    )
    if not shots:
        print("predict: no shots detected; nothing to do", file=sys.stderr)
        return 1
    pix_fmt = "yuv420p"  # canonical reference format; matches saliency.py + the corpus loop

    predictor = Predictor(model_path=args.model)

    def _features(shot):
        return extract_features(
            shot=shot,
            source=args.source,
            codec=args.codec,
            config=feat_cfg,
        )

    # Validation work-area lives for the lifetime of _run_predict so
    # ``run_score``'s lazy decode of the distorted output finds the
    # encoded file still on disk. Cleaned at function exit.
    workdir = Path(tempfile.mkdtemp(prefix="vmaf-tune-predict-"))

    def _real_encode_and_score(shot: Shot, crf: int, codec: str) -> tuple[Path, float]:
        """Run the actual encode + libvmaf score for one validation shot.

        Workflow: extract the shot range from ``args.source`` to a raw
        YUV reference, encode that reference at the predictor-picked CRF
        via :func:`encode.run_encode`, score with
        :func:`score.run_score` (which handles the distorted-side
        decode internally), and return ``(encoded_path, vmaf_score)``.
        """
        ref_yuv = workdir / f"ref_{shot.start_frame}_{shot.end_frame}.yuv"
        dist_path = workdir / f"dist_{shot.start_frame}_{shot.end_frame}.mp4"

        if fps > 0.0:
            ss_arg = f"{shot.start_frame / fps:.6f}"
        else:
            ss_arg = str(shot.start_frame)
        extract_cmd = [
            args.ffmpeg_bin,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-ss",
            ss_arg,
            "-i",
            str(args.source),
            "-frames:v",
            str(shot.length),
            "-pix_fmt",
            pix_fmt,
            "-f",
            "rawvideo",
            str(ref_yuv),
        ]
        completed = subprocess.run(extract_cmd, capture_output=True, text=True, check=False)
        if completed.returncode != 0 or not ref_yuv.exists():
            return dist_path, float("nan")

        encode_req = EncodeRequest(
            source=ref_yuv,
            width=width,
            height=height,
            pix_fmt=pix_fmt,
            framerate=fps if fps > 0.0 else 24.0,
            encoder=codec,
            preset="medium",
            crf=crf,
            output=dist_path,
        )
        encode_result = run_encode(encode_req, ffmpeg_bin=args.ffmpeg_bin)
        if encode_result.exit_status != 0 or not dist_path.exists():
            return dist_path, float("nan")

        # Decode the encoded container to a raw YUV sidecar — the vmaf
        # CLI only accepts .yuv / .y4m inputs.
        dist_yuv = workdir / f"dist_{shot.start_frame}_{shot.end_frame}.decoded.yuv"
        decode_cmd = [
            args.ffmpeg_bin,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(dist_path),
            "-f",
            "rawvideo",
            "-pix_fmt",
            pix_fmt,
            str(dist_yuv),
        ]
        dec = subprocess.run(decode_cmd, capture_output=True, text=True, check=False)
        dist_for_score = dist_yuv if dec.returncode == 0 and dist_yuv.exists() else dist_path

        score_req = ScoreRequest(
            reference=ref_yuv,
            distorted=dist_for_score,
            width=width,
            height=height,
            pix_fmt=pix_fmt,
        )
        score_result = run_score(score_req)
        return dist_path, float(score_result.vmaf_score)

    try:
        report = validate_predictor(
            predictor=predictor,
            shots=shots,
            target_vmaf=args.target_vmaf,
            codec=args.codec,
            feature_extractor=_features,
            real_encode_and_score=_real_encode_and_score,
            k=args.validate_k,
            residual_threshold_vmaf=args.residual_threshold,
        )
    finally:
        # Clean the per-run scratch dir even on interrupt — the encoded
        # distorted files can run to gigabytes for long shots.
        import shutil

        shutil.rmtree(workdir, ignore_errors=True)

    # Optional conformal calibration: load the sidecar once and reuse
    # the same calibration for every per-shot interval. ``None`` falls
    # through to ``ConformalPredictor``'s degraded path
    # (``low == high == point``) so the JSON schema is stable whether
    # or not the operator shipped a sidecar.
    calibration = None
    uncalibrated = False
    if args.with_uncertainty:
        from .conformal import load_split_calibration

        if args.calibration_sidecar is not None:
            calibration = load_split_calibration(args.calibration_sidecar)
        else:
            uncalibrated = True

    def _interval_for(predicted_vmaf: float) -> dict | None:
        if not args.with_uncertainty:
            return None
        if calibration is None:
            return {"low": predicted_vmaf, "high": predicted_vmaf, "alpha": None}
        from .conformal import ConformalPredictor

        # Acknowledge the wrapper class — we use ``cal`` directly here
        # so the hot path doesn't re-run the predictor.
        _ = ConformalPredictor
        cal = calibration
        if args.alpha is not None:
            import dataclasses as _dc

            cal = _dc.replace(cal, alpha=args.alpha)
        # Re-derive the interval purely from the residual quantile —
        # we don't need to re-run the predictor since we already have
        # the point estimate. Construct a synthetic ConformalInterval.
        q = cal.quantile()
        low = max(0.0, min(100.0, predicted_vmaf - q))
        high = max(0.0, min(100.0, predicted_vmaf + q))
        return {"low": low, "high": high, "alpha": cal.alpha}

    payload = {
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
        "residuals": [
            {
                "shot_start": r.shot.start_frame,
                "shot_end": r.shot.end_frame,
                "crf": r.crf_picked,
                "predicted_vmaf": r.predicted_vmaf,
                "measured_vmaf": r.measured_vmaf,
                "residual": r.residual,
                **({"interval": _interval_for(r.predicted_vmaf)} if args.with_uncertainty else {}),
            }
            for r in report.residuals
        ],
    }
    rendered = json.dumps(payload, indent=2)
    if args.report_out is not None:
        args.report_out.write_text(rendered + "\n", encoding="utf-8")
    else:
        sys.stdout.write(rendered + "\n")
    return 0 if report.verdict != Verdict.FALL_BACK else 2


def _run_tune_per_shot(args: argparse.Namespace) -> int:
    # ADR-0613: pre-validate --score-backend before touching any source
    # files or launching bisect workers.  An unavailable backend must fail
    # fast here (exit 2 + actionable message) rather than surfacing as a
    # cryptic vmaf binary error buried inside the first shot's bisect loop.
    # Mirrors the select_backend() pattern already used by _run_ladder
    # (ADR-0511) and _run_corpus (ADR-0299 / ADR-0314).
    raw_backend_pershot = getattr(args, "score_backend", "auto")
    vmaf_bin_pershot = getattr(args, "vmaf_bin", "vmaf")
    try:
        _resolved_backend_pershot = select_backend(
            prefer=raw_backend_pershot, vmaf_bin=vmaf_bin_pershot
        )
    except BackendUnavailableError as exc:
        sys.stderr.write(f"vmaf-tune tune-per-shot: {exc}\n")
        return 2
    sys.stderr.write(f"vmaf-tune tune-per-shot: scoring backend = {_resolved_backend_pershot}\n")

    # ADR-0542: auto-probe geometry for container sources so operators
    # do not need to pre-extract a raw YUV or know --width/--height.
    # The probe result is written back onto `args` so all downstream
    # helpers (_build_per_shot_bisect_predicate, merge_shots, plan
    # serialisation) see consistent geometry without signature changes.
    _resolved_width = args.width
    _resolved_height = args.height
    _resolved_framerate = args.framerate  # may be None = not yet known
    _resolved_total_frames = args.total_frames if args.total_frames > 0 else None

    if not _source_needs_rawvideo_demux(args.src):
        # Container source: auto-probe missing geometry.
        if _resolved_width is None or _resolved_height is None or _resolved_framerate is None:
            from .report import probe_source as _probe_source

            try:
                _info = _probe_source(args.src)
            except Exception as _exc:
                sys.stderr.write(f"vmaf-tune tune-per-shot: ffprobe failed on {args.src}: {_exc}\n")
                return 2
            if _resolved_width is None:
                _resolved_width = _info.width or None
            if _resolved_height is None:
                _resolved_height = _info.height or None
            if _resolved_framerate is None and _info.fps > 0.0:
                _resolved_framerate = _info.fps
            if _resolved_total_frames is None and _info.frame_count > 0:
                _resolved_total_frames = _info.frame_count
    else:
        # Raw YUV source: explicit geometry is required.
        if _resolved_width is None or _resolved_height is None:
            sys.stderr.write(
                "vmaf-tune tune-per-shot: --width and --height are required "
                "for raw YUV sources. For container sources (mp4, mkv, …) "
                "these flags are optional and auto-probed via ffprobe.\n"
            )
            return 2

    if _resolved_width is None or _resolved_height is None:
        sys.stderr.write(
            "vmaf-tune tune-per-shot: could not determine source width/height. "
            "Pass --width and --height explicitly.\n"
        )
        return 2
    if _resolved_framerate is None:
        _resolved_framerate = 24.0  # safe default when probe yields nothing

    # Write resolved geometry back onto args so every downstream helper
    # that reads args.width / args.height / args.framerate is consistent.
    args.width = _resolved_width
    args.height = _resolved_height
    args.framerate = _resolved_framerate

    total_frames = _resolved_total_frames
    # ADR-0512: thread the user-tunable scene threshold + uniform-window
    # splitter through so short clips and under-cutting content still
    # produce a multi-shot timeline for the per-shot tuner.
    max_shot_duration = getattr(args, "max_shot_duration", None)
    if max_shot_duration is not None and max_shot_duration <= 0.0:
        max_shot_duration = None
    shots = detect_shots(
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
    predicate_label = "bisect"
    scratch_ctx = None
    # ADR-0536: bitrate_sidecar is populated by the bisect predicate closure
    # with the measured kbps for each shot.  External predicates (--predicate-module)
    # do not provide bitrate data so the sidecar stays empty, leaving
    # ShotRecommendation.bitrate_kbps at its NaN default (serialised as null).
    bitrate_sidecar: dict[tuple[int, int], float] = {}
    try:
        if args.predicate_module:
            predicate = _load_per_shot_predicate(args.predicate_module)
            predicate_label = args.predicate_module
        else:
            crf_range = _parse_optional_crf_range(
                args.crf_min,
                args.crf_max,
            )
            # ADR-0549: honour --workdir / VMAFTUNE_WORKDIR for
            # per-shot bisect scratch space so artefacts land on a
            # volume with sufficient free space rather than /tmp.
            from .bisect import _workdir_parent as _bwp

            _pershot_wd = getattr(args, "workdir", None)
            _pershot_parent = _pershot_wd if _pershot_wd is not None else _bwp()
            if _pershot_parent is not None:
                _pershot_parent.mkdir(parents=True, exist_ok=True)
            scratch_ctx = tempfile.TemporaryDirectory(
                prefix="vmaf-tune-per-shot-", dir=_pershot_parent
            )
            # ADR-0577: configure the decode semaphore for per-shot bisect.
            import threading as _threading_pershot

            from .bisect import set_decode_semaphore as _set_decode_sem

            _ps_max_decodes = int(getattr(args, "max_concurrent_decodes", 1))
            _set_decode_sem(_ps_max_decodes)
            _pershot_decode_sem = _threading_pershot.Semaphore(_ps_max_decodes)

            # ADR-0624 / ADR-0615: build the NR proxy backend when the
            # --fast-nr flag is passed.  Errors surface immediately so the
            # operator does not wait through shot detection before finding out
            # onnxruntime is missing.
            _pershot_nr_proxy = None
            if getattr(args, "fast_nr", False):
                from .score_backend import (
                    NRProxyBackend,
                    NRProxyBackendError,
                )

                try:
                    _pershot_nr_proxy = NRProxyBackend()
                    _ps_delta = _pershot_nr_proxy.calibration_threshold
                    sys.stderr.write(
                        f"vmaf-tune tune-per-shot: --fast-nr enabled; "
                        f"δ_fast={_ps_delta:.1f} VMAF (NR early-elimination)\n"
                    )
                except NRProxyBackendError as exc:
                    sys.stderr.write(f"vmaf-tune tune-per-shot: --fast-nr: {exc}\n")
                    raise

            predicate, bitrate_sidecar = _build_per_shot_bisect_predicate(
                args,
                scratch=Path(scratch_ctx.name),
                crf_range=crf_range,
                decode_semaphore=_pershot_decode_sem,
                nr_proxy_backend=_pershot_nr_proxy,
            )
        recs = tune_per_shot(
            shots,
            target_vmaf=args.target_vmaf,
            encoder=args.encoder,
            predicate=predicate,
        )
    except (AttributeError, ImportError, RuntimeError, ValueError) as exc:
        sys.stderr.write(f"vmaf-tune tune-per-shot: {exc}\n")
        return 2
    finally:
        if scratch_ctx is not None:
            scratch_ctx.cleanup()

    # Patch each ShotRecommendation with the measured bitrate collected by the
    # predicate sidecar (ADR-0536).  ShotRecommendation is frozen so we
    # reconstruct via dataclasses.replace; shots absent from the sidecar (e.g.
    # external predicate path) keep their NaN default.
    recs = [
        dataclasses.replace(
            r,
            bitrate_kbps=bitrate_sidecar.get(
                (r.shot.start_frame, r.shot.end_frame),
                r.bitrate_kbps,
            ),
        )
        for r in recs
    ]

    plan = merge_shots(
        recs,
        source=args.src,
        output=args.output,
        framerate=args.framerate,
        encoder=args.encoder,
        segment_dir=args.segment_dir,
        ffmpeg_bin=args.ffmpeg_bin,
    )

    def _shot_bitrate(br: float) -> float | None:
        # ADR-0531: serialise NaN/inf as null (RFC-8259-portable JSON).
        # The report ingester maps null → NaN → "—" in the Bitrate column,
        # which is the correct rendering for synthetic/dry-run predicates
        # that never perform a real encode.
        if not isinstance(br, float) or math.isnan(br) or math.isinf(br):
            return None
        return round(br, 2)

    plan_doc = {
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
                "bitrate_kbps": _shot_bitrate(r.bitrate_kbps),
            }
            for r in plan.recommendations
        ],
        "segment_commands": [list(c) for c in plan.segment_commands],
        "concat_command": list(plan.concat_command),
    }
    rendered = json.dumps(plan_doc, indent=2, sort_keys=True)
    if args.plan_out is None:
        sys.stdout.write(rendered)
        sys.stdout.write("\n")
    else:
        args.plan_out.parent.mkdir(parents=True, exist_ok=True)
        args.plan_out.write_text(rendered + "\n", encoding="utf-8")
        sys.stderr.write(f"wrote plan -> {args.plan_out}\n")

    if args.script_out is not None:
        args.script_out.parent.mkdir(parents=True, exist_ok=True)
        args.script_out.write_text(plan_to_shell_script(plan), encoding="utf-8")
        sys.stderr.write(f"wrote shell script -> {args.script_out}\n")

    # Derive the segments directory.  Prefer the explicit --segment-dir flag,
    # then the directory that contains --plan-out (writable by construction —
    # the plan JSON was just written there), and only fall back to
    # <output>.parent/segments when neither is set.  The fallback resolves
    # relative to the CWD, which may be read-only inside a bind-mounted
    # container workspace (ADR-0530).
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
    """Build the production Phase-D predicate from Phase-B bisect.

    ``bisect_target_vmaf`` operates on raw YUV references, so the
    per-shot CLI first extracts each detected shot to a temporary raw
    YUV file and then runs the existing encode+score bisect loop over
    that isolated shot.

    Returns a ``(predicate, bitrate_sidecar)`` pair.  ``bitrate_sidecar``
    is a dict keyed by ``(start_frame, end_frame)`` that the predicate
    closure populates with the measured ``bitrate_kbps`` for each shot as
    the bisect completes.  The caller reads the sidecar after
    :func:`tune_per_shot` to patch :attr:`ShotRecommendation.bitrate_kbps`
    without widening the :data:`PredicateFn` return type (ADR-0536).

    ``nr_proxy_backend`` is an optional
    :class:`~vmaftune.score_backend.NRProxyBackend` for fast NR
    pre-scoring (ADR-0624 / ADR-0615). Injected from the ``--fast-nr``
    CLI flag.  When ``None`` (default), full-reference scoring is used
    throughout.
    """
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


def _run_recommend_saliency(args: argparse.Namespace) -> int:
    """Bucket #2 — single saliency-aware encode (ADR-0287).

    Builds an :class:`~vmaftune.encode.EncodeRequest` from the CLI
    flags and delegates to :func:`vmaftune.saliency.saliency_aware_encode`,
    which runs the fork's ``saliency_student_v1`` ONNX model over the
    source, materialises the selected encoder's ROI sidecar/argv, and
    runs one encode biased toward salient regions. Falls back to a
    plain encode when onnxruntime / the model are unavailable so the
    caller always gets a result.
    """
    from .encode import EncodeRequest
    from .saliency import SaliencyConfig, saliency_aware_encode

    adapter = get_adapter(args.encoder)
    crf = args.crf if args.crf is not None else adapter.quality_default

    # If --output ends with .json the caller intends the path as a JSON
    # report destination, not a video container.  Encode to a sibling
    # <stem>_encoded.mp4 so ffmpeg gets a valid muxer, then write the
    # result JSON to the original path.
    output_path = args.output
    json_report_path: Path | None = None
    if output_path is not None and output_path.suffix.lower() == ".json":
        json_report_path = output_path
        output_path = output_path.with_name(output_path.stem + "_encoded.mp4")

    request = EncodeRequest(
        source=args.src,
        width=args.width,
        height=args.height,
        pix_fmt=args.pix_fmt,
        framerate=args.framerate,
        encoder=args.encoder,
        preset=args.preset,
        crf=crf,
        output=output_path,
    )
    if not args.saliency_aware:
        # --saliency-aware was not set; run a plain encode without touching
        # the saliency model.  Calling saliency_aware_encode with config=None
        # would silently create a default SaliencyConfig() and run the model
        # anyway — guard here to enforce the intent.
        from .encode import run_encode

        result = run_encode(request, ffmpeg_bin=args.ffmpeg_bin)
    else:
        cfg = SaliencyConfig(
            foreground_offset=args.saliency_offset,
            temporal_aggregator=args.saliency_aggregator,
            ema_alpha=args.saliency_ema_alpha,
            allow_unsupported_encoder_fallback=args.saliency_fallback_plain,
        )
        result = saliency_aware_encode(
            request,
            duration_frames=args.duration_frames,
            model_path=args.saliency_model,
            config=cfg,
            ffmpeg_bin=args.ffmpeg_bin,
        )
    payload = {
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
    payload_text = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if json_report_path is not None:
        # The caller asked for a .json report path; write JSON there
        # and print the path to stdout so the shell can find it.
        json_report_path.parent.mkdir(parents=True, exist_ok=True)
        json_report_path.write_text(payload_text, encoding="utf-8")
        sys.stdout.write(str(json_report_path) + "\n")
    else:
        sys.stdout.write(payload_text)
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


def _run_ladder(args: argparse.Namespace) -> int:
    """Phase E — per-title bitrate ladder (ADR-0295).

    Builds the convex hull of (resolution × target-VMAF) sample points,
    selects ``--quality-tiers`` knees by the chosen spacing strategy,
    and emits an HLS / DASH / JSON manifest. The Phase B sampler is
    used by default; tests can inject a stub via the ``ladder.SamplerFn``
    parameter.

    When ``--with-uncertainty`` is set the production path preserves
    per-row ``vmaf_interval`` payloads from the corpus sampler,
    applies :func:`vmaftune.ladder.apply_uncertainty_recipe`, and
    then selects knees from the adjusted rung set. Point-only sampler
    rows get a conservative centred interval using the active
    ``wide_interval_min_width`` threshold, so they still participate
    in midpoint insertion instead of bypassing the recipe.
    """
    from .ladder import build_and_emit, make_default_sampler
    from .uncertainty import load_confidence_thresholds

    thresholds = None
    if getattr(args, "with_uncertainty", False):
        thresholds = load_confidence_thresholds(getattr(args, "uncertainty_sidecar", None))
    resolutions = _parse_resolutions(args.resolutions)
    target_vmafs = _parse_target_vmafs(args.target_vmafs)
    # Bind the default sampler to the CLI source-shape flags so the
    # corpus sweep encodes at the right framerate / pix_fmt / duration
    # and a smoke run can drop the canonical 5-point CRF schedule for
    # a shorter list (Bug #4 / Bug #5, BBB e2e 2026-05-17).
    crf_sweep = None
    if getattr(args, "crf_sweep", None):
        try:
            crf_sweep = tuple(int(c.strip()) for c in args.crf_sweep.split(",") if c.strip())
        except ValueError as exc:
            sys.stderr.write(f"vmaf-tune ladder: invalid --crf-sweep: {exc}\n")
            return 2
        if not crf_sweep:
            sys.stderr.write("vmaf-tune ladder: --crf-sweep produced an empty list\n")
            return 2
    # ADR-0498 / Bug #v2-B: source dims default to the largest rung
    # in the resolution list so a multi-resolution ladder against a
    # raw YUV source decodes at the source's native geometry and
    # downscales for sub-source rungs via a -vf scale=W:H filter.
    src_w = getattr(args, "src_width", None)
    src_h = getattr(args, "src_height", None)
    if src_w is None or src_h is None:
        # Pick the largest (by pixel count) requested rung. Single-
        # resolution ladders trivially match the source so the
        # legacy single-res behaviour is preserved.
        max_w, max_h = max(resolutions, key=lambda wh: wh[0] * wh[1])
        if src_w is None:
            src_w = int(max_w)
        if src_h is None:
            src_h = int(max_h)
    # ADR-0505 / BBB e2e v5 Bug #V5-2 + #V5-3: collect the full
    # per-CRF sweep cloud via a sink list shared across all
    # ``(resolution, target_vmaf)`` cells. The sink supersedes the
    # historic per-target picks as the source of the JSON ``samples``
    # array — see :func:`vmaftune.ladder.build_and_emit` for the
    # dedup + emit semantics.
    from .ladder import LadderPoint as _LadderPoint

    # Bug C / ADR-0511: resolve --score-backend up-front so an
    # unavailable backend errors out before any encodes start.
    raw_backend = getattr(args, "score_backend", "auto")
    vmaf_bin = getattr(args, "vmaf_bin", "vmaf")
    try:
        resolved_backend = select_backend(prefer=raw_backend, vmaf_bin=vmaf_bin)
    except BackendUnavailableError as exc:
        sys.stderr.write(f"vmaf-tune ladder: {exc}\n")
        return 2
    sys.stderr.write(f"vmaf-tune ladder: scoring backend = {resolved_backend}\n")
    # Pass None when auto resolved to CPU so the corpus step omits the
    # explicit ``--backend`` flag and lets libvmaf use its own default.
    # When the user explicitly requested a backend (or auto resolved to a
    # GPU), pass the resolved name through to CorpusOptions.score_backend.
    ladder_backend: str | None = (
        None if raw_backend == "auto" and resolved_backend == "cpu" else resolved_backend
    )

    cloud_sink: list[_LadderPoint] = []
    sampler = make_default_sampler(
        pix_fmt=getattr(args, "pix_fmt", "yuv420p"),
        framerate=float(getattr(args, "framerate", 24.0)),
        duration_s=float(getattr(args, "duration_s", 1.0)),
        crf_sweep=crf_sweep,
        src_width=int(src_w),
        src_height=int(src_h),
        cloud_sink=cloud_sink,
        score_backend=ladder_backend,
        vmaf_model=_resolve_vmaf_model(args),
    )
    # BBB e2e v6 Bug #V6-3 (ADR-0506): the sampler can legitimately
    # raise ``RuntimeError`` ("default sampler produced no scorable
    # encodes …") when the source / rung combination yields zero
    # successful corpus rows. Letting the exception bubble out of
    # ``main()`` would terminate with a Python traceback AND
    # ``sys.exit(1)`` — but the CLI wrapper that wraps ``main()``
    # historically caught nothing and returned 0 on traceback, so CI
    # / shell scripts could not distinguish a no-rung ladder from a
    # green one. Catching the failure here and returning 2 (the
    # canonical "operational failure" exit code shared with the
    # other vmaf-tune subcommands) restores the CI / shell-script
    # contract.
    try:
        manifest = build_and_emit(
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
    except (RuntimeError, ValueError, OSError) as exc:
        sys.stderr.write(f"vmaf-tune ladder: {exc}\n")
        return 2
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(manifest, encoding="utf-8")
        sys.stderr.write(f"wrote ladder manifest -> {args.output}\n")
    else:
        sys.stdout.write(manifest)
        if not manifest.endswith("\n"):
            sys.stdout.write("\n")
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
    probe_fn: object | None = None,
    warn_stream: TextIO | None = None,
) -> tuple[int | None, int | None, float, float]:
    """Reconcile user-supplied geometry with an ffprobe of a container source.

    ADR-0509 / BBB e2e v7 Bug #V7-1 root cause: the historic compare
    CLI required ``--framerate`` (default ``24.0``) and ``--duration``
    (default ``0.0``) and threaded them through to ``make_bisect_predicate``
    verbatim. When ``--src`` is a container whose native rate is not
    24 fps (e.g. 60 fps BBB), the per-iteration ``frame_skip_ref`` /
    ``frame_cnt`` derived from the user-supplied ``--framerate`` no
    longer indexes the same source frames the encoder pulled — the
    distorted MKV is decoded back to YUV at the container's native
    rate while ``vmaf --frame_skip_ref`` skips frames at the wrong
    rate, comparing misaligned content and collapsing the apparent
    VMAF (BBB 60fps → VMAF=90 at CRF=6, physically wrong).

    The fix: when ``src`` is a container and the user did NOT
    explicitly override ``--framerate`` / ``--duration`` (i.e. left
    them at their argparse defaults), probe the source via
    :func:`vmaftune.report.probe_source` and substitute the probed
    values. When the user passed values that disagree with the probe,
    keep the user override (operators may intentionally subsample) but
    emit a one-line stderr warning so a misconfigured run is visible.
    Width and height: when the user did not pass them and the source
    is a container, fill them in from the probe (the existing rung-
    target scale filter in :mod:`encode` handles geometry mismatch).

    Returns the (possibly probe-derived) ``(width, height, framerate,
    duration_s)`` tuple. The probe is best-effort — if it returns
    zero / fails, user-supplied values are kept.

    Sister fix to ADR-0505 (ladder ``source_is_container`` plumbing).
    The compare path already plumbed ``source_is_container`` correctly
    through :class:`vmaftune.encode.EncodeRequest` (see ``bisect.py``);
    the remaining defect was that the user-supplied ``--framerate``
    silently disagreed with the container's native rate, defeating the
    per-iteration frame-window alignment between reference and
    distorted decodes.
    """
    # Local imports keep the heavy ffprobe path off the import graph
    # of CLI smoke tests that don't exercise compare.
    from .score import VMAF_RAW_SUFFIXES

    if probe_fn is None:
        from .report import probe_source as _probe_source
    else:
        _probe_source = probe_fn  # type: ignore[assignment]

    stream = warn_stream if warn_stream is not None else sys.stderr

    if Path(src).suffix.lower() in VMAF_RAW_SUFFIXES:
        # Raw YUV source — no probe possible / required. Width / height
        # are already checked as mandatory by the caller; framerate /
        # duration fall through verbatim.
        return width, height, framerate, duration_s

    try:
        info = _probe_source(Path(src))  # type: ignore[operator]
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

    out_w = width
    out_h = height
    out_fr = framerate
    out_dur = duration_s

    # Width / height: fill in from probe when user did not pass them.
    # Don't override an explicit user value — operators may intentionally
    # request a rung target different from the source rendition (the
    # encode path's ``-vf scale`` filter handles that, see ADR-0505).
    if out_w is None and probed_w > 0:
        out_w = probed_w
    if out_h is None and probed_h > 0:
        out_h = probed_h

    # Framerate: probe overrides the argparse default; warn on explicit
    # mismatch but honour the user's override (subsampling is legitimate).
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

    # Duration: argparse default is ``0.0`` (= full source). Fill from
    # probe so downstream ``--duration`` clamping (ADR-0506 / V6-1) and
    # the score-side reference decode budget actually bound to a real
    # number when the user didn't pin one.
    if duration_was_default and probed_dur > 0.0:
        out_dur = probed_dur

    return out_w, out_h, out_fr, out_dur


def _run_compare(args: argparse.Namespace) -> int:
    """Compare codec adapters at a target VMAF using Phase B bisect.

    Parses the comma-separated ``--encoders`` list, delegates to
    :func:`vmaftune.compare.compare_codecs` (which runs the per-codec
    predicate in a thread pool and ranks by smallest bitrate), then
    emits a markdown / JSON / CSV report via
    :func:`vmaftune.compare.emit_report`.

    Default CLI behaviour now binds :func:`vmaftune.bisect.make_bisect_predicate`
    from the source geometry flags, so ``compare`` is no longer a
    report-only scaffold. ``--predicate-module`` remains as an advanced
    test/operator hook and bypasses the bisect backend.

    ADR-0509 / BBB e2e v7 Bug #V7-1: when ``--src`` is a container the
    argparse defaults for ``--framerate`` (24.0) and ``--duration`` (0)
    no longer index the same frames the encoder pulled, mis-aligning
    the per-iteration reference vs. distorted YUV decode and collapsing
    the apparent VMAF (sister bug to ADR-0505's ladder path). The
    compare runner now auto-probes container sources via
    :func:`vmaftune.report.probe_source` and substitutes the probed
    framerate / duration when the user left those flags at their
    defaults; explicit user values still win, with a stderr warning on
    explicit mismatch.
    """
    import contextlib
    import os
    import threading

    from .bisect import make_bisect_predicate, set_decode_semaphore
    from .compare import (
        DEFAULT_CPU_ENCODERS,
        DEFAULT_VAAPI_DEVICE,
        compare_codecs,
        compare_codecs_sweep,
        emit_report,
        emit_sweep_report,
        probe_encoder_available,
        supported_formats,
    )
    from .encoder_runtime import EncoderRuntimeSpec, resolve_encoder_runtime_specs
    from .score import VMAF_RAW_SUFFIXES, _decode_to_raw_yuv

    # ADR-0601: resolve the VA-API render-node for QSV device init.
    # Resolution: --vaapi-device > VMAFTUNE_VAAPI_DEVICE env var > default.
    vaapi_device: str = (
        getattr(args, "vaapi_device", None)
        or os.environ.get("VMAFTUNE_VAAPI_DEVICE", "")
        or DEFAULT_VAAPI_DEVICE
    )

    # ADR-0641: ``--encoders`` defaults to the current production CPU
    # decision set when omitted. Legacy x264/VP9 BBB sweeps remain
    # available only when listed explicitly.
    encoders_raw = args.encoders if args.encoders is not None else ",".join(DEFAULT_CPU_ENCODERS)
    encoder_tokens = [token.strip() for token in encoders_raw.split(",") if token.strip()]
    if not encoder_tokens:
        sys.stderr.write("vmaf-tune compare: --encoders is empty\n")
        return 2
    try:
        runtime_specs = resolve_encoder_runtime_specs(
            encoder_tokens,
            ffmpeg_bin=args.ffmpeg_bin,
            encoder_ffmpeg_bins=getattr(args, "encoder_ffmpeg_bin", ()),
        )
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune compare: {exc}\n")
        return 2
    encoders = [spec.token for spec in runtime_specs]
    runtime_by_token = {spec.token: spec for spec in runtime_specs}

    def _annotate_runtime_result(result, spec: EncoderRuntimeSpec):
        return dataclasses.replace(
            result,
            codec=spec.token,
            adapter=spec.adapter,
            runtime_variant=spec.variant,
            ffmpeg_bin=spec.ffmpeg_bin,
        )

    def _runtime_row_metadata(codec: str) -> dict[str, str]:
        spec = runtime_by_token[codec]
        return {
            "adapter": spec.adapter,
            "runtime_variant": spec.variant,
            "ffmpeg_bin": spec.ffmpeg_bin,
        }

    _profile_formats = ("html", "both")
    _supported_compare_formats = (*supported_formats(), *_profile_formats)
    if args.format not in _supported_compare_formats:
        sys.stderr.write(
            f"vmaf-tune compare: unsupported --format {args.format!r}; "
            f"expected one of {_supported_compare_formats}\n"
        )
        return 2
    if args.format == "both" and args.output is None:
        sys.stderr.write("vmaf-tune compare: --format both requires --output PATH\n")
        return 2

    # ADR-0613: pre-validate --score-backend before engaging any bisect or
    # CRF-sweep worker, mirroring the select_backend() pattern in _run_ladder
    # (ADR-0511) and _run_corpus (ADR-0299 / ADR-0314).  An unavailable backend
    # must fail fast here (exit 2 + actionable message) rather than surfacing
    # as a cryptic vmaf binary error buried inside a bisect iteration.
    # Note: the compare argparse argument defaults to None (not "auto") when the
    # flag is omitted; treat None as "auto" here so select_backend receives a
    # valid preference string.
    _raw_backend_compare = getattr(args, "score_backend", None) or "auto"
    _vmaf_bin_compare = getattr(args, "vmaf_bin", "vmaf")
    try:
        _resolved_backend_compare = select_backend(
            prefer=_raw_backend_compare, vmaf_bin=_vmaf_bin_compare
        )
    except BackendUnavailableError as exc:
        sys.stderr.write(f"vmaf-tune compare: {exc}\n")
        return 2
    sys.stderr.write(f"vmaf-tune compare: scoring backend = {_resolved_backend_compare}\n")

    # ADR-0542: CRF-sweep mode — bypass bisect entirely when --no-bisect
    # is set. Dispatched after format validation so the format guard still
    # applies; dispatched before the target-VMAF / predicate-module blocks
    # because none of that logic is needed for a sweep.
    if getattr(args, "no_bisect", False):
        if args.format in _profile_formats:
            sys.stderr.write(
                "vmaf-tune compare --no-bisect: --format html/both is not supported; "
                "emit --format json and pass it to vmaf-tune report.\n"
            )
            return 2
        return _run_compare_crf_sweep(args, encoders, runtime_specs=runtime_specs)

    # ADR-0516: ``--target-vmafs`` parses to a list of floats; falls
    # back to ``[--target-vmaf]`` (single-target legacy path) when the
    # multi-target flag is not passed. Duplicates collapse and the list
    # is sorted ascending so the report's rate-quality curve is stable.
    #
    # ADR-0534 back-compat: ``--target-vmafs`` now has a non-empty
    # default (``75,80,85,90,93``) so the realistic-streaming sweep
    # runs out of the box. When the user explicitly passes
    # ``--target-vmaf NN`` without touching ``--target-vmafs``, the
    # legacy single-target v1 path is honoured — preserves CSV / JSON
    # back-compat for operators / scripts that pin a single VMAF.
    target_vmafs_was_default = getattr(args, "_target_vmafs_was_default", True)
    target_vmaf_was_default = getattr(args, "_target_vmaf_was_default", True)
    use_single_target_legacy = target_vmafs_was_default and not target_vmaf_was_default
    if args.target_vmafs and not use_single_target_legacy:
        try:
            target_vmafs = sorted(
                {float(t.strip()) for t in args.target_vmafs.split(",") if t.strip()}
            )
        except ValueError as exc:
            sys.stderr.write(f"vmaf-tune compare: invalid --target-vmafs: {exc}\n")
            return 2
        if not target_vmafs:
            sys.stderr.write("vmaf-tune compare: --target-vmafs is empty\n")
            return 2
    else:
        target_vmafs = [float(args.target_vmaf)]
    predicate = None
    sweep_predicate = None
    if args.predicate_module:
        try:
            loaded_predicate = _load_compare_predicate(args.predicate_module)
            # For sweeps, the same module-level predicate accepts the
            # per-call target_vmaf — no extra binding needed.
        except (AttributeError, ImportError, ValueError) as exc:
            sys.stderr.write(f"vmaf-tune compare: invalid --predicate-module: {exc}\n")
            return 2

        def _predicate_module_dispatcher(codec: str, src_, target_: float):
            spec = runtime_by_token[codec]
            return _annotate_runtime_result(loaded_predicate(codec, src_, target_), spec)

        predicate = _predicate_module_dispatcher
        sweep_predicate = _predicate_module_dispatcher
    else:
        # Container-source auto-probe (ADR-0509 / BBB e2e v7 Bug #V7-1).
        # ``_framerate_was_default`` / ``_duration_was_default`` are
        # sentinel attributes the argparse parser stamps onto ``args``
        # when the user did NOT pass the flag explicitly. See
        # ``build_parser`` for the wiring.
        framerate_was_default = getattr(args, "_framerate_was_default", False)
        duration_was_default = getattr(args, "_duration_was_default", False)
        resolved_w, resolved_h, resolved_fr, resolved_dur = _resolve_compare_source_geometry(
            Path(args.src),
            width=args.width,
            height=args.height,
            framerate=args.framerate,
            duration_s=args.duration,
            framerate_was_default=framerate_was_default,
            duration_was_default=duration_was_default,
        )
        if resolved_w is None or resolved_h is None:
            sys.stderr.write(
                "vmaf-tune compare: --width and --height are required for the "
                "real bisect backend. Use --predicate-module MODULE:CALLABLE "
                "to provide a custom predicate.\n"
            )
            return 2
        crf_range = None
        if args.crf_min is not None or args.crf_max is not None:
            if args.crf_min is None or args.crf_max is None:
                sys.stderr.write("vmaf-tune compare: pass both --crf-min and --crf-max\n")
                return 2
            crf_range = (args.crf_min, args.crf_max)
        # ADR-0613: backend already pre-validated above; map "auto" → None.
        score_backend = None if args.score_backend in (None, "auto") else args.score_backend

        # ADR-0549: CLI --workdir beats env var; env var beats /tmp.
        _compare_workdir = getattr(args, "workdir", None)

        # ADR-0577: configure the decode concurrency cap. The semaphore
        # is shared across all thread-pool workers so only
        # --max-concurrent-decodes reference-YUV decodes run at once.
        # Default 1 = serial decodes (safest for disk-space constrained
        # volumes like /probes at 420 GB with 110 GB per 1080p source).
        _max_decodes = int(getattr(args, "max_concurrent_decodes", 1))
        set_decode_semaphore(_max_decodes)
        _decode_sem = threading.Semaphore(_max_decodes)

        # ADR-0624 / ADR-0615: build the NR proxy backend when --fast-nr
        # is passed. A single shared instance is used for all codecs and
        # all target-VMAF rungs in the compare run so the ONNX model is
        # loaded once and the per-CRF cache is shared across the sweep.
        # NRProxyBackendError from construction (missing onnxruntime,
        # missing model file) is surfaced immediately as a user error.
        _nr_proxy: NRProxyBackend | None = None
        if getattr(args, "fast_nr", False):
            from .score_backend import (
                NRProxyBackend,
                NRProxyBackendError,
            )

            try:
                _nr_proxy = NRProxyBackend()
                # Eager δ_fast resolution so any sidecar-read error is
                # surfaced before any encode starts.
                _delta = _nr_proxy.calibration_threshold
                sys.stderr.write(
                    f"vmaf-tune compare: --fast-nr enabled; "
                    f"δ_fast={_delta:.1f} VMAF (NR early-elimination)\n"
                )
            except NRProxyBackendError as exc:
                sys.stderr.write(f"vmaf-tune compare: --fast-nr: {exc}\n")
                return 2

        def _build_bisect_for_target(target: float, *, ffmpeg_bin: str):
            return make_bisect_predicate(
                target_vmaf=target,
                width=resolved_w,
                height=resolved_h,
                pix_fmt=args.pix_fmt,
                framerate=resolved_fr,
                duration_s=resolved_dur,
                sample_clip_seconds=args.sample_clip_seconds,
                preset=args.preset,
                crf_range=crf_range,
                max_iterations=args.max_iterations,
                vmaf_model=_resolve_vmaf_model(args),
                score_backend=score_backend,
                ffmpeg_bin=ffmpeg_bin,
                vmaf_bin=args.vmaf_bin,
                workdir=_compare_workdir,
                decode_semaphore=_decode_sem,
                nr_proxy_backend=_nr_proxy,
            )

        # Single-target legacy: build one predicate against
        # --target-vmaf and use the v1 emitter. Sweep mode: build a
        # per-call dispatcher that lazily memoises one bisect closure
        # per distinct target VMAF, so the cross-product (codec x
        # target) still gets the right ``make_bisect_predicate`` for
        # each rung.
        _bisect_cache: dict[tuple[str, float], object] = {}

        def _sweep_dispatcher(codec, src_, target_):
            spec = runtime_by_token[codec]
            target_f = float(target_)
            cache_key = (spec.ffmpeg_bin, target_f)
            if cache_key not in _bisect_cache:
                _bisect_cache[cache_key] = _build_bisect_for_target(
                    target_f,
                    ffmpeg_bin=spec.ffmpeg_bin,
                )
            result = _bisect_cache[cache_key](spec.adapter, src_, target_)  # type: ignore[operator]
            return _annotate_runtime_result(result, spec)

        predicate = _sweep_dispatcher
        sweep_predicate = _sweep_dispatcher

    # ADR-0607: decode the reference YUV exactly once for the entire compare
    # run, then share the decoded path with every worker. This eliminates
    # the quadratic re-decode pattern from ADR-0577 / PR #1354 where each
    # bisect's finally block deleted the shared 118 GB reference and every
    # subsequent worker re-decoded it from scratch, yielding
    # N_workers × N_iters re-decodes instead of 1. The pre_decoded_ref
    # argument lets compare_codecs / compare_codecs_sweep pass the raw-YUV
    # path to every bisect worker; each bisect sees src_is_container=False
    # and skips its own reference decode entirely.
    #
    # For --no-bisect CRF-sweep mode the same optimisation applies; we
    # pass the decoded path through _run_compare_crf_sweep's workdir.
    # Encoding workers still produce per-encoder distorted YUVs in the
    # shared workdir; those are cleaned up by _encode_and_score as before.
    #
    # Cleanup responsibility: cli.py owns the pre-decoded file and deletes
    # it after both pool.shutdown() and the report are done, even on error.
    _src_path = Path(args.src)
    _src_is_container = _src_path.suffix.lower() not in VMAF_RAW_SUFFIXES
    _pre_decoded_ref: Path | None = None
    _decode_workdir: Path | None = None

    # Only decode when using the real bisect backend (not custom predicate)
    # and only when the source is a container.
    _should_pre_decode = _src_is_container and not getattr(args, "predicate_module", None)

    if _should_pre_decode:
        # Resolve the workdir for the shared-ref decode. Use the same
        # precedence as the per-bisect path (ADR-0549): --workdir > env var.
        from .bisect import _workdir_parent

        _given_workdir = getattr(args, "workdir", None)
        _decode_workdir = _given_workdir if _given_workdir is not None else _workdir_parent()
        if _decode_workdir is None:
            import tempfile

            _decode_workdir = Path(tempfile.mkdtemp(prefix="vmaftune-compare-"))

        _decode_workdir = Path(_decode_workdir)
        _decode_workdir.mkdir(parents=True, exist_ok=True)
        _pre_decoded_ref = _decode_workdir / (_src_path.stem + ".shared-ref.yuv")

        if not _pre_decoded_ref.exists():
            sys.stderr.write(
                f"vmaf-tune compare: decoding shared reference YUV once "
                f"({_src_path.name} -> {_pre_decoded_ref.name}) ...\n"
            )
            # Resolve the effective duration and pix_fmt from the args
            # already resolved above (only set when not using predicate_module).
            # ``_should_pre_decode`` gates on not predicate_module, so
            # ``resolved_dur`` is always defined at this point (set by
            # ``_resolve_compare_source_geometry`` in the bisect path above).
            _ref_dur: float | None = (
                float(resolved_dur) if float(resolved_dur) > 0.0 else None  # type: ignore[name-defined]
            )
            _ref_pix_fmt = getattr(args, "pix_fmt", "yuv420p")
            _ref_ffmpeg = getattr(args, "ffmpeg_bin", "ffmpeg")

            _rc = _decode_to_raw_yuv(
                _src_path,
                _pre_decoded_ref,
                pix_fmt=_ref_pix_fmt,
                ffmpeg_bin=_ref_ffmpeg,
                duration_s=_ref_dur,
            )
            if _rc != 0 or not _pre_decoded_ref.exists():
                sys.stderr.write(
                    f"vmaf-tune compare: shared reference decode failed (rc={_rc}); "
                    "falling back to per-worker decode.\n"
                )
                _pre_decoded_ref = None
            else:
                _ref_bytes = _pre_decoded_ref.stat().st_size
                sys.stderr.write(
                    f"vmaf-tune compare: shared reference decoded "
                    f"({_ref_bytes / 1024**3:.1f} GB) — "
                    f"all {len(encoders)} workers will share it.\n"
                )

    try:
        # Pick the v2 sweep path when more than one target was requested.
        if len(target_vmafs) > 1:
            # Build the encoder-availability probe: real ffmpeg-backed for
            # the default path, no-op when a custom predicate-module is in
            # play (tests inject fake predicates and shouldn't be forced to
            # shell out).
            if args.predicate_module:

                def _probe(_codec: str) -> tuple[bool, str]:
                    return True, ""

            else:

                def _probe(codec: str) -> tuple[bool, str]:
                    spec = runtime_by_token[codec]
                    return probe_encoder_available(
                        spec.adapter,
                        ffmpeg_bin=spec.ffmpeg_bin,
                        vaapi_device=vaapi_device,
                    )

            sweep = compare_codecs_sweep(
                src=args.src,
                target_vmafs=target_vmafs,
                encoders=encoders,
                parallel=not args.no_parallel,
                max_workers=args.max_workers,
                predicate=sweep_predicate,
                availability_probe=_probe,
                pre_decoded_ref=_pre_decoded_ref,
                row_metadata=_runtime_row_metadata,
            )
            if args.format in _profile_formats:
                outputs = _write_compare_profile_report(args, sweep_report=sweep)
                if outputs:
                    sys.stderr.write(
                        "wrote compare profile report -> "
                        + ", ".join(str(p) for p in outputs)
                        + "\n"
                    )
                any_ok = any(r.ok for r in sweep.rows)
                return 0 if any_ok else 1
            rendered = emit_sweep_report(sweep, format=args.format)
            if args.output is not None:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(rendered, encoding="utf-8")
                sys.stderr.write(f"wrote compare sweep report -> {args.output}\n")
            else:
                sys.stdout.write(rendered)
                if not rendered.endswith("\n"):
                    sys.stdout.write("\n")
            any_ok = any(r.ok for r in sweep.rows)
            return 0 if any_ok else 1

        report = compare_codecs(
            src=args.src,
            target_vmaf=args.target_vmaf,
            encoders=encoders,
            parallel=not args.no_parallel,
            max_workers=args.max_workers,
            predicate=predicate,
            pre_decoded_ref=_pre_decoded_ref,
            row_metadata=_runtime_row_metadata,
        )
        if args.format in _profile_formats:
            outputs = _write_compare_profile_report(args, comparison_report=report)
            if outputs:
                sys.stderr.write(
                    "wrote compare profile report -> " + ", ".join(str(p) for p in outputs) + "\n"
                )
            return 0 if report.best() is not None else 1
        rendered = emit_report(report, format=args.format)
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered, encoding="utf-8")
            sys.stderr.write(f"wrote compare report -> {args.output}\n")
        else:
            sys.stdout.write(rendered)
            if not rendered.endswith("\n"):
                sys.stdout.write("\n")
        return 0 if report.best() is not None else 1
    finally:
        # ADR-0607: delete the shared reference YUV now that all workers
        # have finished. The pool (inside compare_codecs / compare_codecs_sweep)
        # shuts down before we reach this point, so no worker can still be
        # reading the file.
        if _pre_decoded_ref is not None:
            with contextlib.suppress(OSError):
                if _pre_decoded_ref.exists():
                    _pre_decoded_ref.unlink()
                    sys.stderr.write(
                        f"vmaf-tune compare: deleted shared reference YUV "
                        f"({_pre_decoded_ref.name}).\n"
                    )


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


def _write_compare_profile_report(
    args: argparse.Namespace,
    *,
    comparison_report: Any | None = None,
    sweep_report: Any | None = None,
) -> list[Path]:
    """Render compare results through the profile-card renderer."""
    from datetime import datetime, timezone

    from .report import ReportData, render_html, render_markdown

    if comparison_report is None and sweep_report is None:
        raise ValueError("comparison_report or sweep_report is required")

    codec_rows = ()
    sweep_points = ()
    sweep_targets = ()
    if sweep_report is not None:
        rows = [
            r.to_row(target)
            for target, r in zip(sweep_report.row_targets, sweep_report.rows, strict=True)
        ]
        sweep_points = tuple(_sweep_point_from_json(row) for row in rows)
        sweep_targets = tuple(float(t) for t in sweep_report.target_vmafs)
        target_vmaf = float(sweep_targets[0]) if sweep_targets else float(args.target_vmaf)
    else:
        # The XOR-style guard above means ``comparison_report`` is bound
        # whenever ``sweep_report`` is None; the assert tells the
        # type-checker (and guards against a future signature drift).
        assert comparison_report is not None
        codec_rows = tuple(
            _codec_row_from_json(r.to_row(float(comparison_report.target_vmaf)))
            for r in comparison_report.rows
        )
        target_vmaf = float(comparison_report.target_vmaf)

    data = ReportData(
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

    output = getattr(args, "output", None)
    fmt = str(args.format)
    if output is None:
        if fmt == "html":
            rendered = render_html(data)
            sys.stdout.write(rendered)
            if not rendered.endswith("\n"):
                sys.stdout.write("\n")
            return []
        raise ValueError("--format both requires --output PATH")

    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    outputs: list[Path] = []
    if fmt == "both":
        # Write the raw JSON artifact alongside HTML and MD so that
        # downstream tools can re-render or diff two reports without
        # re-running the encode pipeline.
        import json as _json

        json_path = output.with_suffix(".json")
        json_path.write_text(_json.dumps(data.to_dict(), indent=2) + "\n", encoding="utf-8")
        outputs.append(json_path)
    if fmt in ("html", "both"):
        html_path = output if fmt == "html" else output.with_suffix(".html")
        html_path.write_text(render_html(data), encoding="utf-8")
        outputs.append(html_path)
    if fmt in ("markdown", "both"):
        md_path = output if fmt == "markdown" else output.with_suffix(".md")
        md_path.write_text(render_markdown(data), encoding="utf-8")
        outputs.append(md_path)
    return outputs


def _run_compare_crf_sweep(
    args: argparse.Namespace,
    encoders: list[str],
    *,
    runtime_specs: tuple[Any, ...] | None = None,
) -> int:
    """CRF-sweep mode for ``compare --no-bisect`` (ADR-0542).

    Encodes each ``(codec, CRF)`` pair from ``--crf-sweep`` exactly once via
    :func:`vmaftune.bisect._encode_and_score` — no iterative bisect search.
    Emits schema-version-3 JSON with ``mode: "crf_sweep"`` and a ``rows`` list
    (one entry per ``(codec, crf)`` pair).

    ``--target-vmaf`` / ``--target-vmafs`` are parsed but act as label-only
    knobs in this mode (pareto frontier annotation); they do not drive the
    encode loop.
    """
    import os
    import tempfile
    import time
    from concurrent.futures import ThreadPoolExecutor, as_completed

    from .bisect import _encode_and_score
    from .codec_adapters import get_adapter
    from .compare import DEFAULT_VAAPI_DEVICE, probe_encoder_available
    from .encoder_runtime import resolve_encoder_runtime_specs

    # ADR-0601: resolve the VA-API render-node for QSV device init.
    _vaapi_device: str = (
        getattr(args, "vaapi_device", None)
        or os.environ.get("VMAFTUNE_VAAPI_DEVICE", "")
        or DEFAULT_VAAPI_DEVICE
    )
    if runtime_specs is None:
        try:
            runtime_specs = resolve_encoder_runtime_specs(
                encoders,
                ffmpeg_bin=getattr(args, "ffmpeg_bin", "ffmpeg"),
                encoder_ffmpeg_bins=getattr(args, "encoder_ffmpeg_bin", ()),
            )
        except ValueError as exc:
            sys.stderr.write(f"vmaf-tune compare --no-bisect: {exc}\n")
            return 2
    runtime_by_token = {spec.token: spec for spec in runtime_specs}

    # Parse --crf-sweep (required in this mode).
    crf_sweep_raw = getattr(args, "crf_sweep", None)
    if not crf_sweep_raw:
        sys.stderr.write(
            "vmaf-tune compare --no-bisect: --crf-sweep LIST is required. "
            "Example: --crf-sweep 18,23,28,33\n"
        )
        return 2
    try:
        crf_values = [int(c.strip()) for c in crf_sweep_raw.split(",") if c.strip()]
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune compare --no-bisect: invalid --crf-sweep: {exc}\n")
        return 2
    if not crf_values:
        sys.stderr.write("vmaf-tune compare --no-bisect: --crf-sweep produced an empty list\n")
        return 2

    # Resolve source geometry the same way as the bisect path.
    framerate_was_default = getattr(args, "_framerate_was_default", False)
    duration_was_default = getattr(args, "_duration_was_default", False)
    resolved_w, resolved_h, resolved_fr, resolved_dur = _resolve_compare_source_geometry(
        Path(args.src),
        width=args.width,
        height=args.height,
        framerate=args.framerate,
        duration_s=args.duration,
        framerate_was_default=framerate_was_default,
        duration_was_default=duration_was_default,
    )
    if resolved_w is None or resolved_h is None:
        sys.stderr.write("vmaf-tune compare --no-bisect: --width and --height are required.\n")
        return 2

    src_path = Path(args.src)
    # ADR-0613: backend already pre-validated in _run_compare before dispatch
    # to this function; map "auto" → None so the vmaf binary self-selects.
    score_backend = None if args.score_backend in (None, "auto") else args.score_backend

    # Probe encoder availability once per codec (mirrors the bisect path).
    availability: dict[str, tuple[bool, str]] = {}
    for spec in runtime_specs:
        availability[spec.token] = probe_encoder_available(
            spec.adapter,
            ffmpeg_bin=spec.ffmpeg_bin,
            vaapi_device=_vaapi_device,
        )

    def _encode_one(codec: str, crf: int) -> dict:
        spec = runtime_by_token[codec]
        avail, reason = availability[codec]
        if not avail:
            return {
                "codec": codec,
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
        adapter = get_adapter(spec.adapter)
        # ADR-0549: honour --workdir / VMAFTUNE_WORKDIR so the sweep
        # decode artefacts land on a volume with sufficient free space
        # rather than the 8 GB /tmp tmpfs inside the dev-mcp container.
        _cli_workdir = getattr(args, "workdir", None)
        from .bisect import _workdir_parent as _bwp

        _sweep_parent = _cli_workdir if _cli_workdir is not None else _bwp()
        if _sweep_parent is not None:
            _sweep_parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="vmaf-tune-crf-sweep-", dir=_sweep_parent) as _wd:
            result = _encode_and_score(
                src=src_path,
                codec=spec.adapter,
                adapter=adapter,
                preset=args.preset,
                crf=crf,
                width=resolved_w,
                height=resolved_h,
                pix_fmt=args.pix_fmt,
                framerate=resolved_fr,
                duration_s=resolved_dur,
                sample_clip_seconds=getattr(args, "sample_clip_seconds", 0.0),
                vmaf_model=_resolve_vmaf_model(args),
                score_backend=score_backend,
                ffmpeg_bin=spec.ffmpeg_bin,
                vmaf_bin=args.vmaf_bin,
                workdir=Path(_wd),
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

    t0 = time.monotonic()
    rows: list[dict] = []
    cells = [(codec, crf) for codec in encoders for crf in crf_values]

    no_parallel = getattr(args, "no_parallel", False)
    max_workers = getattr(args, "max_workers", None)

    if no_parallel or len(cells) == 1:
        for codec, crf in cells:
            rows.append(_encode_one(codec, crf))
    else:
        n_workers = max_workers if max_workers is not None else len(cells)
        futures = {}
        with ThreadPoolExecutor(max_workers=n_workers) as pool:
            for codec, crf in cells:
                futures[pool.submit(_encode_one, codec, crf)] = (codec, crf)
            # Collect in submission order for deterministic output.
            ordered: dict[tuple[str, int], dict] = {}
            for fut in as_completed(futures):
                key = futures[fut]
                ordered[key] = fut.result()
        for cell in cells:
            rows.append(ordered[cell])

    wall_ms = (time.monotonic() - t0) * 1000.0

    def _nan_to_none(v: object) -> object:
        """Serialise NaN/inf as null for RFC-8259 portability."""
        if isinstance(v, float) and (v != v or v == float("inf") or v == float("-inf")):
            return None
        return v

    rows_clean = [
        {k: (_nan_to_none(v) if isinstance(v, float) else v) for k, v in row.items()}
        for row in rows
    ]

    try:
        import importlib.metadata as _meta

        _tool_version = _meta.version("vmaf-tune")
    except Exception:
        _tool_version = "unknown"

    payload = {
        "schema_version": 3,
        "mode": "crf_sweep",
        "src": str(src_path),
        "crf_sweep": crf_values,
        "target_vmaf": float(args.target_vmaf),
        "tool_version": _tool_version,
        "wall_time_ms": round(wall_ms, 1),
        "rows": rows_clean,
    }
    rendered = json.dumps(payload, indent=2)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
        sys.stderr.write(f"wrote compare crf-sweep report -> {args.output}\n")
    else:
        sys.stdout.write(rendered)
        if not rendered.endswith("\n"):
            sys.stdout.write("\n")
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
    """Phase F — ``vmaf-tune auto`` (ADR-0364 / ADR-0454).

    Runs the Phase F decision tree. Non-smoke mode probes source
    geometry, duration, and HDR metadata before planning; ``--smoke``
    exercises the same composition with synthetic metadata.

    When ``--execute`` is set, the selected plan cell(s) are realised as
    actual FFmpeg encodes followed by libvmaf scores; results land in
    ``--runs-dir/tune_results.jsonl`` (ADR-0454).
    """
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


def _add_fast_args(p: argparse.ArgumentParser) -> None:
    """Wire ``vmaf-tune fast`` user-facing flags onto ``p``.

    The fast-path replaces the grid sweep with a single short probe
    encode per TPE trial plus one final real-encode verify pass at the
    chosen CRF. Flags mirror ``recommend`` where the semantics overlap
    (``--target-vmaf``, ``--encoder``, ``--preset``, source geometry)
    so operators can swap between subcommands without re-learning the
    surface; fast-path-specific knobs (``--n-trials``, ``--crf-min`` /
    ``--crf-max``, ``--proxy-tolerance``, ``--smoke``) sit alongside.
    """
    p.add_argument(
        "--src",
        type=Path,
        default=None,
        help=(
            "source video (raw YUV or any FFmpeg-readable container). "
            "Required for production mode; optional for ``--smoke``."
        ),
    )
    p.add_argument(
        "--width",
        type=int,
        default=0,
        help="raw-YUV reference width (required when ``--src`` is a raw YUV)",
    )
    p.add_argument(
        "--height",
        type=int,
        default=0,
        help="raw-YUV reference height (required when ``--src`` is a raw YUV)",
    )
    p.add_argument("--pix-fmt", default="yuv420p", help="ffmpeg pix_fmt (default yuv420p)")
    p.add_argument("--framerate", type=float, default=24.0, help="reference framerate")
    p.add_argument(
        "--target-vmaf",
        type=float,
        required=True,
        help="quality target on the standard VMAF [0, 100] scale",
    )
    p.add_argument(
        "--encoder",
        default="libx264",
        choices=list(known_codecs()),
        help="codec adapter (must be in ENCODER_VOCAB_V2 for production mode)",
    )
    p.add_argument(
        "--preset",
        default="medium",
        help="encoder preset for the probe + verify encodes (default medium)",
    )
    p.add_argument(
        "--crf-min",
        type=int,
        default=DEFAULT_CRF_LO,
        help=f"minimum CRF in the TPE search range (default {DEFAULT_CRF_LO})",
    )
    p.add_argument(
        "--crf-max",
        type=int,
        default=DEFAULT_CRF_HI,
        help=f"maximum CRF in the TPE search range (default {DEFAULT_CRF_HI})",
    )
    p.add_argument(
        "--n-trials",
        type=int,
        default=None,
        help=(
            f"TPE trial budget. Default: {PROD_N_TRIALS} in production mode, "
            f"{SMOKE_N_TRIALS} in --smoke mode."
        ),
    )
    p.add_argument(
        "--time-budget-s",
        type=int,
        default=300,
        help=(
            "soft wall-clock cap in seconds for the Optuna TPE loop "
            "(default 300; in-flight trials are allowed to finish)"
        ),
    )
    p.add_argument(
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
    p.add_argument(
        "--sample-chunk-seconds",
        type=float,
        default=5.0,
        help=(
            "duration in seconds of the proxy probe-encode slice per TPE trial "
            "(default 5.0). Shorter = faster TPE iterations, longer = more "
            "stable canonical-6 features."
        ),
    )
    p.add_argument(
        "--smoke",
        action="store_true",
        help=(
            "use the deterministic synthetic CRF->VMAF curve; no ffmpeg, no "
            "ONNX, no GPU verify. Intended for CI on hosts without the "
            "[fast] extras."
        ),
    )
    p.add_argument(
        "--score-backend",
        default="auto",
        choices=("auto", *ALL_BACKENDS),
        help=(
            "libvmaf scoring backend for the verify pass (default: auto; "
            "cuda > sycl > hip > cpu). See ``vmaf-tune corpus --help``."
        ),
    )
    p.add_argument(
        "--ffmpeg-bin",
        default="ffmpeg",
        help="path to the ffmpeg binary (default ffmpeg on PATH)",
    )
    p.add_argument(
        "--vmaf-bin",
        default="vmaf",
        help="path to the libvmaf CLI binary (default vmaf on PATH)",
    )
    p.add_argument(
        "--vmaf-model",
        default=DEFAULT_MODEL,
        help="vmaf model version string (default: the fork default model)",
    )
    p.add_argument(
        "--encode-dir",
        type=Path,
        default=Path(".workingdir2/fast"),
        help="scratch dir for probe + verify encodes (default .workingdir2/fast, gitignored)",
    )
    p.add_argument(
        "--output",
        type=Path,
        default=None,
        help="JSON destination for the recommendation payload (default: stdout)",
    )


def _build_fast_sample_extractor(
    args: argparse.Namespace,
    workdir: Path,
) -> Callable[[Path, int, str], tuple[list[float], float]]:
    """Build the production ``sample_extractor`` callable for fast-path.

    The seam encodes a short ``--sample-chunk-seconds`` slice of the
    source at the trial CRF, scores it with libvmaf, and parses the
    canonical-6 (``adm2``, ``vif_scale0..3``, ``motion2``) per-feature
    means out of the libvmaf JSON output. Proxy normalisation
    (StandardScaler) is the proxy module's responsibility — this
    helper returns the raw libvmaf means.
    """
    import json as _json
    import subprocess as _sub
    import tempfile as _tempfile

    from .encode import EncodeRequest, run_encode
    from .score import build_vmaf_command

    workdir.mkdir(parents=True, exist_ok=True)

    def _extract(src: Path, crf: int, encoder: str) -> tuple[list[float], float]:
        # Encode a short probe slice at this CRF.
        slot = workdir / f"probe_{encoder}_crf{crf}.mp4"
        req = EncodeRequest(
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
        encode_result = run_encode(req, ffmpeg_bin=args.ffmpeg_bin)
        if encode_result.exit_status != 0 or not slot.exists():
            return ([0.0] * 6, 0.0)

        size_bytes = encode_result.encode_size_bytes
        observed_kbps = (
            (size_bytes * 8.0 / 1000.0) / max(args.sample_chunk_seconds, 1e-3)
            if size_bytes > 0
            else 0.0
        )

        # Score the slice and parse canonical-6 per-feature means out
        # of libvmaf's per-frame JSON. We bypass score.run_score's
        # pooled-only parser because we need adm2 / vif_scale0..3 /
        # motion2 means rather than the headline VMAF score.
        with _tempfile.TemporaryDirectory(prefix="fast-score-") as score_tmp:
            json_path = Path(score_tmp) / "vmaf.json"
            score_cmd = build_vmaf_command(
                _ScoreReq(
                    reference=src,
                    distorted=slot,
                    width=args.width,
                    height=args.height,
                    pix_fmt=args.pix_fmt,
                    model=_resolve_vmaf_model(args),
                ),
                json_path,
                vmaf_bin=args.vmaf_bin,
                backend=None,  # fast-path proxy is encoder-side; pooled CPU is fine
            )
            completed = _sub.run(score_cmd, capture_output=True, text=True, check=False)
            if completed.returncode != 0 or not json_path.exists():
                return ([0.0] * 6, observed_kbps)
            payload = _json.loads(json_path.read_text(encoding="utf-8"))
            features = _parse_canonical6_means(payload)
        return (features, observed_kbps)

    return _extract


@dataclasses.dataclass(frozen=True)
class _ScoreReq:
    """Minimal duck-typed ScoreRequest for ``build_vmaf_command``.

    ``score.run_score`` is the wrong seam here — we need the per-feature
    means, not just the pooled VMAF score. Reusing ``build_vmaf_command``
    keeps us on the canonical CLI invocation; this duck-type avoids
    importing extras the helper does not need.
    """

    reference: Path
    distorted: Path
    width: int
    height: int
    pix_fmt: str
    model: str
    frame_skip_ref: int = 0
    frame_cnt: int = 0


_CANONICAL_6_KEYS: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)


def _parse_canonical6_means(payload: dict) -> list[float]:
    """Pull canonical-6 per-feature means from libvmaf JSON output.

    Tries ``pooled_metrics.<feature>.mean`` first (modern libvmaf shape),
    falls back to averaging ``frames[].metrics.<feature>`` when only the
    per-frame surface is present. Missing features fill 0.0 — the
    fr_regressor_v2 proxy sees a zero feature rather than NaN, which is
    in-distribution for content where libvmaf's model omits a metric.
    """
    pooled = payload.get("pooled_metrics") or {}
    out: list[float] = []
    frames = payload.get("frames") or []
    for key in _CANONICAL_6_KEYS:
        block = pooled.get(key) or {}
        if "mean" in block:
            out.append(float(block["mean"]))
            continue
        # Per-frame fallback.
        vals: list[float] = []
        for fr in frames:
            metrics = fr.get("metrics") or {}
            if key in metrics:
                vals.append(float(metrics[key]))
        out.append(sum(vals) / len(vals) if vals else 0.0)
    return out


def _build_fast_encode_runner(
    args: argparse.Namespace,
    workdir: Path,
    backend: str,
) -> Callable[[Path, str, int, str], tuple[float, float]]:
    """Build the production ``encode_runner`` callable for the verify pass.

    Runs a single full-clip encode at the recommended CRF and scores it
    via :func:`score.run_score`, returning ``(observed_kbps, vmaf_score)``.
    The verify pass is mandatory — proxy alone never wins
    (ADR-0304 invariant).
    """
    from .encode import EncodeRequest, run_encode
    from .score import ScoreRequest, run_score

    workdir.mkdir(parents=True, exist_ok=True)

    def _runner(src: Path, encoder: str, crf: int, _backend_advisory: str) -> tuple[float, float]:
        slot = workdir / f"verify_{encoder}_crf{crf}.mp4"
        req = EncodeRequest(
            source=src,
            width=args.width,
            height=args.height,
            pix_fmt=args.pix_fmt,
            framerate=args.framerate,
            encoder=encoder,
            preset=args.preset,
            crf=crf,
            output=slot,
        )
        encode_result = run_encode(req, ffmpeg_bin=args.ffmpeg_bin)
        if encode_result.exit_status != 0 or not slot.exists():
            return (0.0, float("nan"))
        # Estimate kbps from size ÷ clip duration.
        # For raw-YUV sources the clip duration is derived from the
        # source file size and frame geometry (frame_bytes × framerate);
        # this is exact and does not depend on wall-clock encode time,
        # which the original code incorrectly used as the denominator.
        size_bytes = encode_result.encode_size_bytes
        framerate = max(args.framerate, 1e-3)
        frame_bytes = args.width * args.height * (
            2 if args.pix_fmt.endswith(("10le", "12le", "16le")) else 1
        ) + (
            (args.width // 2)
            * (args.height // 2)
            * 2
            * (2 if args.pix_fmt.endswith(("10le", "12le", "16le")) else 1)
        )
        src_size = src.stat().st_size if src.exists() and frame_bytes > 0 else 0
        total_frames = src_size // frame_bytes if frame_bytes > 0 and src_size > 0 else 0
        clip_duration_s = total_frames / framerate if total_frames > 0 else 0.0
        observed_kbps = (
            size_bytes * 8.0 / 1000.0 / clip_duration_s
            if size_bytes > 0 and clip_duration_s > 0.0
            else 0.0
        )

        score_req = ScoreRequest(
            reference=src,
            distorted=slot,
            width=args.width,
            height=args.height,
            pix_fmt=args.pix_fmt,
            model=_resolve_vmaf_model(args),
        )
        score_result = run_score(
            score_req,
            vmaf_bin=args.vmaf_bin,
            backend=backend if backend != "cpu" else None,
        )
        return (observed_kbps, float(score_result.vmaf_score))

    return _runner


def _run_fast(args: argparse.Namespace) -> int:
    """Drive ``vmaf-tune fast`` end to end and emit the JSON payload.

    Smoke mode skips ffmpeg / ONNX / GPU entirely and runs the
    synthetic curve so CI on bare hosts still exercises the search
    loop. Production mode wires the canonical-6 sample extractor and
    the real-encode verify runner through the existing
    :mod:`vmaftune.encode` + :mod:`vmaftune.score` pipeline.
    """
    if args.crf_min < 0 or args.crf_max < args.crf_min:
        sys.stderr.write(f"vmaf-tune fast: invalid CRF range [{args.crf_min}, {args.crf_max}]\n")
        return 2

    sample_extractor = None
    encode_runner = None
    backend_for_payload: str | None = None

    if not args.smoke:
        if args.src is None:
            sys.stderr.write("vmaf-tune fast: --src is required in production mode\n")
            return 2
        if args.width <= 0 or args.height <= 0:
            sys.stderr.write(
                "vmaf-tune fast: --width / --height are required in production mode "
                "(raw-YUV geometry)\n"
            )
            return 2
        try:
            backend_for_payload = select_backend(prefer=args.score_backend, vmaf_bin=args.vmaf_bin)
        except BackendUnavailableError as exc:
            sys.stderr.write(f"vmaf-tune fast: {exc}\n")
            return 2
        sys.stderr.write(f"vmaf-tune fast: scoring backend = {backend_for_payload}\n")
        workdir = args.encode_dir
        sample_extractor = _build_fast_sample_extractor(args, workdir / "probes")
        encode_runner = _build_fast_encode_runner(args, workdir / "verify", backend_for_payload)

    try:
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
    except (RuntimeError, ValueError) as exc:
        # fast.fast_recommend raises RuntimeError when Optuna is missing
        # and ValueError for invalid in-process arguments.
        sys.stderr.write(f"vmaf-tune fast: {exc}\n")
        return 2
    except NotImplementedError as exc:
        sys.stderr.write(f"vmaf-tune fast: {exc}\n")
        return 2

    if backend_for_payload is not None:
        result["score_backend"] = backend_for_payload

    rendered = json.dumps(result, indent=2, sort_keys=True)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
        sys.stderr.write(f"wrote fast recommendation -> {args.output}\n")
    else:
        sys.stdout.write(rendered + "\n")

    gap = result.get("proxy_verify_gap")
    if gap is not None and gap > args.proxy_tolerance:
        # OOD signal — caller should fall back to the slow grid.
        return 3
    return 0


def _add_prefilter_args(p: argparse.ArgumentParser) -> None:
    """Wire ``vmaf-tune prefilter`` user-facing flags onto ``p``.

    The prefilter subcommand drives a joint TPE search over the Pelorus
    deband filter's strength knobs (the frozen ADR-0110 contract) + CRF
    with VMAF as the oracle. Source-geometry / encoder / model flags
    mirror ``fast`` so operators can swap between subcommands without
    re-learning the surface.
    """
    p.add_argument(
        "--src",
        type=Path,
        default=None,
        help=(
            "source video (raw YUV or any FFmpeg-readable container). "
            "Required for the live loop; optional for ``--smoke``."
        ),
    )
    p.add_argument(
        "--width",
        type=int,
        default=0,
        help="raw-YUV reference width (required for the live loop on raw YUV)",
    )
    p.add_argument(
        "--height",
        type=int,
        default=0,
        help="raw-YUV reference height (required for the live loop on raw YUV)",
    )
    p.add_argument("--pix-fmt", default="yuv420p", help="ffmpeg pix_fmt (default yuv420p)")
    p.add_argument("--framerate", type=float, default=24.0, help="reference framerate")
    p.add_argument(
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
    p.add_argument(
        "--target-vmaf",
        type=float,
        required=True,
        help="quality target on the standard VMAF [0, 100] scale",
    )
    p.add_argument(
        "--encoder",
        default="libx264",
        choices=list(known_codecs()),
        help="HW/SW codec adapter that performs the post-deband encode (default libx264)",
    )
    p.add_argument(
        "--preset",
        default="medium",
        help="encoder preset for the probe encodes (default medium)",
    )
    p.add_argument(
        "--filter",
        dest="filter_name",
        default="pelorus_deband",
        choices=list(known_filters()),
        help="pre-encode filter adapter to autotune (default pelorus_deband)",
    )
    p.add_argument(
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
    p.add_argument(
        "--crf-min",
        type=int,
        default=PREFILTER_CRF_LO,
        help=f"minimum CRF in the joint TPE search range (default {PREFILTER_CRF_LO})",
    )
    p.add_argument(
        "--crf-max",
        type=int,
        default=PREFILTER_CRF_HI,
        help=f"maximum CRF in the joint TPE search range (default {PREFILTER_CRF_HI})",
    )
    p.add_argument(
        "--n-trials",
        type=int,
        default=None,
        help=(
            f"TPE trial budget. Default: {PREFILTER_N_TRIALS} in the live loop, "
            f"{PREFILTER_SMOKE_N_TRIALS} in --smoke mode."
        ),
    )
    p.add_argument(
        "--time-budget-s",
        type=float,
        default=600.0,
        help="soft wall-clock cap in seconds for the Optuna TPE loop (default 600)",
    )
    p.add_argument(
        "--seed",
        type=int,
        default=0,
        help="TPE sampler seed for a reproducible search (default 0)",
    )
    p.add_argument(
        "--smoke",
        action="store_true",
        help=(
            "use the synthetic deband+CRF surface; no ffmpeg, no Vulkan, no "
            "GPU. Intended for CI on hosts without a pelorus-enabled ffmpeg."
        ),
    )
    p.add_argument(
        "--score-backend",
        default="auto",
        choices=("auto", *ALL_BACKENDS),
        help=(
            "libvmaf scoring backend for the probe scores (default: auto; "
            "cuda > sycl > hip > cpu). vmafx scores the deband output; the "
            "deband filter itself runs in ffmpeg, not in vmafx."
        ),
    )
    p.add_argument("--ffmpeg-bin", default="ffmpeg", help="ffmpeg binary (default ffmpeg on PATH)")
    p.add_argument("--vmaf-bin", default="vmaf", help="libvmaf CLI binary (default vmaf on PATH)")
    p.add_argument(
        "--vmaf-model",
        default=DEFAULT_MODEL,
        help="vmaf model version string (default: the fork default model)",
    )
    _add_neg_flag(p)
    p.add_argument(
        "--encode-dir",
        type=Path,
        default=Path(".workingdir2/prefilter"),
        help="scratch dir for probe encodes (default .workingdir2/prefilter, gitignored)",
    )
    p.add_argument(
        "--output",
        type=Path,
        default=None,
        help="JSON destination for the recommendation payload (default: stdout)",
    )


def _build_prefilter_probe(
    args: argparse.Namespace,
    workdir: Path,
    backend: str,
) -> Callable[[Mapping[str, float], int], ProbeResult]:
    """Build the live ``(deband_params, crf) -> ProbeResult`` probe.

    Each probe call emits the deband ``-vf`` fragment via the filter
    adapter, runs ``[pelorus_deband_vulkan=...] -> encode`` through the
    existing :mod:`vmaftune.encode` driver, scores the encoded output
    against the source with libvmaf, and returns the achieved VMAF +
    bitrate. The deband filter runs inside ffmpeg — vmafx only supplies
    the string and reads the score.
    """
    from .encode import EncodeRequest, bitrate_kbps, run_encode
    from .score import ScoreRequest, run_score

    adapter = get_filter_adapter(args.filter_name)
    workdir.mkdir(parents=True, exist_ok=True)
    is_container = args.src.suffix.lower() not in {".yuv", ".raw", ".y4m", ""}

    def _probe(deband: Mapping[str, float], crf: int) -> ProbeResult:
        fragment = adapter.vf_fragment(deband)
        slot = workdir / f"probe_crf{crf}_{abs(hash(fragment)) & 0xFFFFFF:06x}.mp4"
        req = EncodeRequest(
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
            # The deband fragment is injected as an input filter chain
            # ahead of the encoder via -vf; extra_params is appended
            # after the codec args and before the output (encode.py).
            extra_params=("-vf", fragment),
            source_is_container=is_container,
        )
        encode_result = run_encode(req, ffmpeg_bin=args.ffmpeg_bin)
        if encode_result.exit_status != 0 or not slot.exists():
            # A failed probe scores 0 VMAF so TPE steers away from it.
            return ProbeResult(vmaf=0.0, kbps=0.0, vf_fragment=fragment)
        size_bytes = encode_result.encode_size_bytes
        observed_kbps = bitrate_kbps(size_bytes, args.duration_s) if size_bytes > 0 else 0.0
        score_req = ScoreRequest(
            reference=args.src,
            distorted=slot,
            width=args.width,
            height=args.height,
            pix_fmt=args.pix_fmt,
            model=_resolve_vmaf_model(args),
        )
        score_result = run_score(
            score_req,
            vmaf_bin=args.vmaf_bin,
            backend=backend if backend != "cpu" else None,
        )
        return ProbeResult(
            vmaf=float(score_result.vmaf_score),
            kbps=float(observed_kbps),
            vf_fragment=fragment,
        )

    return _probe


def _run_prefilter(args: argparse.Namespace) -> int:
    """Drive ``vmaf-tune prefilter`` end to end and emit the JSON payload.

    Smoke mode runs the synthetic deband+CRF surface so CI on bare hosts
    exercises the joint search loop. The live loop is gated behind
    :func:`pelorus_filter_available` — the Pelorus Vulkan deband filter
    must be compiled into the ffmpeg build, since vmafx drives it via a
    ``-vf`` string but does not ship the filter itself.
    """
    if args.crf_min < 0 or args.crf_max < args.crf_min:
        sys.stderr.write(
            f"vmaf-tune prefilter: invalid CRF range [{args.crf_min}, {args.crf_max}]\n"
        )
        return 2

    sweep_knobs = tuple(args.sweep_knobs) if args.sweep_knobs else None
    probe = None

    if not args.smoke:
        if args.src is None:
            sys.stderr.write("vmaf-tune prefilter: --src is required for the live loop\n")
            return 2
        if args.width <= 0 or args.height <= 0:
            sys.stderr.write(
                "vmaf-tune prefilter: --width / --height are required for the live "
                "loop (raw-YUV geometry)\n"
            )
            return 2
        if not pelorus_filter_available(args.ffmpeg_bin):
            sys.stderr.write(
                "vmaf-tune prefilter: the Pelorus deband filter "
                "('pelorus_deband_vulkan') is not available in this ffmpeg "
                f"build ({args.ffmpeg_bin}). Build ffmpeg with the Pelorus "
                "Vulkan filter, or use --smoke to exercise the search loop "
                "without a live encode. (ADR-1116 / pelorus ADR-0110)\n"
            )
            return 2
        try:
            backend = select_backend(prefer=args.score_backend, vmaf_bin=args.vmaf_bin)
        except BackendUnavailableError as exc:
            sys.stderr.write(f"vmaf-tune prefilter: {exc}\n")
            return 2
        sys.stderr.write(f"vmaf-tune prefilter: scoring backend = {backend}\n")
        probe = _build_prefilter_probe(args, args.encode_dir / "probes", backend)

    try:
        result = recommend_prefilter(
            src=args.src,
            target_vmaf=args.target_vmaf,
            encoder=args.encoder,
            filter_name=args.filter_name,
            crf_range=(args.crf_min, args.crf_max),
            sweep_knobs=sweep_knobs,
            n_trials=args.n_trials,
            time_budget_s=args.time_budget_s,
            smoke=args.smoke,
            probe=probe,
            seed=args.seed,
        )
    except (RuntimeError, ValueError, KeyError, PelorusFilterUnavailableError) as exc:
        sys.stderr.write(f"vmaf-tune prefilter: {exc}\n")
        return 2

    rendered = json.dumps(result, indent=2, sort_keys=True)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
        sys.stderr.write(f"wrote prefilter recommendation -> {args.output}\n")
    else:
        sys.stdout.write(rendered + "\n")
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


def _run_sidecar(args: argparse.Namespace) -> int:
    """Run the ``vmaf-tune sidecar`` operator surface."""
    try:
        sp = _build_sidecar_predictor(args)
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        sys.stderr.write(f"vmaf-tune sidecar: {exc}\n")
        return 2

    if args.sidecar_cmd == "status":
        _emit_sidecar_status(_sidecar_status_payload(sp), args.json)
        return 0

    if args.sidecar_cmd == "predict":
        try:
            features = _sidecar_features_from_mapping(_read_json_object(args.features_json))
        except ValueError as exc:
            sys.stderr.write(f"vmaf-tune sidecar predict: {exc}\n")
            return 2
        base = sp.predictor.predict_vmaf(features, args.crf, args.codec)
        correction = sp.model.predict_correction(features, args.crf)
        payload = {
            "schema": "vmaf-tune-sidecar-predict/v1",
            "codec": args.codec,
            "crf": args.crf,
            "base_vmaf": base,
            "correction": correction,
            "sidecar_vmaf": sp.predict_vmaf(features, args.crf),
            "n_updates": sp.model.n_updates,
        }
        if args.json:
            sys.stdout.write(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        else:
            sys.stdout.write(
                "base={base_vmaf:.6f} correction={correction:.6f} "
                "sidecar={sidecar_vmaf:.6f} updates={n_updates}\n".format(**payload)
            )
        return 0

    if args.sidecar_cmd == "record":
        try:
            features = _sidecar_features_from_mapping(_read_json_object(args.features_json))
        except ValueError as exc:
            sys.stderr.write(f"vmaf-tune sidecar record: {exc}\n")
            return 2
        base = sp.predictor.predict_vmaf(features, args.crf, args.codec)
        sp.record_capture(
            features,
            crf=args.crf,
            observed_vmaf=args.observed_vmaf,
            persist=not args.no_persist,
        )
        payload = _sidecar_status_payload(sp)
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

    if args.sidecar_cmd == "batch-record":
        rows = 0
        skipped = 0
        try:
            with args.captures_jsonl.open(encoding="utf-8") as fh:
                for lineno, line in enumerate(fh, start=1):
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        row = json.loads(line)
                        if not isinstance(row, dict):
                            raise ValueError("row is not an object")
                        features = _sidecar_features_from_mapping(row)
                        crf = int(row["crf"])
                        observed = float(row["observed_vmaf"])
                    except (
                        KeyError,
                        TypeError,
                        ValueError,
                        json.JSONDecodeError,
                    ) as exc:
                        skipped += 1
                        sys.stderr.write(
                            f"vmaf-tune sidecar batch-record: skip line {lineno}: {exc}\n"
                        )
                        continue
                    sp.record_capture(features, crf=crf, observed_vmaf=observed, persist=False)
                    rows += 1
        except OSError as exc:
            sys.stderr.write(f"vmaf-tune sidecar batch-record: cannot read input: {exc}\n")
            return 2
        if rows:
            sp.save()
        payload = _sidecar_status_payload(sp)
        payload.update(
            {
                "schema": "vmaf-tune-sidecar-batch-record/v1",
                "rows_recorded": rows,
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

    sys.stderr.write(f"vmaf-tune sidecar: unknown subcommand {args.sidecar_cmd!r}\n")
    return 2


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


def _sweep_point_from_json(r: dict[str, Any]) -> CodecSweepPoint:  # type: ignore[name-defined]  # noqa: F821
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


def _codec_row_from_json(r: dict[str, Any]) -> CodecRow:  # type: ignore[name-defined]  # noqa: F821
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


def _run_report(args: argparse.Namespace) -> int:
    """Render a vmaf-tune profile-card from one or more JSON dumps."""
    from datetime import datetime, timezone

    from .compare import detect_schema_version
    from .report import (
        CodecRow,
        CodecSweepPoint,
        LadderRung,
        LadderSample,
        ReportData,
        ShotRow,
        probe_source,
        render_html,
        render_markdown,
    )

    src_info = probe_source(args.src)

    codec_rows: tuple[CodecRow, ...] = ()
    sweep_points: tuple[CodecSweepPoint, ...] = ()
    sweep_targets: tuple[float, ...] = ()
    if args.compare_json is not None:
        try:
            cmp_payload = json.loads(args.compare_json.read_text())
        except (OSError, json.JSONDecodeError) as e:
            sys.stderr.write(f"vmaf-tune report: cannot load --compare-json: {e}\n")
            return 2
        cmp_rows_in = cmp_payload.get("rows") or cmp_payload.get("results") or []
        schema_v = detect_schema_version(cmp_payload)
        if schema_v >= 2:
            # ADR-0516: v2 sweep schema. Build CodecSweepPoint instances
            # and surface the declared target list so the renderer's
            # summary table prints a uniform per-codec / per-target grid
            # even when one codec's row is missing for some target (the
            # "encoder unavailable" or per-target bisect failure cases).
            sweep_points = tuple(_sweep_point_from_json(r) for r in cmp_rows_in)
            sweep_targets = tuple(float(t) for t in cmp_payload.get("target_vmafs") or ())
            if not sweep_targets:
                sweep_targets = tuple(sorted({p.target_vmaf for p in sweep_points}))
        else:
            codec_rows = tuple(_codec_row_from_json(r) for r in cmp_rows_in)

    ladder_samples: tuple[LadderSample, ...] = ()
    ladder_rungs: tuple[LadderRung, ...] = ()
    if args.ladder_json is not None:
        try:
            lp = json.loads(args.ladder_json.read_text())
        except (OSError, json.JSONDecodeError) as e:
            sys.stderr.write(f"vmaf-tune report: cannot load --ladder-json: {e}\n")
            return 2
        for s in lp.get("samples") or lp.get("points") or []:
            ladder_samples = ladder_samples + (
                LadderSample(
                    width=int(s.get("width") or 0),
                    height=int(s.get("height") or 0),
                    bitrate_kbps=(
                        float(s["bitrate_kbps"])
                        if s.get("bitrate_kbps") is not None
                        else float("nan")
                    ),
                    vmaf=(float(s["vmaf"]) if s.get("vmaf") is not None else float("nan")),
                    crf=int(s.get("crf") or 0),
                ),
            )
        for r in lp.get("renditions") or lp.get("rungs") or []:
            ladder_rungs = ladder_rungs + (
                LadderRung(
                    width=int(r.get("width") or 0),
                    height=int(r.get("height") or 0),
                    bitrate_kbps=(
                        float(r["bitrate_kbps"])
                        if r.get("bitrate_kbps") is not None
                        else float("nan")
                    ),
                    vmaf=(float(r["vmaf"]) if r.get("vmaf") is not None else float("nan")),
                    crf=int(r.get("crf") or 0),
                ),
            )

    shots: tuple[ShotRow, ...] = ()
    if args.per_shot_json is not None:
        try:
            sp = json.loads(args.per_shot_json.read_text())
        except (OSError, json.JSONDecodeError) as e:
            sys.stderr.write(f"vmaf-tune report: cannot load --per-shot-json: {e}\n")
            return 2
        for idx, s in enumerate(sp.get("shots") or sp.get("plan") or []):
            # Per-shot tune emits `shot_id` + `frames` (count) + `predicted_crf`
            # — not the bisect-style `index`/`best_crf` fields. Map both shapes
            # so a real `vmaf-tune tune-per-shot` plan ingests without
            # collapsing every shot to "0 kbps / 0s". Compute duration from
            # frame count + source fps when the JSON doesn't carry it; mark
            # missing per-shot vmaf / bitrate as NaN so the renderer shows
            # "—" instead of misleading zeros.
            start_f = int(s.get("start_frame") or s.get("start") or 0)
            end_f = int(s.get("end_frame") or s.get("end") or 0)
            frame_count = int(s.get("frames") or max(0, end_f - start_f))
            fps = float(s.get("framerate") or sp.get("framerate") or src_info.fps or 0.0)
            duration_default = (frame_count / fps) if fps > 0 else 0.0
            shots = shots + (
                ShotRow(
                    shot_index=int(s.get("shot_id", s.get("index", idx))),
                    start_frame=start_f,
                    end_frame=end_f,
                    width=int(s.get("width") or src_info.width),
                    height=int(s.get("height") or src_info.height),
                    best_crf=int(s.get("best_crf") or s.get("crf") or s.get("predicted_crf") or 0),
                    vmaf=float(s.get("vmaf") or s.get("predicted_vmaf") or float("nan")),
                    bitrate_kbps=float(s.get("bitrate_kbps") or float("nan")),
                    duration_s=float(s.get("duration_s") or duration_default),
                ),
            )

    data = ReportData(
        source=src_info,
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

    args.output.parent.mkdir(parents=True, exist_ok=True)
    outputs: list[Path] = []
    if args.format in ("html", "both"):
        html_path = args.output if args.format == "html" else args.output.with_suffix(".html")
        html_path.write_text(render_html(data), encoding="utf-8")
        outputs.append(html_path)
    if args.format in ("markdown", "both"):
        md_path = args.output if args.format == "markdown" else args.output.with_suffix(".md")
        md_path.write_text(render_markdown(data, assets_dir=args.assets_dir), encoding="utf-8")
        outputs.append(md_path)

    # Aggregate row-level ``ok`` flags into the top-level status field
    # so consumers can ``jq .ok`` to decide whether the report is
    # trustworthy without re-scanning every row. The compare-stage
    # bisect can fail per-codec while the report itself succeeds in
    # producing a card — both signals matter, but the previous
    # unconditional ``"ok": true`` masked the per-row failures (Bug #6,
    # BBB e2e 2026-05-17). The semantics: ``ok=true`` iff at least one
    # codec row succeeded AND no row recorded a failure; mirrors the
    # ``ComparisonReport.best()`` definition. With no codec rows at
    # all the report is informational and stays ``ok=true``.
    #
    # ADR-0501 / BBB e2e v4 Bug #V4-C: an "encoder unavailable" row
    # records an infrastructure gap (the codec binary wasn't built
    # into the runtime), not a quality regression. The bisect
    # discriminator in ADR-0498 already labels such rows
    # ``error="encoder unavailable (NAME): …"``. Treat those rows
    # as *informational failures* — they don't gate ``ok`` but they
    # do raise a new ``degraded`` flag so dashboards can surface the
    # missing codec without flipping the run to red. The aggregation:
    # ``ok=true`` when every non-``ok`` row's error starts with
    # ``"encoder unavailable"`` AND at least one row succeeded;
    # ``degraded=true`` when any row is an encoder-unavailable row.
    _UNAVAIL_PREFIX = "encoder unavailable"
    _HW_UNAVAIL_PREFIX = "hardware encoder not available"
    # Treat both v1 ``codec_rows`` and v2 ``sweep_points`` uniformly
    # for the ok / degraded aggregation. v2 (ADR-0513) introduces
    # ``hardware encoder not available: ...`` as a sibling unavailable
    # marker for hardware encoders the operator listed but the host
    # can't run — same semantics as the legacy ``encoder unavailable``
    # marker the bisect produces.
    aggregate_rows: list[Any] = list(codec_rows) + list(sweep_points)
    failed_rows = [r for r in aggregate_rows if not r.ok]
    unavail_rows = [
        r
        for r in failed_rows
        if r.error.startswith(_UNAVAIL_PREFIX) or r.error.startswith(_HW_UNAVAIL_PREFIX)
    ]
    real_failures = [
        r
        for r in failed_rows
        if not (r.error.startswith(_UNAVAIL_PREFIX) or r.error.startswith(_HW_UNAVAIL_PREFIX))
    ]
    rows_any_ok = any(r.ok for r in aggregate_rows) if aggregate_rows else True
    top_ok = bool(rows_any_ok and not real_failures)
    degraded = bool(unavail_rows)
    sys.stdout.write(
        json.dumps(
            {
                "ok": top_ok,
                "degraded": degraded,
                "outputs": [str(p) for p in outputs],
                "codec_rows": len(codec_rows),
                "codec_rows_ok": sum(1 for r in codec_rows if r.ok),
                "codec_rows_failed": sum(1 for r in codec_rows if not r.ok),
                "codec_rows_unavailable": sum(
                    1
                    for r in codec_rows
                    if (not r.ok)
                    and (
                        r.error.startswith(_UNAVAIL_PREFIX)
                        or r.error.startswith(_HW_UNAVAIL_PREFIX)
                    )
                ),
                "sweep_points": len(sweep_points),
                "sweep_points_ok": sum(1 for r in sweep_points if r.ok),
                "sweep_points_failed": sum(1 for r in sweep_points if not r.ok),
                "ladder_samples": len(ladder_samples),
                "ladder_rungs": len(ladder_rungs),
                "shots": len(shots),
            }
        )
        + "\n"
    )
    return 0


def _run_encode_profile(args: argparse.Namespace) -> int:
    """Encode one recommendation from an embedded vmaf-tune profile."""
    from .encode import build_ffmpeg_command, run_encode
    from .encoder_profile import (
        build_encode_request,
        load_profile_payload,
        select_recommendation,
    )

    try:
        profile = load_profile_payload(args.profile)
        recommendation = select_recommendation(
            profile,
            codec=args.codec,
            target_vmaf=args.target_vmaf,
            recommendation_index=args.recommendation_index,
        )
        req = build_encode_request(
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
    except ValueError as exc:
        sys.stderr.write(f"vmaf-tune encode-profile: {exc}\n")
        return 2

    run_meta = profile.get("run") or {}
    ffmpeg_bin = args.ffmpeg_bin or run_meta.get("ffmpeg_bin") or "ffmpeg"
    argv = build_ffmpeg_command(req, ffmpeg_bin=str(ffmpeg_bin))
    if args.dry_run:
        sys.stdout.write(
            json.dumps(
                {
                    "ok": True,
                    "dry_run": True,
                    "profile": str(args.profile),
                    "selected": recommendation,
                    "ffmpeg_argv": argv,
                    "output": str(args.output),
                },
                indent=2,
                sort_keys=True,
            )
            + "\n"
        )
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = run_encode(req, ffmpeg_bin=str(ffmpeg_bin))
    sys.stdout.write(
        json.dumps(
            {
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
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
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


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
