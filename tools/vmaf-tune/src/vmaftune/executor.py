# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Phase F execute mode — drive real encodes + scores for a ``vmaf-tune auto`` plan.

``run_plan`` iterates the ``selected`` cell(s) from an :class:`~vmaftune.auto.AutoPlan`,
runs FFmpeg (via :func:`~vmaftune.encode.run_encode`) per rung, scores each output with
the libvmaf CLI (via :func:`~vmaftune.score.run_score`), and appends rows to a JSONL
results file under ``out_dir/tune_results.jsonl``.

Two additional execution modes extend the base executor (ADR-0588):

* **Per-shot** (``run_plan_per_shot``): detects shot boundaries via
  :func:`~vmaftune.per_shot.detect_shots`, scores each segment independently,
  and reports per-shot VMAF alongside an aggregate weighted by shot length.
* **Saliency** (``run_plan_saliency``): applies saliency-aware encoding via
  :func:`~vmaftune.saliency.saliency_aware_encode` so salient regions receive
  preferential bit allocation; the resulting encode is scored in the same
  encode → score pipeline as the base mode.

Design notes (ADR-0579, ADR-0588):

* Zero new mandatory dependencies — results are written as JSONL, matching the corpus
  path (``corpus.py``). A future polars/pyarrow layer can convert on demand.
* The subprocess boundary is the seam: ``encode_runner`` and ``score_runner`` kwargs
  accept the same mock-runner pattern used throughout the harness so the executor is
  fully testable without FFmpeg or ``vmaf`` binaries.
* Only the ``selected`` cell is executed by default; pass ``execute_all=True`` to run
  every cell in the plan (useful for a post-hoc A/B comparison).
* Per-shot and saliency modes are opt-in via dedicated entry-points rather than flags
  on ``run_plan`` — the call-site intent is explicit and test isolation is simpler.
