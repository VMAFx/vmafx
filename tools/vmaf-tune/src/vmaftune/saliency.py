# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Saliency-aware ROI tuning — Bucket #2 of the PR #354 audit.

Wires the fork's `saliency_student_v1` ONNX model
(`model/tiny/saliency_student_v1.onnx`, ~113K params, BSD-3-Clause-
Plus-Patent, ADR-0286) into the vmaf-tune harness so callers can hand
encoders a per-MB QP-offset map that biases bits toward salient regions
(faces, focal subjects) and saves bits on background.

Pipeline:

    raw yuv420p ── sample N frames ── YUV->RGB ── ImageNet-normalise
            ── onnxruntime(saliency_student_v1) ── mean saliency mask [0, 1]
            ── per-block reduce ── linear map to QP offsets [-12, +12]
            ── codec ROI sidecar/argv ── ffmpeg

Design notes:

- The saliency model is loaded lazily and is **optional**: callers
  can graceful-fallback to non-saliency encoding when onnxruntime or
  the model file is unavailable. This matches the `vmaf-roi` C
  sidecar's posture (ADR-0247).
- Per-codec helpers reduce the pixel-level map to the encoder's ROI
  unit: 16x16 luma for x264/libaom qpfiles, a spatial mean for x265
  zones, and 64x64 units for SVT-AV1 / VVenC ROI files.
- All numeric kernels (RGB conversion, ImageNet normalisation,
  per-MB reduce, QP-offset clamp) are pure NumPy so the test suite
  can exercise them without onnxruntime installed.
- The subprocess boundary stays the integration seam — encoding
  itself is delegated to ``encode.run_encode`` with augmented
  ``extra_params``.

