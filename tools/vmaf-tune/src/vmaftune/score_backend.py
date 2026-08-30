# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Backend selection for the libvmaf CLI used by `vmaf-tune`.

`vmaf` exposes a unified ``--backend NAME`` selector
(values: ``auto|cpu|cuda|sycl|hip``) per ADR-0127 / ADR-0175 /
ADR-0422 / ADR-0726 (Vulkan backend dropped 2026-05-28).
The selector engages the GPU dispatch in libvmaf and gives a
~10-30x speedup on the score axis at 1080p relative to the CPU path.

This module turns user intent (``--score-backend cuda|sycl|hip|cpu|auto``)
into a concrete, validated choice by intersecting:

1. What the **vmaf binary** advertises in its ``--help`` output (the
   `--backend` line lists which values are recognised);
2. What the **host hardware / runtime** actually offers, probed via
   cheap external tools (``nvidia-smi``, ``sycl-ls``,
   ``rocminfo`` / ``rocm-smi``) with conservative fallbacks.

Hard rules (per task spec):

- ``--score-backend cuda`` on a host without CUDA must FAIL with a
  clear error. We never silently fall back when the user explicitly
  requested a backend.
- Only ``auto`` walks the fallback chain. The default chain is
  ``cuda -> sycl -> hip -> cpu``, picking the first that is both
  binary-supported and hardware-available.

NRProxyBackend (ADR-0624 / ADR-0615)
-------------------------------------
``NRProxyBackend`` wraps ``model/tiny/nr_metric_v1.onnx`` and provides a
lightweight, no-reference MOS proxy score (~200 ms per shot on CPU,
<50 ms on GPU EP).  The sidecar calibration maps that raw model output
to a VMAF-scale proxy before ``--fast-nr`` compares it with the target
VMAF, cutting bisect wall-time 2–4×.

The ``δ_fast`` threshold (``calibration_threshold`` in the model sidecar
JSON) is calibrated against the Netflix corpus by
``ai/scripts/calibrate_nr_threshold.py``.  During bisect:

- If ``|calibrated_NR_VMAF - target| > δ_fast`` — proceed in the NR-implied
  direction without paying the FR cost.