"""

from __future__ import annotations

import dataclasses
import itertools
import tempfile
import time
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any

from .defaultmodel import DEFAULT_MODEL
from .encode import CODEC_PARAM_FLAGS, EncodeRequest, EncodeResult, run_encode
from .jsonio import dumps_strict
from .score import ScoreRequest, ScoreResult, run_score


@dataclasses.dataclass(frozen=True)
class ExecuteResult:
    """Outcome of one (encode + score) pair for a single plan cell.

    ``cell`` is the original plan dict (a reference to the ``cells`` entry).
    ``encode`` and ``score`` are ``None`` when the respective step was skipped
    (e.g. encode failed and ``score`` was not attempted).
    ``row`` is the flat dict written to the JSONL results file — it is a
    merged view of the cell metadata and the encode/score outcomes.
    """

    cell: dict[str, Any]
    encode: EncodeResult | None
    score: ScoreResult | None
    row: dict[str, Any]


def _cell_to_encode_request(
    cell: dict[str, Any],
    src: Path,
    out_dir: Path,
    *,
    pix_fmt: str,
    width: int,
    height: int,
    framerate: float,
    source_is_container: bool,
) -> EncodeRequest:
    """Build an :class:`EncodeRequest` from an ``AutoPlan`` cell dict."""
    codec = str(cell.get("codec", "libx264"))
    preset = str(cell.get("preset", "medium"))
    crf = int(cell.get("crf", 23))
    cell_index = int(cell.get("cell_index", 0))
    output = out_dir / f"encode_{cell_index:03d}_{codec}_{preset}_crf{crf}.mkv"
    return EncodeRequest(
        source=src,
        width=width,
        height=height,
        pix_fmt=pix_fmt,
        framerate=framerate,
        encoder=codec,
        preset=preset,
        crf=crf,
        output=output,
        source_is_container=source_is_container,
    )


def _make_row(
    cell: dict[str, Any],
    enc: EncodeResult | None,
    sc: ScoreResult | None,
) -> dict[str, Any]:
    """Flatten cell + encode + score outcome into a single results dict."""
    row: dict[str, Any] = {
        "cell_index": cell.get("cell_index"),
        "codec": cell.get("codec"),
        "preset": cell.get("preset"),
        "crf": cell.get("crf"),
        "selected": bool(cell.get("selected", False)),
        "estimated_vmaf": cell.get("estimated_vmaf"),
        "estimated_bitrate_kbps": cell.get("estimated_bitrate_kbps"),
        "prediction_source": cell.get("prediction_source"),
        # Encode outcomes
        "encode_size_bytes": enc.encode_size_bytes if enc else None,
        "encode_time_ms": enc.encode_time_ms if enc else None,
        "encode_exit_status": enc.exit_status if enc else None,
        "ffmpeg_version": enc.ffmpeg_version if enc else None,
        "encoder_version": enc.encoder_version if enc else None,
        "encode_path": str(enc.request.output) if enc else None,
        # Score outcomes
        "vmaf_score": sc.vmaf_score if sc else None,
        "score_time_ms": sc.score_time_ms if sc else None,
        "score_exit_status": sc.exit_status if sc else None,
        "vmaf_binary_version": sc.vmaf_binary_version if sc else None,
    }
    if sc is not None:
        for feat, val in sc.feature_means.items():
            row[f"feature_{feat}_mean"] = val
        for feat, val in sc.feature_stds.items():
            row[f"feature_{feat}_std"] = val
    return row


def _write_jsonl_row(fh: Any, row: dict[str, Any]) -> None:
    """Append one portable JSONL row with non-finite values rendered as null."""
    fh.write(dumps_strict(row, indent=None, sort_keys=True) + "\n")


@dataclasses.dataclass(frozen=True)
class _ExecCtx:
    """Per-run settings shared by the three execution modes."""

    src: Path
    out_dir: Path
    pix_fmt: str
    width: int
    height: int
    framerate: float
    vmaf_model: str
    vmaf_bin: str
    ffmpeg_bin: str
    encode_runner: Callable[..., Any] | None
    score_runner: Callable[..., Any] | None


def _effective_geometry(plan: Any, width: int, height: int) -> tuple[int, int]:
    """Frame size: plan ``source_meta`` fills in a caller left at the defaults."""
    source_meta = plan.metadata.get("source_meta", {})
    eff_width = int(source_meta.get("width", width)) if width == 1920 else width
    eff_height = int(source_meta.get("height", height)) if height == 1080 else height
    return eff_width, eff_height


def _cells_to_run(plan: Any, execute_all: bool) -> list[dict[str, Any]]:
    """Cells to execute: all of them, or only the ``selected`` ones."""
    return [cell for cell in plan.cells if execute_all or bool(cell.get("selected", False))]


def _score_in_tempdir(ctx: _ExecCtx, score_req: ScoreRequest) -> ScoreResult:
    """Run the scorer with a throw-away work directory."""
    with tempfile.TemporaryDirectory() as td:
        return run_score(
            score_req,
            vmaf_bin=ctx.vmaf_bin,
            runner=ctx.score_runner,
            workdir=Path(td),
        )


def _execute_cell(
    ctx: _ExecCtx, cell: dict[str, Any], source_is_container: bool
) -> tuple[EncodeResult | None, ScoreResult | None]:
    """Encode one cell, then score it when the encode succeeded."""
    enc_req = _cell_to_encode_request(
        cell,
        ctx.src,
        ctx.out_dir,
        pix_fmt=ctx.pix_fmt,
        width=ctx.width,
        height=ctx.height,
        framerate=ctx.framerate,
        source_is_container=source_is_container,
    )
    enc: EncodeResult | None = None
    sc: ScoreResult | None = None
    try:
        enc = run_encode(enc_req, ffmpeg_bin=ctx.ffmpeg_bin, runner=ctx.encode_runner)
    except Exception as exc:
        # Encode failure is recorded in the row; scoring is skipped.
        _log(f"executor: encode failed for cell {cell.get('cell_index')}: {exc}")
    if enc is not None and enc.exit_status == 0:
        score_req = ScoreRequest(
            reference=ctx.src,
            distorted=enc_req.output,
            width=ctx.width,
            height=ctx.height,
            pix_fmt=ctx.pix_fmt,
            model=ctx.vmaf_model,
        )
        try:
            sc = _score_in_tempdir(ctx, score_req)
        except Exception as exc:
            _log(f"executor: score failed for cell {cell.get('cell_index')}: {exc}")
    return enc, sc


def run_plan(
    plan: AutoPlan,  # type: ignore[name-defined]  # noqa: F821
    src: Path,
    out_dir: Path,
    *,
    pix_fmt: str = "yuv420p",
    width: int = 1920,
    height: int = 1080,
    framerate: float = 25.0,
    source_is_container: bool = True,
    execute_all: bool = False,
    vmaf_model: str = DEFAULT_MODEL,
    vmaf_bin: str = "vmaf",
    ffmpeg_bin: str = "ffmpeg",
    encode_runner: Callable[..., Any] | None = None,
    score_runner: Callable[..., Any] | None = None,
) -> list[ExecuteResult]:
    """Realise an ``AutoPlan`` by running real encodes and scores.

    ``plan`` is the :class:`~vmaftune.auto.AutoPlan` from
    :func:`~vmaftune.auto.run_auto`; ``src`` the reference path;
    ``out_dir`` receives the encodes and ``tune_results.jsonl`` (created if
    absent). ``width`` / ``height`` / ``framerate`` come from the plan's
    ``metadata.source_meta`` when left at the defaults. With
    ``source_is_container=True`` the encoder lets FFmpeg detect the format.
    Only ``selected`` cells run unless ``execute_all``. ``encode_runner`` and
    ``score_runner`` are ``subprocess.run``-compatible test seams.

    Returns one :class:`ExecuteResult` per executed cell in plan order; the
    JSONL log is written even on partial failure.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    results_path = out_dir / "tune_results.jsonl"
    eff_width, eff_height = _effective_geometry(plan, width, height)
    # fmt: off
    ctx = _ExecCtx(
        src, out_dir, pix_fmt, eff_width, eff_height, framerate,
        vmaf_model, vmaf_bin, ffmpeg_bin, encode_runner, score_runner,
    )
    # fmt: on
    results: list[ExecuteResult] = []
    with results_path.open("a", encoding="utf-8") as fh:
        for cell in _cells_to_run(plan, execute_all):
            enc, sc = _execute_cell(ctx, cell, source_is_container)
            row = _make_row(cell, enc, sc)
            _write_jsonl_row(fh, row)
            fh.flush()
            results.append(ExecuteResult(cell=cell, encode=enc, score=sc, row=row))
    return results