ADR-0293; companion to ADR-0237 (parent) and ADR-0286
(saliency_student_v1, fork-trained on DUTS-TR).
"""

from __future__ import annotations

import dataclasses
import logging
import math
import os
import tempfile
from collections.abc import Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:  # pragma: no cover - typing only
    import numpy as np

    from .encode import EncodeRequest, EncodeResult

_LOG = logging.getLogger(__name__)

# Default location of the fork-trained student model. The harness
# resolves this path relative to the repo root when the caller does
# not pass an explicit ``model_path``.
DEFAULT_SALIENCY_MODEL_RELPATH = Path("model/tiny/saliency_student_v1.onnx")

# QP offsets are clamped to a window both x264 and x265 accept
# comfortably (per ADR-0247 sidecar convention).
QP_OFFSET_MIN = -12
QP_OFFSET_MAX = 12

# x264 macroblock side in luma samples — fixed by the codec.
X264_MB_SIDE = 16

# ImageNet mean/std the saliency_student_v1 input layer expects
# (matches the C-side `vmaf_tensor_from_rgb_imagenet` helper).
_IMAGENET_MEAN = (0.485, 0.456, 0.406)
_IMAGENET_STD = (0.229, 0.224, 0.225)

# Default frame-sample count when reducing a clip to one aggregate
# saliency mask. Eight frames is the same default the saliency
# scoring extractor uses for per-clip aggregates.
DEFAULT_FRAME_SAMPLES = 8

SALIENCY_AGGREGATORS = ("mean", "ema", "max", "motion-weighted")
DEFAULT_SALIENCY_AGGREGATOR = "mean"
DEFAULT_SALIENCY_EMA_ALPHA = 0.6


class SaliencyUnavailableError(RuntimeError):
    """Raised when saliency inference is requested but onnxruntime or
    the model file is missing. Callers catch this to drop back to
    non-saliency encoding."""


class SaliencyUnsupportedEncoderError(SystemExit):
    """Raised (exit code 2) when ``--saliency`` is requested for a codec that
    has no ROI dispatch implementation and the caller has not opted in to the
    graceful-fallback behaviour via ``--saliency-fallback-plain`` or the
    ``VMAFTUNE_SALIENCY_FALLBACK_OK=1`` environment variable.

    Inherits from :class:`SystemExit` so unhandled propagation through the CLI
    produces a clean exit-2 without a traceback (matches ADR-0498 hard-fail
    posture for requested-but-unavailable features).
    """

    def __init__(self, encoder: str, supported: list[str]) -> None:
        self.encoder = encoder
        self.supported = sorted(supported)
        msg = (
            f"vmaf-tune: saliency ROI is not implemented for encoder {encoder!r}.\n"
            f"Supported encoders: {', '.join(self.supported)}\n"
            "To accept a plain encode without ROI bias, pass --saliency-fallback-plain "
            "or set VMAFTUNE_SALIENCY_FALLBACK_OK=1."
        )
        super().__init__(msg)


@dataclasses.dataclass(frozen=True)
class SaliencyConfig:
    """User-tunable knobs for the saliency-aware encode path."""

    # Per-pixel QP offset applied at peak saliency (1.0). Negative
    # values lower the QP (better quality) in salient regions.
    foreground_offset: int = -4
    # Number of frames sampled across the clip for the aggregate
    # saliency mask. Higher = more stable, slower.
    frame_samples: int = DEFAULT_FRAME_SAMPLES
    # If True, write the qpfile to ``encode_dir`` next to the
    # encoded artifact (gitignored). If False, use a
    # ``tempfile.NamedTemporaryFile`` cleaned up by the OS.
    persist_qpfile: bool = False
    # Temporal reducer for the sampled per-frame saliency masks.
    # ``mean`` preserves the historical aggregate. ``ema`` and ``max``
    # are cheap video-saliency baselines from ADR-0396 Phase 1.
    temporal_aggregator: str = DEFAULT_SALIENCY_AGGREGATOR
    # Current-frame weight for the EMA reducer.
    ema_alpha: float = DEFAULT_SALIENCY_EMA_ALPHA
    # ADR-0546 (saliency-tune-01): when True, an unsupported encoder silently
    # falls back to a plain encode (the pre-ADR-0546 behaviour) with an ERROR
    # log instead of raising SaliencyUnsupportedEncoderError (exit code 2).
    # Set via --saliency-fallback-plain or VMAFTUNE_SALIENCY_FALLBACK_OK=1.
    allow_unsupported_encoder_fallback: bool = False


def _import_numpy() -> Any:
    """Import numpy lazily so the module imports on systems without
    it (the grid-sweep corpus path doesn't need numpy)."""
    try:
        import numpy as np  # noqa: PLC0415  (deliberately lazy)
    except ImportError as exc:  # pragma: no cover - defensive
        raise SaliencyUnavailableError("numpy is required for saliency-aware encoding") from exc
    return np


def _import_onnxruntime() -> Any:
    try:
        import onnxruntime as ort  # noqa: PLC0415
    except ImportError as exc:
        raise SaliencyUnavailableError(
            "onnxruntime is required for saliency-aware encoding; "
            "install with `pip install onnxruntime`"
        ) from exc
    return ort


def _yuv420p_frame_size(width: int, height: int) -> int:
    """Return raw 8-bit yuv420p frame size in bytes."""
    return width * height * 3 // 2


def _read_yuv420p_planes(
    path: Path, frame_index: int, width: int, height: int
) -> tuple["np.ndarray", "np.ndarray", "np.ndarray"]:
    """Read one yuv420p frame as ``uint8`` Y, U, V planes."""
    np = _import_numpy()
    frame_bytes = _yuv420p_frame_size(width, height)
    luma_bytes = width * height
    chroma_width = width // 2
    chroma_height = height // 2
    chroma_bytes = chroma_width * chroma_height
    with path.open("rb") as fh:
        fh.seek(frame_index * frame_bytes)
        buf = fh.read(frame_bytes)
    if len(buf) != frame_bytes:
        raise ValueError(
            f"short read on {path}: wanted {frame_bytes} bytes at frame {frame_index}, got {len(buf)}"
        )
    y_end = luma_bytes
    u_end = y_end + chroma_bytes
    y = np.frombuffer(buf[:y_end], dtype=np.uint8).reshape(height, width)
    u = np.frombuffer(buf[y_end:u_end], dtype=np.uint8).reshape(chroma_height, chroma_width)
    v = np.frombuffer(buf[u_end:], dtype=np.uint8).reshape(chroma_height, chroma_width)
    return y, u, v


def _yuv420p_to_rgb_imagenet(y: "np.ndarray", u: "np.ndarray", v: "np.ndarray") -> "np.ndarray":
    """Convert yuv420p BT.709-limited planes to ImageNet-normalised RGB.

    Returns ``float32 [1, 3, H, W]`` — the NCHW tensor
    `saliency_student_v1` expects.
    """
    np = _import_numpy()
    y_f = (y.astype(np.float32) - 16.0) / 219.0
    u_f = (u.astype(np.float32) - 128.0) / 224.0
    v_f = (v.astype(np.float32) - 128.0) / 224.0
    u_full = np.repeat(np.repeat(u_f, 2, axis=0), 2, axis=1)[: y.shape[0], : y.shape[1]]
    v_full = np.repeat(np.repeat(v_f, 2, axis=0), 2, axis=1)[: y.shape[0], : y.shape[1]]

    r = y_f + (1.5748 * v_full)
    g = y_f - (0.1873 * u_full) - (0.4681 * v_full)
    b = y_f + (1.8556 * u_full)
    rgb = np.stack([r, g, b], axis=0)
    rgb = np.clip(rgb, 0.0, 1.0)
    mean = np.asarray(_IMAGENET_MEAN, dtype=np.float32).reshape(3, 1, 1)
    std = np.asarray(_IMAGENET_STD, dtype=np.float32).reshape(3, 1, 1)
    rgb = (rgb - mean) / std
    return rgb[None, ...]  # [1, 3, H, W]


def _sample_frame_indices(total: int, n: int) -> list[int]:
    """Pick ``n`` evenly-spaced frame indices in ``[0, total)``."""
    if total <= 0:
        return []
    if n <= 1 or total == 1:
        return [0]
    step = max(1, total // n)
    idx = list(range(0, total, step))[:n]
    if not idx:
        idx = [0]
    return idx


def _frame_count(path: Path, width: int, height: int) -> int:
    """Number of yuv420p frames in ``path``."""
    fsize = path.stat().st_size
    return fsize // _yuv420p_frame_size(width, height)


def _validate_temporal_aggregator(temporal_aggregator: str, ema_alpha: float) -> None:
    if temporal_aggregator not in SALIENCY_AGGREGATORS:
        raise ValueError(
            f"temporal_aggregator must be one of {SALIENCY_AGGREGATORS}, "
            f"got {temporal_aggregator!r}"
        )
    if not (0.0 < float(ema_alpha) <= 1.0):
        raise ValueError(f"ema_alpha must be in (0, 1], got {ema_alpha!r}")


def _motion_weight(prev_y: "np.ndarray | None", y: "np.ndarray") -> float:
    """Return a non-zero saliency weight from luma motion energy."""
    if prev_y is None:
        return 1.0
    np = _import_numpy()
    delta = np.abs(y.astype(np.float32) - prev_y.astype(np.float32))
    return max(float(delta.mean() / 255.0), 1.0e-6)


def _pad_to_multiple(tensor: "np.ndarray", multiple: int = 32) -> "tuple[np.ndarray, int, int]":
    """Pad a ``[1, 3, H, W]`` float32 tensor to the next multiple of ``multiple``.

    The ``saliency_student_v1`` UNet encoder-decoder uses skip connections
    that require both spatial dimensions to be exact multiples of 32.
    Sources whose height or width are not multiples of 32 cause a shape
    mismatch inside onnxruntime (encoder feature map cannot be concatenated
    with the decoder up-sampled tensor).

    Returns ``(padded_tensor, orig_H, orig_W)`` so the caller can crop the
    model output back to ``[:, :, :orig_H, :orig_W]`` after inference.
    When no padding is needed (both dims already multiples of ``multiple``),
    the original tensor is returned unchanged with its actual H/W.
    """
    np = _import_numpy()
    _, _, H, W = tensor.shape
    pH = math.ceil(H / multiple) * multiple
    pW = math.ceil(W / multiple) * multiple
    if pH == H and pW == W:
        return tensor, H, W
    padded = np.zeros((1, 3, pH, pW), dtype=np.float32)
    padded[:, :, :H, :W] = tensor
    return padded, H, W


def compute_saliency_map(
    video_path: Path,
    width: int,
    height: int,
    *,
    model_path: Path | None = None,
    frame_samples: int = DEFAULT_FRAME_SAMPLES,
    temporal_aggregator: str = DEFAULT_SALIENCY_AGGREGATOR,
    ema_alpha: float = DEFAULT_SALIENCY_EMA_ALPHA,
    session_factory: Any = None,
) -> "np.ndarray":
    """Run ``saliency_student_v1`` over a sampled subset of frames.

    Returns a ``float32 [H, W]`` aggregate saliency mask in ``[0, 1]``.
    ``temporal_aggregator`` controls how sampled per-frame masks are
    reduced:

    - ``mean``: historical per-pixel arithmetic mean.
    - ``ema``: exponential moving average, current-frame weight
      ``ema_alpha``.
    - ``max``: per-pixel maximum over sampled masks.
    - ``motion-weighted``: per-pixel weighted mean where each frame's
      weight is the mean luma delta from the previous sampled frame.

    ``session_factory`` is the test seam: tests pass a fake that
    returns a stub session object exposing ``.run(...)``.

    Raises :class:`SaliencyUnavailableError` if onnxruntime or the
    model file cannot be loaded.
    """
    np = _import_numpy()
    _validate_temporal_aggregator(temporal_aggregator, ema_alpha)

    # The saliency_student_v1 model uses 8x downsampling in its encoder
    # path; heights not divisible by 8 produce off-by-one tensor shapes
    # that cause a runtime crash inside onnxruntime.  Reject early with a
    # clear message rather than surfacing a cryptic internal error.
    if height % 8 != 0:
        raise ValueError(
            f"compute_saliency_map: height {height} is not divisible by 8. "
            f"The saliency_student_v1 encoder path requires height % 8 == 0. "
            f"Pad or crop the source to the next multiple of 8 (e.g. {((height + 7) // 8) * 8}) "
            f"before calling this function."
        )

    if model_path is None:
        model_path = DEFAULT_SALIENCY_MODEL_RELPATH
    if not Path(model_path).exists():
        raise SaliencyUnavailableError(
            f"saliency model not found: {model_path} (Bucket #2 needs "
            "`saliency_student_v1.onnx` from PR #359 / ADR-0286)"
        )

    if session_factory is None:
        ort = _import_onnxruntime()
        session_factory = lambda p: ort.InferenceSession(  # noqa: E731
            str(p), providers=["CPUExecutionProvider"]
        )
    session = session_factory(model_path)

    nframes = _frame_count(video_path, width, height)
    if nframes <= 0:
        raise ValueError(f"no frames in {video_path} for {width}x{height} yuv420p")
    indices = _sample_frame_indices(nframes, frame_samples)

    accum = np.zeros((height, width), dtype=np.float32)
    max_mask = np.zeros((height, width), dtype=np.float32)
    ema_mask: "np.ndarray | None" = None
    weight_sum = 0.0
    prev_y: "np.ndarray | None" = None
    for fi in indices:
        y, u, v = _read_yuv420p_planes(video_path, fi, width, height)
        tensor = _yuv420p_to_rgb_imagenet(y, u, v)
        # saliency_student_v1 uses a UNet encoder-decoder with skip
        # connections that require H and W to be exact multiples of 32.
        # Pad to the next multiple-of-32 boundary before inference and
        # crop the output mask back to the original spatial dimensions.
        tensor_padded, orig_h, orig_w = _pad_to_multiple(tensor, 32)
        outputs = session.run(None, {"input": tensor_padded})
        # saliency_student_v1 returns NCHW [1, 1, H_pad, W_pad] in [0, 1].
        mask = np.asarray(outputs[0])[0, 0, :orig_h, :orig_w]
        mask = mask.astype(np.float32)
        if temporal_aggregator == "mean":
            accum += mask
        elif temporal_aggregator == "max":
            max_mask = np.maximum(max_mask, mask)
        elif temporal_aggregator == "ema":
            if ema_mask is None:
                ema_mask = mask.copy()
            else:
                ema_mask = (float(ema_alpha) * mask) + ((1.0 - float(ema_alpha)) * ema_mask)
        else:
            weight = _motion_weight(prev_y, y)
            accum += mask * weight
            weight_sum += weight
            prev_y = y.copy()

    if temporal_aggregator == "mean":
        accum /= float(len(indices))
    elif temporal_aggregator == "max":
        accum = max_mask
    elif temporal_aggregator == "ema":
        accum = ema_mask if ema_mask is not None else accum
    else:
        accum /= weight_sum
    # Numerically pin to [0, 1] in case of FP drift on the boundary.
    return np.clip(accum, 0.0, 1.0)


def saliency_to_qp_map(
    mask: "np.ndarray",
    baseline_qp: int,
    foreground_offset: int = -4,
) -> "np.ndarray":
    """Map a saliency mask to a per-pixel QP offset map.

    Convention (matches ``vmaf-roi`` ADR-0247):

    - High saliency (1.0) -> ``foreground_offset`` (typically negative)
    - Low saliency (0.0)  -> ``-foreground_offset`` (background gets
      *higher* QP, saving bits)
    - Neutral (0.5)        -> 0

    Returns ``int32 [H, W]`` of QP offsets clamped to
    ``[QP_OFFSET_MIN, QP_OFFSET_MAX]``. ``baseline_qp`` is *not*
    folded in — the offsets are deltas the encoder applies on top of
    its own rate-control decision.
    """
    np = _import_numpy()
    # Map [0, 1] saliency to [-1, +1] linearly.
    centred = (np.asarray(mask, dtype=np.float32) * 2.0) - 1.0
    # Apply the user-chosen gain. ``foreground_offset`` is signed:
    # -4 means "subtract 4 QP at peak saliency". With ``centred`` in
    # [-1, +1], scaling by ``foreground_offset`` directly produces the
    # desired sign convention:
    #   sal=1.0 -> centred=+1 -> offset=foreground_offset (negative)
    #   sal=0.0 -> centred=-1 -> offset=-foreground_offset (positive)
    #   sal=0.5 -> centred= 0 -> offset=0
    offsets = centred * float(foreground_offset)
    # Clamp to the encoder-accepted band.
    offsets = np.clip(np.round(offsets), QP_OFFSET_MIN, QP_OFFSET_MAX)
    # Bound the API to a known dtype.
    return offsets.astype(np.int32)


def reduce_qp_map_to_blocks(qp_map: "np.ndarray", block: int = X264_MB_SIDE) -> "np.ndarray":
    """Reduce a per-pixel QP-offset map to per-block (mean-rounded).

    x264's ``--qpfile`` is per-MB (16x16). Block-mean keeps offsets
    locally smooth and avoids the encoder rejecting a too-noisy file.
    """
    np = _import_numpy()
    h, w = qp_map.shape
    bh = h // block
    bw = w // block
    if bh == 0 or bw == 0:
        raise ValueError(f"qp_map {h}x{w} smaller than block size {block}x{block}")
    cropped = qp_map[: bh * block, : bw * block]
    reshaped = cropped.reshape(bh, block, bw, block).astype(np.float32)
    means = reshaped.mean(axis=(1, 3))
    return np.clip(np.round(means), QP_OFFSET_MIN, QP_OFFSET_MAX).astype(np.int32)


def write_x264_qpfile(
    block_offsets: "np.ndarray",
    out_path: Path,
    *,
    duration_frames: int = 1,
) -> Path:
    """Write a x264 ``--qpfile`` for an N-frame clip.

    x264 qpfile format: one line per frame, ``frame_index frame_type
    qp``. We emit one frame_index entry per encode-frame so the
    encoder applies the same per-MB pattern across the full clip
    (the saliency mask is a per-clip aggregate). Per-MB granularity
    is delivered via the ``--qpfile`` extension x264 honours since
    r2390 — the 0..bw lines after the frame header carry the MB
    deltas.

    NOTE: this is the conservative fallback that targets the widely
    available x264 ``--qpfile`` syntax. Encoders that prefer a true
    per-MB ROI map (SVT-AV1, x265 ``--qpfile``) consume the same
    block_offsets via a sibling formatter — see ``vmaf-roi``.
    """
    bh, bw = block_offsets.shape
    lines: list[str] = []
    for frame_idx in range(duration_frames):
        # 'I' for the first frame so x264 anchors the GOP; 'P' for the
        # rest. Preset/tune still owns the actual GOP structure.
        kind = "I" if frame_idx == 0 else "P"
        lines.append(f"{frame_idx} {kind} 0")
        for row in range(bh):
            row_offsets = " ".join(str(int(v)) for v in block_offsets[row])
            lines.append(row_offsets)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="ascii")
    return out_path


def augment_extra_params_with_qpfile(base: Sequence[str], qpfile: Path) -> tuple[str, ...]:
    """Return ``base + ('-x264-params', f'qpfile={qpfile}')``.

    Keeping this a pure helper means the encode driver itself stays
    agnostic of saliency.
    """
    return tuple(base) + ("-x264-params", f"qpfile={qpfile}")


def augment_extra_params_with_libaom_qpfile(base: Sequence[str], qpfile: Path) -> tuple[str, ...]:
    """Return ``base + ('-qpfile', str(qpfile))`` for patched libaom-av1.

    The fork's FFmpeg patch stack exposes a shared top-level ``-qpfile``
    AVOption on ``libaom-av1``. Unlike x264's wrapper, libaom does not
    receive the path through an opaque encoder-params key.
    """
    return tuple(base) + ("-qpfile", str(qpfile))


# ---------------------------------------------------------------------------
# x265 zones formatter
# ---------------------------------------------------------------------------


def write_x265_zones_arg(
    block_offsets: "np.ndarray",
    *,
    duration_frames: int = 1,
    fps: float = 24.0,
    zone_duration_frames: int = 0,
) -> str:
    """Produce a ``--zones`` argument string for libx265 from per-block offsets.

    x265's ``--zones`` syntax is a comma-separated list of zone descriptors::

        startfrm,endfrm,b=<float>|q=<int>|qp=<int>

    We derive a *single spatial aggregate* QP offset from the block map
    (the mean rounded to int) and replicate it across the full clip as a
    single zone (``0,<last>,q=<offset>``). A future extension could emit
    per-zone entries by splitting the clip into N temporal chunks; for
    Bucket #2 the per-clip spatial aggregate matches the x264 qpfile
    posture and is sufficient for first-order ROI wins.

    The ``q=`` form adds a QP *delta* on top of the encoder's own
    rate-control decision — the semantics x264 ``--qpfile`` uses.
    Negative delta = higher quality (more bits) in salient regions.

    ``zone_duration_frames`` is reserved for future temporal-zone
    partitioning; pass 0 (default) to emit a single all-frames zone.

    Returns the raw zones string (without the leading ``--zones``).
    """
    np = _import_numpy()
    mean_offset = int(np.clip(np.round(block_offsets.mean()), QP_OFFSET_MIN, QP_OFFSET_MAX))
    last_frame = max(0, duration_frames - 1)
    return f"0,{last_frame},q={mean_offset}"


def augment_extra_params_with_x265_zones(base: Sequence[str], zones_str: str) -> tuple[str, ...]:
    """Return ``base + ('-x265-params', f'zones={zones_str}')``.

    x265 accepts the zones directive through ``-x265-params`` (the same
    opaque key-value channel as the two-pass ``pass=N`` flag).  The
    ``zones=`` key is documented in the libx265 encoder options table
    and forwarded by FFmpeg's ``libx265`` wrapper.
    """
    return tuple(base) + ("-x265-params", f"zones={zones_str}")


# ---------------------------------------------------------------------------
# SVT-AV1 qpoffset map formatter
# ---------------------------------------------------------------------------

# SVT-AV1's qpoffset map uses 64x64 super-block granularity.
SVTAV1_SB_SIDE = 64


def write_svtav1_qpoffset_map(
    block_offsets: "np.ndarray",
    out_path: Path,
    *,
    duration_frames: int = 1,
) -> Path:
    """Write an SVT-AV1 QP-offset map file for an N-frame clip.

    SVT-AV1's ``--qp-file`` format (added in SVT-AV1 1.7) expects a
    plain-text file where each line is a space-separated row of integer
    QP offsets — one row per super-block row, one file per frame.
    Multiple frames are separated by a blank line between each frame's
    block grid.  The same spatial mask is replicated for every frame
    (the saliency mask is a per-clip aggregate).

    Format reference: SVT-AV1 ``Source/App/EncApp/EbAppConfig.c``
    (``read_qp_map_file``, tag ``v2.1.0``, accessed 2026-05-09):

        frame 0 block row 0:  "off0 off1 off2 ...\\n"
        frame 0 block row 1:  "off0 off1 off2 ...\\n"
        ...
        (blank line)
        frame 1 block row 0:  ...

    The ``block_offsets`` array is expected to be at the super-block
    granularity (64x64); callers should call
    :func:`reduce_qp_map_to_blocks` with ``block=SVTAV1_SB_SIDE`` first.
    """
    bh, bw = block_offsets.shape
    frame_lines: list[str] = []
    for row in range(bh):
        frame_lines.append(" ".join(str(int(v)) for v in block_offsets[row]))
    frame_block = "\n".join(frame_lines)

    all_frames = ("\n\n".join(frame_block for _ in range(duration_frames))) + "\n"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(all_frames, encoding="ascii")
    return out_path


def augment_extra_params_with_svtav1_qpmap(base: Sequence[str], qpmap: Path) -> tuple[str, ...]:
    """Return ``base + ('-svtav1-params', f'qp-file={qpmap}')``.

    FFmpeg's ``libsvtav1`` wrapper forwards opaque ``-svtav1-params
    key=value:key=value`` strings to ``EbConfig`` via the public
    ``svt_av1_enc_set_parameter`` API, mirroring the pattern the
    vvenc wrapper uses for ``-vvenc-params``.  The ``qp-file`` key
    is the documented SVT-AV1 config name (``EbAppConfig.c`` v2.1.0).
    """
    return tuple(base) + ("-svtav1-params", f"qp-file={qpmap}")


# ---------------------------------------------------------------------------
# libvvenc ROI formatter
# ---------------------------------------------------------------------------

# VVenC's ROI map uses 64x64 CTU granularity (the VVC CTB size for 1080p+).
VVENC_CTU_SIDE = 64


def write_vvenc_roi_csv(
    block_offsets: "np.ndarray",
    out_path: Path,
    *,
    duration_frames: int = 1,
) -> Path:
    """Write a VVenC ROI-map CSV for an N-frame clip.

    VVenC's ``--roi`` option accepts a CSV file where each row is a
    comma-separated sequence of QP delta values, one row per CTU row,
    frames separated by a blank line.  The format is documented in
    ``vvencapp --help`` (``--roi <file>``) at VVenC tag ``v1.14.0``
    and confirmed against ``source/Lib/apputils/VVEncAppCfg.cpp``
    ``parseROIFile`` at the same tag (accessed 2026-05-09).

    Positive delta = lower quality (save bits). Negative delta = higher
    quality (spend more bits). Convention matches :func:`saliency_to_qp_map`.

    ``block_offsets`` should be at CTU granularity (64x64); callers
    should call :func:`reduce_qp_map_to_blocks` with
    ``block=VVENC_CTU_SIDE`` first.
    """
    bh, bw = block_offsets.shape
    frame_lines: list[str] = []
    for row in range(bh):
        frame_lines.append(",".join(str(int(v)) for v in block_offsets[row]))
    frame_block = "\n".join(frame_lines)

    all_frames = ("\n\n".join(frame_block for _ in range(duration_frames))) + "\n"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(all_frames, encoding="ascii")
    return out_path


def augment_extra_params_with_vvenc_roi(base: Sequence[str], roi_csv: Path) -> tuple[str, ...]:
    """Return ``base + ('-vvenc-params', f'ROIFile={roi_csv}')``.

    VVenC's ROI map file is passed via ``-vvenc-params ROIFile=<path>``
    through FFmpeg's ``libvvenc`` wrapper (``vvenc_set_param`` KV API).
    The ``ROIFile`` key is defined in ``VVEncAppCfg.h`` at tag
    ``v1.14.0`` line 895 (``ROI_FILE``).
    """
    return tuple(base) + ("-vvenc-params", f"ROIFile={roi_csv}")


def _saliency_augment_x264(
    request: "EncodeRequest",
    qp_map: "np.ndarray",
    *,
    duration_frames: int,
    persist: bool,
) -> "EncodeRequest":
    """Return a copy of ``request`` augmented with the x264 qpfile argv.

    Internal helper for :func:`saliency_aware_encode`. Receives the raw
    per-pixel ``int32 [H, W]`` QP-offset map, reduces it to 16×16 MB
    granularity, writes the x264 qpfile, and patches
    ``request.extra_params``.
    """
    block_offsets = reduce_qp_map_to_blocks(qp_map, block=X264_MB_SIDE)
    if persist:
        qpfile = request.output.with_suffix(".qpfile.txt")
        write_x264_qpfile(block_offsets, qpfile, duration_frames=duration_frames)
        return dataclasses.replace(
            request,
            extra_params=augment_extra_params_with_qpfile(request.extra_params, qpfile),
        )
    with tempfile.NamedTemporaryFile(mode="w", suffix=".qpfile.txt", delete=False) as fh:
        qpfile = Path(fh.name)
    write_x264_qpfile(block_offsets, qpfile, duration_frames=duration_frames)
    # The caller must clean up the ephemeral file after the encode.
    return dataclasses.replace(
        request,
        extra_params=augment_extra_params_with_qpfile(request.extra_params, qpfile),
    )


def _saliency_augment_libaom(
    request: "EncodeRequest",
    qp_map: "np.ndarray",
    *,
    duration_frames: int,
    persist: bool,
) -> "EncodeRequest":
    """Return a copy of ``request`` augmented with libaom's qpfile argv.

    The FFmpeg patch stack teaches ``libaom-av1`` to consume the same
    x264-style qpfile saliency.py already emits, mapping each 16x16
    macroblock delta onto libaom's mode-info grid and segment-QP table.
    """
    block_offsets = reduce_qp_map_to_blocks(qp_map, block=X264_MB_SIDE)
    if persist:
        qpfile = request.output.with_suffix(".libaom-qpfile.txt")
        write_x264_qpfile(block_offsets, qpfile, duration_frames=duration_frames)
        return dataclasses.replace(
            request,
            extra_params=augment_extra_params_with_libaom_qpfile(request.extra_params, qpfile),
        )
    with tempfile.NamedTemporaryFile(mode="w", suffix=".libaom-qpfile.txt", delete=False) as fh:
        qpfile = Path(fh.name)
    write_x264_qpfile(block_offsets, qpfile, duration_frames=duration_frames)
    return dataclasses.replace(
        request,
        extra_params=augment_extra_params_with_libaom_qpfile(request.extra_params, qpfile),
    )


def _saliency_augment_x265(
    request: "EncodeRequest",
    qp_map: "np.ndarray",
    *,
    duration_frames: int,
    persist: bool,  # noqa: ARG001 — zones string is argv-only, no file to persist
) -> "EncodeRequest":
    """Return a copy of ``request`` augmented with the x265 zones argv.

    Internal helper for :func:`saliency_aware_encode`. Receives the raw
    per-pixel ``int32 [H, W]`` QP-offset map, reduces it to 16×16 MB
    granularity for the mean calculation, and patches
    ``request.extra_params`` with the zones string. The zones string
    is passed as argv (no file I/O), so ``persist`` has no effect.
    """
    block_offsets = reduce_qp_map_to_blocks(qp_map, block=X264_MB_SIDE)
    zones_str = write_x265_zones_arg(block_offsets, duration_frames=duration_frames)
    return dataclasses.replace(
        request,
        extra_params=augment_extra_params_with_x265_zones(request.extra_params, zones_str),
    )


def _saliency_augment_svtav1(
    request: "EncodeRequest",
    qp_map: "np.ndarray",
    *,
    duration_frames: int,
    persist: bool,
) -> "EncodeRequest":
    """Return a copy of ``request`` augmented with the SVT-AV1 qpmap argv.

    Internal helper for :func:`saliency_aware_encode`. Receives the raw
    per-pixel ``int32 [H, W]`` QP-offset map, reduces it to 64×64
    super-block granularity (SVT-AV1's native ROI-map unit), writes the
    space-separated qpmap file, and patches ``request.extra_params``
    with ``-svtav1-params qp-file=…``.
    """
    sb_offsets = reduce_qp_map_to_blocks(qp_map, block=SVTAV1_SB_SIDE)
    if persist:
        qpmap = request.output.with_suffix(".svtav1-qpmap.txt")
        write_svtav1_qpoffset_map(sb_offsets, qpmap, duration_frames=duration_frames)
        return dataclasses.replace(
            request,
            extra_params=augment_extra_params_with_svtav1_qpmap(request.extra_params, qpmap),
        )
    with tempfile.NamedTemporaryFile(mode="w", suffix=".svtav1-qpmap.txt", delete=False) as fh:
        qpmap = Path(fh.name)
    write_svtav1_qpoffset_map(sb_offsets, qpmap, duration_frames=duration_frames)
    return dataclasses.replace(
        request,
        extra_params=augment_extra_params_with_svtav1_qpmap(request.extra_params, qpmap),
    )


def _saliency_augment_vvenc(
    request: "EncodeRequest",
    qp_map: "np.ndarray",
    *,
    duration_frames: int,
    persist: bool,
) -> "EncodeRequest":
    """Return a copy of ``request`` augmented with the VVenC ROI CSV argv.

    Internal helper for :func:`saliency_aware_encode`. Receives the raw
    per-pixel ``int32 [H, W]`` QP-offset map, reduces it to 64×64 CTU
    granularity (VVenC's native ROI-map unit), writes the
    comma-separated ROI-delta CSV, and patches ``request.extra_params``
    with ``-vvenc-params ROIFile=…``.
    """
    ctu_offsets = reduce_qp_map_to_blocks(qp_map, block=VVENC_CTU_SIDE)
    if persist:
        roi_csv = request.output.with_suffix(".vvenc-roi.csv")
        write_vvenc_roi_csv(ctu_offsets, roi_csv, duration_frames=duration_frames)
        return dataclasses.replace(
            request,
            extra_params=augment_extra_params_with_vvenc_roi(request.extra_params, roi_csv),
        )
    with tempfile.NamedTemporaryFile(mode="w", suffix=".vvenc-roi.csv", delete=False) as fh:
        roi_csv = Path(fh.name)
    write_vvenc_roi_csv(ctu_offsets, roi_csv, duration_frames=duration_frames)
    return dataclasses.replace(
        request,
        extra_params=augment_extra_params_with_vvenc_roi(request.extra_params, roi_csv),
    )


# Maps encoder name → per-codec augment function.
#
# Each function receives the raw per-pixel ``int32 [H, W]`` QP-offset
# map from :func:`saliency_to_qp_map` and is responsible for reducing
# it to the encoder's native ROI-map granularity before writing any
# sidecar file.  The dispatch table is keyed by the FFmpeg encoder name
# that appears in ``EncodeRequest.encoder``.
_SALIENCY_DISPATCH: dict[str, Any] = {
    "libx264": _saliency_augment_x264,
    "libaom-av1": _saliency_augment_libaom,
    "libx265": _saliency_augment_x265,
    "libsvtav1": _saliency_augment_svtav1,
    "libvvenc": _saliency_augment_vvenc,
}


def _cleanup_path_from_extra_params(extra_params: Sequence[str]) -> Path | None:
    """Find an ephemeral ROI sidecar path injected into extra params."""
    params = tuple(extra_params)
    for idx in range(len(params) - 1, -1, -1):
        token = params[idx]
        if idx > 0 and params[idx - 1] == "-qpfile":
            candidate = Path(token)
            if candidate.exists():
                return candidate
        if "=" in token:
            candidate = Path(token.split("=", 1)[-1])
            if candidate.exists():
                return candidate
    return None


def saliency_aware_encode(
    request: "EncodeRequest",
    *,
    duration_frames: int,
    model_path: Path | None = None,
    config: SaliencyConfig | None = None,
    encode_runner: Any = None,
    session_factory: Any = None,
    ffmpeg_bin: str = "ffmpeg",
) -> "EncodeResult":
    """Drive a single saliency-aware encode end-to-end.

    Dispatches to the appropriate per-codec ROI channel based on
    ``request.encoder``:

    - ``libx264``  — ASCII ``--qpfile`` at 16×16 macroblock granularity.
    - ``libaom-av1`` — patched FFmpeg ``-qpfile <path>`` bridge at
      16×16 macroblock granularity.
    - ``libx265``  — ``--zones`` QP delta (per-clip spatial mean) via
      ``-x265-params zones=0,N,q=<delta>``.
    - ``libsvtav1`` — space-separated QP-offset map at 64×64 super-block
      granularity via ``-svtav1-params qp-file=…``.
    - ``libvvenc``  — comma-separated ROI-delta CSV at 64×64 CTU
      granularity via ``-vvenc-params ROIFile=…``.

    For encoders not listed above the function raises
    :class:`SaliencyUnsupportedEncoderError` (exit code 2) unless the
    caller has set ``SaliencyConfig.allow_unsupported_encoder_fallback=True``
    (``--saliency-fallback-plain``) or ``VMAFTUNE_SALIENCY_FALLBACK_OK=1``.
    In the opt-in fallback path the call is demoted to a plain encode and an
    ERROR is emitted (ADR-0546 / saliency-tune-01).

    Steps:

    1. Run :func:`compute_saliency_map` over the source.
    2. Map -> per-pixel QP offsets via :func:`saliency_to_qp_map`.
    3. Dispatch to the encoder-specific augment helper (each helper
       reduces the pixel-level map to its native ROI-map granularity).
    4. Delegate to :func:`encode.run_encode` (or the injected runner).

    Falls back to a plain encode (with a WARNING) if onnxruntime or the
    model file is unavailable, so callers always get a result in that case.
    """
    from .encode import run_encode  # local import to avoid cycles

    cfg = config or SaliencyConfig()
    runner = encode_runner

    try:
        mask = compute_saliency_map(
            request.source,
            request.width,
            request.height,
            model_path=model_path,
            frame_samples=cfg.frame_samples,
            temporal_aggregator=cfg.temporal_aggregator,
            ema_alpha=cfg.ema_alpha,
            session_factory=session_factory,
        )
    except SaliencyUnavailableError as exc:
        _LOG.warning("saliency unavailable, falling back to plain encode: %s", exc)
        return run_encode(request, ffmpeg_bin=ffmpeg_bin, runner=runner)

    # Pixel-level QP-offset map in int32 [H, W]; each dispatch helper
    # reduces this to its encoder's native granularity.
    qp_map = saliency_to_qp_map(
        mask, baseline_qp=request.crf, foreground_offset=cfg.foreground_offset
    )

    augment_fn = _SALIENCY_DISPATCH.get(request.encoder)
    if augment_fn is None:
        # ADR-0546 (saliency-tune-01): hard-fail by default (exit 2).  The
        # caller may opt in to the pre-ADR-0546 graceful-fallback behaviour
        # via SaliencyConfig.allow_unsupported_encoder_fallback=True
        # (--saliency-fallback-plain) or the env override.
        env_override = os.environ.get("VMAFTUNE_SALIENCY_FALLBACK_OK", "").strip()
        allow_fallback = cfg.allow_unsupported_encoder_fallback or env_override == "1"
        if not allow_fallback:
            raise SaliencyUnsupportedEncoderError(request.encoder, list(_SALIENCY_DISPATCH))
        _LOG.error(
            "saliency ROI not implemented for encoder %r; falling back to plain encode "
            "(--saliency-fallback-plain / VMAFTUNE_SALIENCY_FALLBACK_OK=1 is set). "
            "Supported encoders: %s",
            request.encoder,
            sorted(_SALIENCY_DISPATCH),
        )
        return run_encode(request, ffmpeg_bin=ffmpeg_bin, runner=runner)

    augmented = augment_fn(
        request,
        qp_map,
        duration_frames=duration_frames,
        persist=cfg.persist_qpfile,
    )

    # For ephemeral (non-persisted) ROI files we must clean up after the
    # encode. The augmented extra_params carries the file path injected
    # by the augment helper in the last ``key=<path>`` slot.
    if not cfg.persist_qpfile:
        cleanup_path = _cleanup_path_from_extra_params(augmented.extra_params)
        try:
            return run_encode(augmented, ffmpeg_bin=ffmpeg_bin, runner=runner)
        finally:
            if cleanup_path is not None:
                try:
                    cleanup_path.unlink(missing_ok=True)
                except OSError:  # pragma: no cover - best effort cleanup
                    pass
    else:
        return run_encode(augmented, ffmpeg_bin=ffmpeg_bin, runner=runner)


__all__ = [
    "DEFAULT_FRAME_SAMPLES",
    "DEFAULT_SALIENCY_AGGREGATOR",
    "DEFAULT_SALIENCY_EMA_ALPHA",
    "DEFAULT_SALIENCY_MODEL_RELPATH",
    "QP_OFFSET_MAX",
    "QP_OFFSET_MIN",
    "SVTAV1_SB_SIDE",
    "VVENC_CTU_SIDE",
    "X264_MB_SIDE",
    "SaliencyConfig",
    "SaliencyUnavailableError",
    "SaliencyUnsupportedEncoderError",
    "SALIENCY_AGGREGATORS",
    "augment_extra_params_with_libaom_qpfile",
    "augment_extra_params_with_qpfile",
    "augment_extra_params_with_svtav1_qpmap",
    "augment_extra_params_with_vvenc_roi",
    "augment_extra_params_with_x265_zones",
    "compute_saliency_map",
    "reduce_qp_map_to_blocks",
    "saliency_aware_encode",
    "saliency_to_qp_map",
    "write_svtav1_qpoffset_map",
    "write_vvenc_roi_csv",
    "write_x264_qpfile",
    "write_x265_zones_arg",
    # Dispatch table — exported so test stubs can assert the encoder is wired.
    "_SALIENCY_DISPATCH",
    # Padding helper — exported so tests can verify round-trip crop correctness.
    "_pad_to_multiple",
]