- If ``|calibrated_NR_VMAF - target| ≤ δ_fast`` — fall through to full FR.
- Final accepted CRF always gets a FR confirmation call.
"""

from __future__ import annotations

import dataclasses
import json
import logging
import shutil
import subprocess
from collections.abc import Sequence
from pathlib import Path
from typing import Any, NamedTuple

_log = logging.getLogger(__name__)

#: Backends the vmaf CLI accepts via ``--backend NAME``.
ALL_BACKENDS: tuple[str, ...] = ("cpu", "cuda", "sycl", "hip")

#: Default fallback chain for ``auto``. Native vendor backends are preferred
#: in vendor-priority order on their respective silicon. CPU is the
#: always-available floor.
DEFAULT_FALLBACKS: tuple[str, ...] = ("cuda", "sycl", "hip", "cpu")


class BackendUnavailableError(RuntimeError):
    """User explicitly requested a backend the host cannot provide.

    Raised when ``select_backend(prefer=X)`` is called with a
    non-``auto`` ``X`` and either the local ``vmaf`` binary lacks
    ``X`` support or the host hardware does not advertise it.
    Never raised by ``auto``-mode selection — that path falls back.
    """


@dataclasses.dataclass(frozen=True)
class BackendProbe:
    """One probe outcome (per backend) used by `detect_available_backends`."""

    name: str
    binary_supports: bool
    hardware_available: bool

    @property
    def usable(self) -> bool:
        return self.binary_supports and self.hardware_available


def _vmaf_help(vmaf_bin: str, runner: object | None = None) -> str:
    """Return the vmaf ``--help`` output (stderr+stdout joined).

    Returns empty string on any error so probe logic degrades to
    "binary doesn't support GPU backends" rather than raising.
    """
    runner_fn = runner or subprocess.run
    if shutil.which(vmaf_bin) is None and "/" not in vmaf_bin:
        return ""
    try:
        completed = runner_fn(  # type: ignore[operator]
            [vmaf_bin, "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    out = getattr(completed, "stdout", "") or ""
    err = getattr(completed, "stderr", "") or ""
    return f"{out}\n{err}"


def parse_supported_backends(help_text: str) -> frozenset[str]:
    """Extract the backends the vmaf binary advertises from `--help`.

    The fork's CLI prints a line like::

        --backend $name:              exclusive backend selector — auto|cpu|cuda|sycl|hip.

    We parse the alternation (``a|b|c``) and intersect it with
    `ALL_BACKENDS`. ``cpu`` is added unconditionally — every build
    has a CPU path, even if the help line is missing.

    Returns a frozenset for cheap membership tests.
    """
    found: set[str] = {"cpu"}
    for backend in ALL_BACKENDS:
        # Look for the exact token surrounded by | or whitespace as
        # a robust check; matches any of: auto|cpu|cuda|sycl|hip
        # without false-positives on substrings (e.g. "cuda" inside
        # a comment about CUDA).
        for needle in (f"|{backend}|", f"|{backend}.", f"|{backend}\n", f"|{backend} "):
            if needle in help_text:
                found.add(backend)
                break
    return frozenset(found)


def _probe_cuda(runner: object | None = None) -> bool:
    """True if a CUDA device is reachable. Tries `nvidia-smi -L`."""
    if shutil.which("nvidia-smi") is None:
        return False
    runner_fn = runner or subprocess.run
    try:
        completed = runner_fn(  # type: ignore[operator]
            ["nvidia-smi", "-L"],
            capture_output=True,
            text=True,
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    rc = int(getattr(completed, "returncode", 1))
    out = getattr(completed, "stdout", "") or ""
    return rc == 0 and "GPU" in out


def _probe_sycl(runner: object | None = None) -> bool:
    """True if a SYCL device is reachable. Tries `sycl-ls`."""
    if shutil.which("sycl-ls") is None:
        return False
    runner_fn = runner or subprocess.run
    try:
        completed = runner_fn(  # type: ignore[operator]
            ["sycl-ls"],
            capture_output=True,
            text=True,
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    rc = int(getattr(completed, "returncode", 1))
    out = getattr(completed, "stdout", "") or ""
    # sycl-ls prints one line per device, prefixed with bracketed
    # backend tokens: "[opencl:gpu]", "[ext_oneapi_level_zero:gpu]", ...
    return rc == 0 and "[" in out and ":gpu" in out.lower()


def _probe_hip(runner: object | None = None) -> bool:
    """True if an AMD ROCm/HIP GPU is reachable."""
    runner_fn = runner or subprocess.run
    if shutil.which("rocminfo") is not None:
        try:
            completed = runner_fn(  # type: ignore[operator]
                ["rocminfo"],
                capture_output=True,
                text=True,
                check=False,
                timeout=5,
            )
        except (OSError, subprocess.SubprocessError):
            completed = None
        if completed is not None:
            rc = int(getattr(completed, "returncode", 1))
            out = f"{getattr(completed, 'stdout', '') or ''}\n{getattr(completed, 'stderr', '') or ''}"
            if rc == 0 and "gfx" in out.lower():
                return True

    if shutil.which("rocm-smi") is None:
        return False
    try:
        completed = runner_fn(  # type: ignore[operator]
            ["rocm-smi", "--showproductname"],
            capture_output=True,
            text=True,
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    rc = int(getattr(completed, "returncode", 1))
    out = f"{getattr(completed, 'stdout', '') or ''}\n{getattr(completed, 'stderr', '') or ''}"
    text_lower = out.lower()
    return rc == 0 and ("gpu" in text_lower or "card series" in text_lower)


def detect_available_backends(
    *,
    vmaf_bin: str = "vmaf",
    runner: object | None = None,
) -> list[str]:
    """Return backends usable on this host, in `ALL_BACKENDS` order.

    "Usable" means both:
      - the local ``vmaf`` binary advertises ``--backend NAME`` support, and
      - the corresponding hardware/runtime probe succeeds.

    CPU is always present (every libvmaf build has a CPU path).
    """
    help_text = _vmaf_help(vmaf_bin, runner=runner)
    supported = parse_supported_backends(help_text)

    probes = {
        "cpu": True,
        "cuda": _probe_cuda(runner=runner) if "cuda" in supported else False,
        "sycl": _probe_sycl(runner=runner) if "sycl" in supported else False,
        "hip": _probe_hip(runner=runner) if "hip" in supported else False,
    }
    return [b for b in ALL_BACKENDS if b in supported and probes[b]]


def select_backend(
    prefer: str = "auto",
    *,
    fallbacks: Sequence[str] = DEFAULT_FALLBACKS,
    available: Sequence[str] | None = None,
    vmaf_bin: str = "vmaf",
    runner: object | None = None,
) -> str:
    """Pick a backend honouring user preference and host capability.

    - ``prefer="auto"`` walks ``fallbacks`` and returns the first
      entry present in ``available``. ``cpu`` must be in the chain
      (or in ``available``) to guarantee a result.
    - Any other ``prefer`` value (``cpu``, ``cuda``, ``sycl``,
      ``hip``) is honoured **strictly**: if it is not in
      ``available``, raise `BackendUnavailableError`. Never
      silently falls back — that would mask hardware/build mismatches
      and lie to the operator about wall-clock expectations.

    ``available`` defaults to ``detect_available_backends(...)``;
    tests inject a literal list to keep the unit boundary tight.
    """
    if prefer not in {"auto", *ALL_BACKENDS}:
        raise ValueError(
            f"unknown backend {prefer!r}; expected one of: " f"auto, {', '.join(ALL_BACKENDS)}"
        )

    if available is None:
        available = detect_available_backends(vmaf_bin=vmaf_bin, runner=runner)

    if prefer == "auto":
        for candidate in fallbacks:
            if candidate in available:
                return candidate
        # Last-ditch: cpu is universally available even if probes failed.
        return "cpu"

    if prefer in available:
        return prefer

    # Strict-mode failure — never silently downgrade.
    raise BackendUnavailableError(
        f"backend {prefer!r} requested but not available on this host "
        f"(available: {', '.join(available) or 'cpu'}). "
        f"Check that the local vmaf binary was built with the matching "
        f"backend support and the corresponding runtime/driver is installed."
    )


# ---------------------------------------------------------------------------
# NRProxyBackend (ADR-0624 / ADR-0615)
# ---------------------------------------------------------------------------

#: Default δ_fast used when the model sidecar JSON lacks a
#: ``calibration_threshold`` field (e.g. when no calibration sweep has been
#: run yet).  8 VMAF units corresponds to the ADR-0615 design default and
#: covers >95 % of in-domain content correctly per the Research-0611
#: calibration plan.
NR_PROXY_DEFAULT_DELTA_FAST: float = 8.0
NR_PROXY_DEFAULT_CALIBRATION_SLOPE: float = 20.0
NR_PROXY_DEFAULT_CALIBRATION_INTERCEPT: float = 0.0

#: Input spatial resolution used by NRProxyBackend when resizing frames for
#: nr_metric_v1.onnx (trained on 224×224 grayscale).
NR_MODEL_INPUT_HW: int = 224


class NRProbeResult(NamedTuple):
    """Result of a single NR proxy score call."""

    nr_score: float
    """Raw NR model score, normally MOS-like in [1, 5]."""
    from_cache: bool
    """True when the result was served from the per-bisect cache."""


class NRProxyBackendError(RuntimeError):
    """Raised when the NR proxy backend cannot complete a scoring request.

    Distinct from ``BackendUnavailableError`` (which is about FR backend
    availability).  This error covers: missing ONNX model file, missing
    onnxruntime package, failed inference, or out-of-range output.
    """


@dataclasses.dataclass
class NRProxyBackend:
    """No-reference MOS proxy backend using ``nr_metric_v1.onnx``.

    Wraps ``model/tiny/nr_metric_v1.onnx`` via onnxruntime and exposes
    ``score_nr(distorted_yuv_path, geometry)`` returning a raw NR model
    value (~200 ms on CPU EP, <50 ms on GPU EP).

    Results are cached per (distorted_path, width, height) so repeated
    calls within the same bisect run are free.  The cache is per-instance;
    callers that want cross-bisect caching must share the instance.

    Parameters
    ----------
    model_path:
        Path to ``nr_metric_v1.onnx``.  Defaults to the in-tree location
        at ``<repo_root>/model/tiny/nr_metric_v1.onnx``.
    sidecar_path:
        Path to ``nr_metric_v1.json`` (sidecar) containing
        ``calibration_threshold``.  Defaults to the sibling ``.json`` of
        ``model_path``.
    use_gpu_ep:
        When ``True``, attempt to load the CUDA/ROCm execution provider
        first; fall back to CPU EP if unavailable.  When ``False`` (or
        when the GPU EP is not installed), use CPU EP exclusively.
    delta_fast:
        Override for the δ_fast threshold.  When ``None`` (default), the
        value is read from the sidecar JSON ``calibration_threshold`` field;
        if that field is absent, ``NR_PROXY_DEFAULT_DELTA_FAST`` is used.
    """

    model_path: Path = dataclasses.field(default_factory=lambda: _default_nr_model_path())
    sidecar_path: Path | None = None
    use_gpu_ep: bool = True
    delta_fast: float | None = None

    # Private: loaded lazily on first call to score_nr.
    _session: object = dataclasses.field(default=None, init=False, repr=False)
    _delta_fast_resolved: float = dataclasses.field(default=float("nan"), init=False, repr=False)
    _calibration_slope_resolved: float = dataclasses.field(
        default=float("nan"), init=False, repr=False
    )
    _calibration_intercept_resolved: float = dataclasses.field(
        default=float("nan"), init=False, repr=False
    )
    _cache: dict[tuple, float] = dataclasses.field(default_factory=dict, init=False, repr=False)

    def __post_init__(self) -> None:
        # Resolve sidecar path alongside model_path.
        if self.sidecar_path is None:
            object.__setattr__(self, "sidecar_path", self.model_path.with_suffix(".json"))

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    @property
    def calibration_threshold(self) -> float:
        """Return the resolved δ_fast threshold (lazy, cached)."""
        import math

        if math.isnan(self._delta_fast_resolved):
            object.__setattr__(self, "_delta_fast_resolved", self._resolve_delta_fast())
        return self._delta_fast_resolved

    @property
    def calibration_slope(self) -> float:
        """Return the raw-NR-to-VMAF calibration slope."""
        import math

        resolved = getattr(self, "_calibration_slope_resolved", float("nan"))
        if math.isnan(resolved):
            object.__setattr__(
                self,
                "_calibration_slope_resolved",
                self._resolve_sidecar_float(
                    "calibration_slope",
                    NR_PROXY_DEFAULT_CALIBRATION_SLOPE,
                ),
            )
        return self._calibration_slope_resolved

    @property
    def calibration_intercept(self) -> float:
        """Return the raw-NR-to-VMAF calibration intercept."""
        import math

        resolved = getattr(self, "_calibration_intercept_resolved", float("nan"))
        if math.isnan(resolved):
            object.__setattr__(
                self,
                "_calibration_intercept_resolved",
                self._resolve_sidecar_float(
                    "calibration_intercept",
                    NR_PROXY_DEFAULT_CALIBRATION_INTERCEPT,
                ),
            )
        return self._calibration_intercept_resolved

    def score_nr(
        self,
        distorted: Path,
        *,
        width: int,
        height: int,
        pix_fmt: str = "yuv420p",
    ) -> NRProbeResult:
        """Return raw NR proxy score for ``distorted``.

        Parameters
        ----------
        distorted:
            Path to the distorted raw YUV file (or any format readable
            by the NR model extractor — only luma plane is used).
        width, height:
            Frame geometry of ``distorted``.
        pix_fmt:
            Pixel format (used to compute luma plane size; ``yuv420p``
            luma plane = ``width × height`` bytes of 8-bit Y).

        Returns
        -------
        NRProbeResult
            ``(nr_score, from_cache)`` where ``nr_score`` is the raw model
            output. Use :meth:`calibrated_vmaf_score` to compare it with a
            VMAF target.

        Raises
        ------
        NRProxyBackendError
            On model load failure, I/O error reading ``distorted``, or
            out-of-range inference result.
        """
        cache_key = (str(distorted), width, height)
        if cache_key in self._cache:
            return NRProbeResult(nr_score=self._cache[cache_key], from_cache=True)

        session = self._get_session()
        score = self._run_inference(session, distorted, width=width, height=height, pix_fmt=pix_fmt)
        self._cache[cache_key] = score
        return NRProbeResult(nr_score=score, from_cache=False)

    def clear_cache(self) -> None:
        """Evict all cached NR scores.

        Call between bisect runs when ``NRProxyBackend`` is reused
        across different (source, codec, target) triples.
        """
        self._cache.clear()

    def is_far_from_target(self, nr_score: float, target_vmaf: float) -> bool:
        """Return True when calibrated NR-VMAF is outside the uncertainty zone.

        Implements the ADR-0615 / ADR-0624 decision rule:
        ``|calibrated_NR_VMAF - target_vmaf| > δ_fast`` → skip FR call.
        """
        return abs(self.calibrated_vmaf_score(nr_score) - target_vmaf) > self.calibration_threshold

    def nr_implied_direction(self, nr_score: float, target_vmaf: float) -> str:
        """Return ``"tighter"`` (raise CRF) or ``"looser"`` (lower CRF).

        Convention matches the bisect direction logic in ``bisect.py``:
        higher CRF = lower quality.  Calibrated NR-VMAF > target →
        quality exceeds goal → we can afford higher CRF (tighter
        compression).
        """
        if self.calibrated_vmaf_score(nr_score) >= target_vmaf:
            return "tighter"
        return "looser"

    def calibrated_vmaf_score(self, nr_score: float) -> float:
        """Map raw NR model output into calibrated VMAF score space."""
        score = (self.calibration_slope * nr_score) + self.calibration_intercept
        return max(0.0, min(100.0, score))

    # ------------------------------------------------------------------
    # Private helpers
    # ------------------------------------------------------------------

    def _get_session(self) -> object:
        """Load / return the cached onnxruntime InferenceSession."""
        if self._session is not None:
            return self._session
        session = _load_ort_session(self.model_path, use_gpu_ep=self.use_gpu_ep)
        object.__setattr__(self, "_session", session)
        return session

    def _resolve_delta_fast(self) -> float:
        """Read δ_fast from sidecar JSON or return the compile-time default."""
        if self.delta_fast is not None:
            return float(self.delta_fast)
        return self._resolve_sidecar_float("calibration_threshold", NR_PROXY_DEFAULT_DELTA_FAST)

    def _resolve_sidecar_float(self, key: str, default: float) -> float:
        """Read a float calibration field from sidecar JSON."""
        sidecar = self.sidecar_path
        if sidecar is not None and sidecar.is_file():
            try:
                data = json.loads(sidecar.read_text(encoding="utf-8"))
                raw = data.get(key)
                if raw is not None:
                    val = float(raw)
                    _log.debug("NRProxyBackend: loaded %s=%.3f from %s", key, val, sidecar)
                    return val
            except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
                _log.warning(
                    "NRProxyBackend: failed to read %s from %s: %s; using default %.3f",
                    key,
                    sidecar,
                    exc,
                    default,
                )
        _log.debug(
            "NRProxyBackend: no %s in sidecar; using default %.3f",
            key,
            default,
        )
        return default

    def _run_inference(
        self,
        session: Any,  # onnxruntime.InferenceSession (no py.typed marker)
        distorted: Path,
        *,
        width: int,
        height: int,
        pix_fmt: str,
    ) -> float:
        """Extract middle luma frame, resize, run inference, return score."""
        import math

        try:
            import numpy as np
        except ImportError as exc:
            raise NRProxyBackendError(
                "numpy is required for NRProxyBackend inference; "
                "install it with: pip install numpy"
            ) from exc

        luma = _extract_middle_luma_frame(distorted, width=width, height=height, pix_fmt=pix_fmt)
        resized = _resize_luma_224(luma, width=width, height=height)

        # nr_metric_v1 expects float32 NCHW: (1, 1, 224, 224), range [0, 1].
        tensor = resized.astype(np.float32)[np.newaxis, np.newaxis, :, :] / 255.0

        input_name = getattr(session, "get_inputs", lambda: [type("I", (), {"name": "input"})()])()[
            0
        ].name  # noqa: E501
        try:
            outputs = session.run(None, {input_name: tensor})
        except Exception as exc:
            raise NRProxyBackendError(f"NR model inference failed for {distorted}: {exc}") from exc
        raw = float(outputs[0].flat[0])

        if math.isnan(raw) or raw < 0.0 or raw > 100.0:
            raise NRProxyBackendError(
                f"NR model returned out-of-range score {raw!r} for {distorted}; "
                "check model integrity or frame extraction."
            )
        return raw


# ---------------------------------------------------------------------------
# Module-level helpers (private)
# ---------------------------------------------------------------------------


def _default_nr_model_path() -> Path:
    """Return the in-tree path to ``nr_metric_v1.onnx``."""
    # Walk up from this file: src/vmaftune/ -> src/ -> vmaf-tune/ -> tools/ -> <repo_root>
    here = Path(__file__).resolve()
    repo_root = here.parents[4]
    return repo_root / "model" / "tiny" / "nr_metric_v1.onnx"


def _load_ort_session(model_path: Path, *, use_gpu_ep: bool = True) -> object:
    """Load an onnxruntime ``InferenceSession`` for ``model_path``.

    Attempts CUDA EP first when ``use_gpu_ep=True``; falls back to CPU EP
    on ``InvalidGraph`` / ``OrtFail`` / missing CUDA EP.  This mirrors the
    progressive-EP pattern used in ``ai/src/vmaf_train/ort_utils.py``.

    Raises
    ------
    NRProxyBackendError
        When onnxruntime is not installed or the model file is missing.
    """
    if not model_path.is_file():
        raise NRProxyBackendError(
            f"NR model file not found: {model_path}. "
            "Run 'python ai/scripts/export_tiny_models.py' to regenerate it, "
            "or check that model/tiny/nr_metric_v1.onnx is present in the repo."
        )
    try:
        import onnxruntime as ort
    except ImportError as exc:
        raise NRProxyBackendError(
            "onnxruntime is required for NRProxyBackend; "
            "install it with: pip install onnxruntime  (CPU) "
            "or onnxruntime-gpu (GPU EP)"
        ) from exc

    providers: list[str] = ["CPUExecutionProvider"]
    if use_gpu_ep:
        available_eps = ort.get_available_providers()
        gpu_eps = [
            ep for ep in ("CUDAExecutionProvider", "ROCMExecutionProvider") if ep in available_eps
        ]  # noqa: E501
        if gpu_eps:
            providers = gpu_eps + providers

    opts = ort.SessionOptions()
    opts.log_severity_level = 3  # ERROR only — suppress ORT verbose init spam
    try:
        session = ort.InferenceSession(str(model_path), sess_options=opts, providers=providers)
    except Exception as exc:
        raise NRProxyBackendError(f"Failed to load NR model from {model_path}: {exc}") from exc

    _log.debug(
        "NRProxyBackend: loaded %s (providers=%s)",
        model_path.name,
        session.get_providers(),
    )
    return session


def _luma_plane_bytes(width: int, height: int, pix_fmt: str) -> int:
    """Return the byte size of the luma plane for one frame."""
    # All supported pix_fmts store luma as width×height pixels.
    # 8-bit formats: 1 byte/pixel; 10/12-bit formats: 2 bytes/pixel (LE).
    bits_16 = pix_fmt.endswith("10le") or pix_fmt.endswith("12le") or pix_fmt.endswith("16le")
    return width * height * (2 if bits_16 else 1)


def _chroma_plane_bytes(width: int, height: int, pix_fmt: str) -> int:
    """Return the total byte size of both chroma planes for one frame."""
    bits_16 = pix_fmt.endswith("10le") or pix_fmt.endswith("12le") or pix_fmt.endswith("16le")
    bpp = 2 if bits_16 else 1
    if pix_fmt.startswith("yuv444"):
        return width * height * 2 * bpp
    if pix_fmt.startswith("yuv422"):
        return (width // 2) * height * 2 * bpp
    # Default: yuv420 (also yuv420p10le etc.)
    return (width // 2) * (height // 2) * 2 * bpp


def _frame_bytes(width: int, height: int, pix_fmt: str) -> int:
    """Return the total byte size of one raw YUV frame."""
    return _luma_plane_bytes(width, height, pix_fmt) + _chroma_plane_bytes(width, height, pix_fmt)


def _extract_middle_luma_frame(
    yuv_path: Path,
    *,
    width: int,
    height: int,
    pix_fmt: str,
) -> "np.ndarray":  # type: ignore[name-defined]  # noqa: F821
    """Read the middle frame's luma plane from a raw YUV file.

    Returns a 2-D ``uint8`` numpy array of shape ``(height, width)``.
    For 10/12-bit formats the high byte of each 16-bit LE sample is used
    (effectively a right-shift by 8), reducing precision to 8 bits — this
    matches the training-time normalisation (KoNViD-1k is 8-bit).

    Raises
    ------
    NRProxyBackendError
        On I/O failure or geometry mismatch.
    """
    try:
        import numpy as np
    except ImportError as exc:
        raise NRProxyBackendError("numpy is required for NRProxyBackend inference") from exc

    frame_sz = _frame_bytes(width, height, pix_fmt)
    luma_sz = _luma_plane_bytes(width, height, pix_fmt)
    try:
        file_sz = yuv_path.stat().st_size
    except OSError as exc:
        raise NRProxyBackendError(f"cannot stat distorted file {yuv_path}: {exc}") from exc

    if frame_sz <= 0:
        raise NRProxyBackendError(
            f"computed frame size is zero for geometry {width}×{height} pix_fmt={pix_fmt}"
        )

    n_frames = max(1, file_sz // frame_sz)
    mid_frame = n_frames // 2
    offset = mid_frame * frame_sz

    bits_16 = pix_fmt.endswith("10le") or pix_fmt.endswith("12le") or pix_fmt.endswith("16le")
    try:
        with yuv_path.open("rb") as fh:
            fh.seek(offset)
            raw = fh.read(luma_sz)
    except OSError as exc:
        raise NRProxyBackendError(
            f"failed to read luma plane from {yuv_path} at offset {offset}: {exc}"
        ) from exc

    if len(raw) < luma_sz:
        raise NRProxyBackendError(
            f"truncated luma read from {yuv_path}: expected {luma_sz} bytes, "
            f"got {len(raw)} (file may be incomplete)"
        )

    if bits_16:
        # Interpret as uint16 LE, take high byte (>>8) → uint8.
        arr_u16 = np.frombuffer(raw, dtype=np.uint16)
        arr_u8 = (arr_u16 >> 8).astype(np.uint8)
    else:
        arr_u8 = np.frombuffer(raw, dtype=np.uint8)

    return arr_u8.reshape(height, width)


def _resize_luma_224(luma: "np.ndarray", *, width: int, height: int) -> "np.ndarray":  # type: ignore[name-defined]  # noqa: F821
    """Resize luma frame to 224×224 using simple bilinear interpolation.

    Uses ``cv2`` when available (faster); falls back to a pure-numpy
    implementation.  Both paths produce ``uint8`` output.
    """
    if width == NR_MODEL_INPUT_HW and height == NR_MODEL_INPUT_HW:
        return luma

    try:
        import cv2  # type: ignore[import-not-found]

        return cv2.resize(  # type: ignore[no-any-return]
            luma,
            (NR_MODEL_INPUT_HW, NR_MODEL_INPUT_HW),
            interpolation=cv2.INTER_LINEAR,
        )
    except ImportError:
        pass

    # Pure-numpy bilinear fallback (slow for large frames but avoids cv2 dep).
    try:
        import numpy as np
    except ImportError as exc:
        raise NRProxyBackendError("numpy is required for NRProxyBackend") from exc

    target = NR_MODEL_INPUT_HW
    row_idx = np.linspace(0, height - 1, target)
    col_idx = np.linspace(0, width - 1, target)
    row_lo = np.floor(row_idx).astype(np.int32).clip(0, height - 2)
    col_lo = np.floor(col_idx).astype(np.int32).clip(0, width - 2)
    row_frac = (row_idx - row_lo).reshape(-1, 1).astype(np.float32)
    col_frac = (col_idx - col_lo).reshape(1, -1).astype(np.float32)

    luma_f = luma.astype(np.float32)
    top = (
        luma_f[row_lo[:, None], col_lo[None, :]] * (1.0 - col_frac)
        + luma_f[row_lo[:, None], col_lo[None, :] + 1] * col_frac
    )  # noqa: E501
    bottom = (
        luma_f[row_lo[:, None] + 1, col_lo[None, :]] * (1.0 - col_frac)
        + luma_f[row_lo[:, None] + 1, col_lo[None, :] + 1] * col_frac
    )  # noqa: E501
    result = top * (1.0 - row_frac) + bottom * row_frac
    return result.clip(0, 255).astype(np.uint8)