def _log(msg: str) -> None:
    """Write a timestamped line to stderr (no logging dep)."""
    import sys

    ts = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime())
    sys.stderr.write(f"[{ts}] {msg}\n")


# ---------------------------------------------------------------------------
# Per-shot execution mode (ADR-0588)
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class ShotExecuteResult:
    """Outcome of scoring one shot segment in per-shot execution mode.

    ``shot_index`` is the 0-based position in the shot list returned by
    :func:`~vmaftune.per_shot.detect_shots`. ``score`` is ``None`` when
    the shot's temporary encode or score step failed. ``length_frames``
    drives the weighted aggregate in :class:`PerShotPlanResult`.
    """

    shot_index: int
    length_frames: int
    score: ScoreResult | None
    row: dict[str, Any]


@dataclasses.dataclass(frozen=True)
class PerShotPlanResult:
    """Aggregate result of a per-shot execution run for one plan cell.

    ``shot_results`` contains one :class:`ShotExecuteResult` per detected
    shot. ``weighted_vmaf`` is the frame-length-weighted mean of per-shot
    VMAF scores (shots with ``score=None`` are excluded). When all shots
    fail, ``weighted_vmaf`` is ``float('nan')``.
    """

    cell: dict[str, Any]
    shot_results: tuple[ShotExecuteResult, ...]
    weighted_vmaf: float
    row: dict[str, Any]


