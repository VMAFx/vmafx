# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Saliency mask materialiser for the Option C ROI-VMAF tool.

Given a reference YUV and a saliency ONNX model, produce a distorted
YUV variant where low-saliency pixels are replaced by the reference's
pixel content. This is the cheap way to make the existing ``vmaf``
binary score "the salient regions only" — every non-salient pixel
scores as a perfect match, so the pooled VMAF ends up dominated by the
saliency-weighted region.

ONNX-Runtime inference is gated on the ``[runtime]`` optional dep and
lazy-imported inside :func:`apply_saliency_mask` so that core unit tests
can run without ``onnxruntime`` installed.

Compose-mode (per-frame mask) is intentionally simple — a hard
threshold with a fade band so the mask boundary is not a sharp step
that would itself contribute to VMAF's edge-sensitive features.
Documented in ``docs/usage/vmaf-roi-score.md`` and in research-0063.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Callable
from pathlib import Path

import numpy as np


@dataclasses.dataclass(frozen=True)
class MaskRequest:
    """Inputs for a saliency-mask materialisation pass."""

    reference: Path
    distorted: Path
    output: Path
    width: int
    height: int
    pix_fmt: str
    saliency_model: Path
    threshold: float = 0.3
    fade: float = 0.1


def apply_saliency_mask(
    req: MaskRequest,
    *,
    inference: Callable[[bytes, int, int], object] | None = None,
) -> Path:
    """Materialise the saliency-masked distorted YUV.

    The contract:

    - reads the reference + distorted YUVs frame-by-frame;
    - runs the saliency ONNX on the **reference** so the mask is
      content-derived, not artefact-derived;
    - blends the distorted Y plane towards the reference Y plane
      outside the salient region;
    - writes the output YUV.

    ``inference`` is the per-frame callable that takes (rgb_bytes,
    width, height) and returns a height x width float mask in
    ``[0, 1]``. Defaults to a lazy-loaded ONNX Runtime session against
    ``req.saliency_model``. The seam exists so unit tests can inject a
    synthetic mask without ORT.

    Returns ``req.output``. Raises ``RuntimeError`` if onnxruntime is
    required but not importable.

    Supports little-endian planar YUV 4:2:0, 4:2:2, and 4:4:4 inputs
    at 8, 10, 12, and 16 bits per component. The saliency mask is
    inferred from the reference Y plane converted to 8-bit RGB and then
    downsampled for chroma planes when needed.
    """
    if inference is None:
        inference = _lazy_onnx_inference(req.saliency_model)

    layout = _layout_for(req.pix_fmt, req.width, req.height)
    frame_size = layout.y_size + (2 * layout.c_size)
    ref_size = req.reference.stat().st_size
    dis_size = req.distorted.stat().st_size
    if ref_size != dis_size:
        raise ValueError("reference and distorted YUV sizes differ: " f"{ref_size} != {dis_size}")
    if frame_size <= 0 or ref_size % frame_size != 0:
        raise ValueError(
            f"input size {ref_size} is not a whole number of {req.pix_fmt} "
            f"{req.width}x{req.height} frames"
        )

    req.output.parent.mkdir(parents=True, exist_ok=True)
    with (
        req.reference.open("rb") as ref_fh,
        req.distorted.open("rb") as dis_fh,
        req.output.open("wb") as out_fh,
    ):
        for _frame_idx in range(ref_size // frame_size):
            ref_frame = ref_fh.read(frame_size)
            dis_frame = dis_fh.read(frame_size)
            if len(ref_frame) != frame_size or len(dis_frame) != frame_size:
                raise ValueError("short read while materialising saliency mask")

            ref_planes = _split_frame(ref_frame, layout)
            dis_planes = _split_frame(dis_frame, layout)
            rgb = _yuv_to_rgb_bytes(ref_planes.y, ref_planes.u, ref_planes.v, layout)
            mask = _coerce_mask(inference(rgb, req.width, req.height), req.width, req.height)
            alpha_y = _mask_to_alpha(mask, req.threshold, req.fade)
            alpha_c = _resize_nearest(alpha_y, layout.chroma_width, layout.chroma_height)

            out_y = _blend_plane(ref_planes.y, dis_planes.y, alpha_y, layout)
            out_u = _blend_plane(ref_planes.u, dis_planes.u, alpha_c, layout)
            out_v = _blend_plane(ref_planes.v, dis_planes.v, alpha_c, layout)
            out_fh.write(out_y.tobytes())
            out_fh.write(out_u.tobytes())
            out_fh.write(out_v.tobytes())

    return req.output


def synthesise_uniform_mask(
    width: int,
    height: int,
    fill: float = 0.5,
) -> list[list[float]]:
    """Return a constant-value mask without numpy.

    Used by the smoke tests to verify the combine-math without pulling
    onnxruntime / numpy into the test path.
    """
    if not (0.0 <= fill <= 1.0):
        raise ValueError(f"fill must be in [0, 1], got {fill!r}")
    if width <= 0 or height <= 0:
        raise ValueError(f"width/height must be positive, got {width}x{height}")
    return [[float(fill)] * int(width) for _ in range(int(height))]


def _lazy_onnx_inference(model_path: Path) -> Callable[[bytes, int, int], object]:
    """Return a per-frame inference callable backed by ORT.

    Imported lazily so the package loads when ``onnxruntime`` is not
    installed (e.g. in the unit-test environment that only pulls the
    pure-Python combine math).
    """
    try:
        import onnxruntime as ort  # type: ignore[import-not-found]
    except ImportError as exc:  # pragma: no cover - exercised by integration tests
        raise RuntimeError(
            "vmaf-roi-score requires onnxruntime for saliency inference; "
            "install with `pip install vmaf-roi-score[runtime]`"
        ) from exc

    # Construct the session eagerly so model-load errors surface here
    # rather than on first frame.
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name
    output_name = session.get_outputs()[0].name

    def _run(rgb_bytes: bytes, width: int, height: int) -> object:
        rgb = np.frombuffer(rgb_bytes, dtype=np.uint8).reshape((height, width, 3))
        x = rgb.astype(np.float32) / 255.0
        mean = np.asarray([0.485, 0.456, 0.406], dtype=np.float32)
        std = np.asarray([0.229, 0.224, 0.225], dtype=np.float32)
        x = ((x - mean) / std).transpose(2, 0, 1)[None, :, :, :]
        return session.run([output_name], {input_name: x})[0]

    return _run


@dataclasses.dataclass(frozen=True)
class _Layout:
    width: int
    height: int
    chroma_width: int
    chroma_height: int
    bit_depth: int

    @property
    def y_size(self) -> int:
        return self.y_samples * self.bytes_per_sample

    @property
    def c_size(self) -> int:
        return self.c_samples * self.bytes_per_sample

    @property
    def y_samples(self) -> int:
        return self.width * self.height

    @property
    def c_samples(self) -> int:
        return self.chroma_width * self.chroma_height

    @property
    def bytes_per_sample(self) -> int:
        return 1 if self.bit_depth <= 8 else 2

    @property
    def dtype(self) -> np.dtype:
        return np.dtype(np.uint8) if self.bytes_per_sample == 1 else np.dtype("<u2")

    @property
    def max_value(self) -> int:
        return (1 << self.bit_depth) - 1


@dataclasses.dataclass(frozen=True)
class _Planes:
    y: np.ndarray
    u: np.ndarray
    v: np.ndarray


def _layout_for(pix_fmt: str, width: int, height: int) -> _Layout:
    if width <= 0 or height <= 0:
        raise ValueError(f"width/height must be positive, got {width}x{height}")
    bit_depth = _bit_depth_for(pix_fmt)
    if bit_depth > 8 and pix_fmt.endswith("be"):
        raise ValueError(
            "vmaf-roi-score mask materialisation supports little-endian "
            f"high-bit-depth YUV only: {pix_fmt}"
        )
    if pix_fmt.startswith("yuv444"):
        return _Layout(width, height, width, height, bit_depth)
    if pix_fmt.startswith("yuv422"):
        return _Layout(width, height, (width + 1) // 2, height, bit_depth)
    if pix_fmt.startswith("yuv420"):
        return _Layout(width, height, (width + 1) // 2, (height + 1) // 2, bit_depth)
    raise ValueError(f"unsupported pix_fmt for mask materialisation: {pix_fmt}")


def _bit_depth_for(pix_fmt: str) -> int:
    if "16" in pix_fmt:
        return 16
    if "12" in pix_fmt:
        return 12
    if "10" in pix_fmt:
        return 10
    return 8


def _split_frame(frame: bytes, layout: _Layout) -> _Planes:
    y_end = layout.y_size
    u_end = y_end + layout.c_size
    y = np.frombuffer(frame[:y_end], dtype=layout.dtype).reshape((layout.height, layout.width))
    u = np.frombuffer(frame[y_end:u_end], dtype=layout.dtype).reshape(
        (layout.chroma_height, layout.chroma_width)
    )
    v = np.frombuffer(frame[u_end:], dtype=layout.dtype).reshape(
        (layout.chroma_height, layout.chroma_width)
    )
    return _Planes(y=y, u=u, v=v)


def _yuv_to_rgb_bytes(y: np.ndarray, u: np.ndarray, v: np.ndarray, layout: _Layout) -> bytes:
    u_full = _resize_nearest(u.astype(np.float32), layout.width, layout.height)
    v_full = _resize_nearest(v.astype(np.float32), layout.width, layout.height)
    y_f = y.astype(np.float32)
    scale = float(1 << max(layout.bit_depth - 8, 0))
    c = y_f - (16.0 * scale)
    d = u_full - (128.0 * scale)
    e = v_full - (128.0 * scale)
    r = ((1.164383 * c) + (1.596027 * e)) / scale
    g = ((1.164383 * c) - (0.391762 * d) - (0.812968 * e)) / scale
    b = ((1.164383 * c) + (2.017232 * d)) / scale
    rgb = np.stack([r, g, b], axis=2)
    return np.clip(np.rint(rgb), 0, 255).astype(np.uint8).tobytes()


def _coerce_mask(mask: object, width: int, height: int) -> np.ndarray:
    arr = np.asarray(mask, dtype=np.float32)
    arr = np.squeeze(arr)
    if arr.shape != (height, width):
        raise ValueError(f"saliency mask shape {arr.shape} does not match {(height, width)}")
    if not np.all(np.isfinite(arr)):
        raise ValueError("saliency mask contains non-finite values")
    return np.clip(arr, 0.0, 1.0)


def _mask_to_alpha(mask: np.ndarray, threshold: float, fade: float) -> np.ndarray:
    if not (0.0 <= threshold <= 1.0):
        raise ValueError(f"threshold must be in [0, 1], got {threshold!r}")
    if fade < 0.0:
        raise ValueError(f"fade must be non-negative, got {fade!r}")
    if fade == 0.0:
        return (mask >= threshold).astype(np.float32)
    return np.clip((mask - threshold) / fade, 0.0, 1.0).astype(np.float32)


def _resize_nearest(src: np.ndarray, width: int, height: int) -> np.ndarray:
    if src.shape == (height, width):
        return src
    y_idx = np.minimum((np.arange(height) * src.shape[0]) // height, src.shape[0] - 1)
    x_idx = np.minimum((np.arange(width) * src.shape[1]) // width, src.shape[1] - 1)
    return src[y_idx[:, None], x_idx[None, :]]


def _blend_plane(
    ref: np.ndarray, dis: np.ndarray, alpha: np.ndarray, layout: _Layout
) -> np.ndarray:
    blended = (ref.astype(np.float32) * (1.0 - alpha)) + (dis.astype(np.float32) * alpha)
    return np.clip(np.rint(blended), 0, layout.max_value).astype(layout.dtype)