def _score_one_shot(ctx: _ExecCtx, cell: dict[str, Any], si: int, shot: Any) -> ShotExecuteResult:
    """Encode one shot segment, score it, and build its result row."""
    codec = str(cell.get("codec", "libx264"))
    crf = int(cell.get("crf", 23))
    cell_index = int(cell.get("cell_index", 0))
    seg_out = ctx.out_dir / f"shot_{cell_index:03d}_{si:04d}_{codec}_crf{crf}.mkv"
    enc_req = EncodeRequest(
        source=ctx.src,
        width=ctx.width,
        height=ctx.height,
        pix_fmt=ctx.pix_fmt,
        framerate=ctx.framerate,
        encoder=codec,
        preset=str(cell.get("preset", "medium")),
        crf=crf,
        output=seg_out,
        source_is_container=True,
    )
    sc: ScoreResult | None = None
    try:
        enc = run_encode(enc_req, ffmpeg_bin=ctx.ffmpeg_bin, runner=ctx.encode_runner)
        if enc.exit_status == 0:
            sc = _score_in_tempdir(
                ctx,
                ScoreRequest(
                    reference=ctx.src,
                    distorted=seg_out,
                    width=ctx.width,
                    height=ctx.height,
                    pix_fmt=ctx.pix_fmt,
                    model=ctx.vmaf_model,
                    frame_skip_ref=shot.start_frame,
                    frame_cnt=shot.length,
                ),
            )
    except Exception as exc:
        _log(f"executor per-shot: cell {cell_index} shot {si} failed: {exc}")
    shot_row: dict[str, Any] = {
        "cell_index": cell_index,
        "shot_index": si,
        "shot_start_frame": shot.start_frame,
        "shot_end_frame": shot.end_frame,
        "shot_length_frames": shot.length,
        "codec": codec,
        "crf": crf,
        "vmaf_score": sc.vmaf_score if sc else None,
        "score_exit_status": sc.exit_status if sc else None,
    }
    return ShotExecuteResult(shot_index=si, length_frames=shot.length, score=sc, row=shot_row)


def _weighted_vmaf(shot_results: Sequence[ShotExecuteResult]) -> float:
    """Frame-length-weighted mean VMAF across the shots that scored."""
    total_frames = 0
    weighted_sum = 0.0
    for sr in shot_results:
        if sr.score is not None and not _is_nan(sr.score.vmaf_score):
            total_frames += sr.length_frames
            weighted_sum += sr.score.vmaf_score * sr.length_frames
    return weighted_sum / total_frames if total_frames > 0 else float("nan")


def _per_shot_cell(
    ctx: _ExecCtx,
    cell: dict[str, Any],
    per_shot_bin: str,
    shot_runner: object | None,
) -> PerShotPlanResult:
    """Detect the shots of one cell, score each, and aggregate."""
    from .per_shot import detect_shots  # local import to avoid cycles

    shots = detect_shots(
        ctx.src,
        width=ctx.width,
        height=ctx.height,
        pix_fmt=ctx.pix_fmt,
        per_shot_bin=per_shot_bin,
        runner=shot_runner,
    )
    shot_results = [_score_one_shot(ctx, cell, si, shot) for si, shot in enumerate(shots)]
    weighted_vmaf = _weighted_vmaf(shot_results)
    plan_row: dict[str, Any] = {
        "cell_index": int(cell.get("cell_index", 0)),
        "codec": str(cell.get("codec", "libx264")),
        "crf": int(cell.get("crf", 23)),
        "selected": bool(cell.get("selected", False)),
        "shot_count": len(shots),
        "weighted_vmaf": weighted_vmaf,
    }
    return PerShotPlanResult(
        cell=cell,
        shot_results=tuple(shot_results),
        weighted_vmaf=weighted_vmaf,
        row=plan_row,
    )


def run_plan_per_shot(
    plan: AutoPlan,  # type: ignore[name-defined]  # noqa: F821
    src: Path,
    out_dir: Path,
    *,
    pix_fmt: str = "yuv420p",
    width: int = 1920,
    height: int = 1080,
    framerate: float = 25.0,
    execute_all: bool = False,
    vmaf_model: str = DEFAULT_MODEL,
    vmaf_bin: str = "vmaf",
    ffmpeg_bin: str = "ffmpeg",
    per_shot_bin: str = "vmaf-perShot",
    encode_runner: Callable[..., Any] | None = None,
    score_runner: Callable[..., Any] | None = None,
    shot_runner: object | None = None,
) -> list[PerShotPlanResult]:
    """Execute an ``AutoPlan`` with per-shot VMAF scoring (ADR-0588).

    For each selected plan cell: detect shot boundaries with
    :func:`~vmaftune.per_shot.detect_shots` (a single-shot range when
    ``vmaf-perShot`` is absent), encode and score each shot segment
    independently, and aggregate into a frame-length-weighted mean. Rows are
    appended to ``out_dir/tune_results_per_shot.jsonl`` (``out_dir`` is
    created if absent). ``per_shot_bin`` names the detector and
    ``shot_runner`` is the test seam for its subprocess call, as
    ``encode_runner`` / ``score_runner`` are for the other two.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    results_path = out_dir / "tune_results_per_shot.jsonl"
    eff_width, eff_height = _effective_geometry(plan, width, height)
    # fmt: off
    ctx = _ExecCtx(
        src, out_dir, pix_fmt, eff_width, eff_height, framerate,
        vmaf_model, vmaf_bin, ffmpeg_bin, encode_runner, score_runner,
    )
    # fmt: on
    all_results: list[PerShotPlanResult] = []
    with results_path.open("a", encoding="utf-8") as fh:
        for cell in _cells_to_run(plan, execute_all):
            result = _per_shot_cell(ctx, cell, per_shot_bin, shot_runner)
            _write_jsonl_row(fh, result.row)
            fh.flush()
            all_results.append(result)
    return all_results


# ---------------------------------------------------------------------------
# Saliency execution mode (ADR-0588)
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class SaliencyExecuteResult:
    """Outcome of one saliency-aware (encode + score) pair for a plan cell.

    ``saliency_available`` is ``True`` when the saliency model ran
    successfully. When ``False``, the encoder fell back to a plain encode
    (the :func:`~vmaftune.saliency.saliency_aware_encode` graceful-fallback
    path). ``score`` and ``row`` follow the same shape as
    :class:`ExecuteResult`.
    """

    cell: dict[str, Any]
    encode: EncodeResult | None
    score: ScoreResult | None
    saliency_available: bool
    row: dict[str, Any]


def _saliency_encode(
    ctx: _ExecCtx,
    enc_req: EncodeRequest,
    cell_index: int,
    saliency_model_path: Path | None,
    duration_frames: int,
    session_factory: Any,
) -> tuple[EncodeResult | None, bool]:
    """Saliency-aware encode; returns ``(encode result, saliency applied)``."""
    from .saliency import (
        SaliencyConfig,
        SaliencyUnavailableError,
        saliency_aware_encode,
    )

    try:
        enc = saliency_aware_encode(
            enc_req,
            duration_frames=duration_frames,
            model_path=saliency_model_path,
            config=SaliencyConfig(),
            encode_runner=ctx.encode_runner,
            session_factory=session_factory,
            ffmpeg_bin=ctx.ffmpeg_bin,
        )
        # ``saliency_aware_encode`` returns an EncodeResult even on fallback;
        # the augmented request tells whether saliency actually ran.
        return enc, _saliency_was_applied(enc_req, enc)
    except SaliencyUnavailableError as exc:
        _log(f"executor saliency: unavailable for cell {cell_index}: {exc}")
    except Exception as exc:
        _log(f"executor saliency: encode failed for cell {cell_index}: {exc}")
    return None, False


def _saliency_cell(
    ctx: _ExecCtx,
    cell: dict[str, Any],
    saliency_model_path: Path | None,
    duration_frames: int,
    session_factory: Any,
) -> SaliencyExecuteResult:
    """Run one saliency-aware encode + score for a plan cell."""
    cell_index = int(cell.get("cell_index", 0))
    codec = str(cell.get("codec", "libx264"))
    preset = str(cell.get("preset", "medium"))
    crf = int(cell.get("crf", 23))
    output = ctx.out_dir / f"sal_{cell_index:03d}_{codec}_{preset}_crf{crf}.mkv"
    enc_req = EncodeRequest(
        source=ctx.src,
        width=ctx.width,
        height=ctx.height,
        pix_fmt=ctx.pix_fmt,
        framerate=ctx.framerate,
        encoder=codec,
        preset=preset,
        crf=crf,
        output=output,
        source_is_container=True,
    )
    enc, sal_available = _saliency_encode(
        ctx, enc_req, cell_index, saliency_model_path, duration_frames, session_factory
    )
    sc: ScoreResult | None = None
    if enc is not None and enc.exit_status == 0:
        score_req = ScoreRequest(
            reference=ctx.src,
            distorted=output,
            width=ctx.width,
            height=ctx.height,
            pix_fmt=ctx.pix_fmt,
            model=ctx.vmaf_model,
        )
        try:
            sc = _score_in_tempdir(ctx, score_req)
        except Exception as exc:
            _log(f"executor saliency: score failed for cell {cell_index}: {exc}")
    row: dict[str, Any] = {
        "cell_index": cell_index,
        "codec": codec,
        "preset": preset,
        "crf": crf,
        "selected": bool(cell.get("selected", False)),
        "saliency_available": sal_available,
        "encode_exit_status": enc.exit_status if enc else None,
        "vmaf_score": sc.vmaf_score if sc else None,
        "score_exit_status": sc.exit_status if sc else None,
    }
    return SaliencyExecuteResult(
        cell=cell, encode=enc, score=sc, saliency_available=sal_available, row=row
    )


def run_plan_saliency(
    plan: AutoPlan,  # type: ignore[name-defined]  # noqa: F821
    src: Path,
    out_dir: Path,
    *,
    pix_fmt: str = "yuv420p",
    width: int = 1920,
    height: int = 1080,
    framerate: float = 25.0,
    execute_all: bool = False,
    vmaf_model: str = DEFAULT_MODEL,
    vmaf_bin: str = "vmaf",
    ffmpeg_bin: str = "ffmpeg",
    saliency_model_path: Path | None = None,
    duration_frames: int = 1,
    encode_runner: Callable[..., Any] | None = None,
    score_runner: Callable[..., Any] | None = None,
    session_factory: Any = None,
) -> list[SaliencyExecuteResult]:
    """Execute an ``AutoPlan`` with saliency-weighted encoding (ADR-0588).

    Each selected cell is encoded with
    :func:`~vmaftune.saliency.saliency_aware_encode` (per-codec ROI / qpfile
    injection) and scored in the standard way. Without onnxruntime or the
    model file the encode falls back to a plain one and
    ``saliency_available`` records which path ran. Rows are appended to
    ``out_dir/tune_results_saliency.jsonl``. ``saliency_model_path`` defaults
    to :func:`~vmaftune.saliency.compute_saliency_map`'s model,
    ``duration_frames`` sizes the ROI sidecar and ``session_factory`` is the
    ONNX Runtime test seam.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    results_path = out_dir / "tune_results_saliency.jsonl"
    eff_width, eff_height = _effective_geometry(plan, width, height)
    # fmt: off
    ctx = _ExecCtx(
        src, out_dir, pix_fmt, eff_width, eff_height, framerate,
        vmaf_model, vmaf_bin, ffmpeg_bin, encode_runner, score_runner,
    )
    # fmt: on
    results: list[SaliencyExecuteResult] = []
    with results_path.open("a", encoding="utf-8") as fh:
        for cell in _cells_to_run(plan, execute_all):
            result = _saliency_cell(
                ctx, cell, saliency_model_path, duration_frames, session_factory
            )
            _write_jsonl_row(fh, result.row)
            fh.flush()
            results.append(result)
    return results


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------


def _is_nan(v: float) -> bool:
    """Return ``True`` if ``v`` is ``float('nan')`` (avoids math import)."""
    return v != v


def _saliency_was_applied(original_req: EncodeRequest, enc: EncodeResult) -> bool:
    """Heuristic: true when the encode request the driver saw has ROI params.

    The saliency augment helpers patch ``extra_params`` on a copy of the
    original request.  Because the executor only holds the pre-augmentation
    request, we inspect the result's ``request`` (which is the augmented one
    that ``run_encode`` received) for known ROI token prefixes.
    """
    # ``enc`` is typed non-Optional so the dead-code ``None`` guard is
    # dropped; ``enc.request`` is always present.
    augmented_params = enc.request.extra_params
    if "-qpfile" in augmented_params:
        return True
    # The encoder-parameter options also carry HDR SEI and pass control: only
    # the keys the saliency augment helpers write mark an ROI.
    roi_keys = ("zones=", "qp-file=", "ROIFile=")
    return any(
        flag in CODEC_PARAM_FLAGS and any(key in value for key in roi_keys)
        for flag, value in itertools.pairwise(augmented_params)
    )


__all__ = [
    "ExecuteResult",
    "PerShotPlanResult",
    "SaliencyExecuteResult",
    "ShotExecuteResult",
    "run_plan",
    "run_plan_per_shot",
    "run_plan_saliency",
]
