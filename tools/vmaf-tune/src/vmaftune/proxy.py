# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""ONNX inference seam for `fr_regressor_v2` (vmaf-tune fast-path proxy).

This module is the single seam every fast-path consumer goes through
when it needs a proxy VMAF prediction. Loading and running the ONNX
session is centralised here so future migrations (probabilistic head /
ensemble / new feature layout) land in one place rather than scattered
across `fast.py`, `recommend.py`, and any future per-shot consumer.

The model contract is fixed by [ADR-0291](../../../docs/adr/0291-fr-regressor-v2-prod-ship.md):

- **Input shape**: 6 canonical libvmaf features (adm2, vif_scale0..3,
  motion2, StandardScaler-normalised) + 14-D codec block (12-way
  encoder one-hot per ENCODER_VOCAB v2 + preset_norm + crf_norm).
- **Output**: scalar VMAF teacher score.
- **MLP shape**: `6 → 32 → 32 → 32 → 1` with codec block concatenated
  before the first dense layer.

Lazy imports — onnxruntime and numpy stay optional dependencies; the
core grid path (corpus.py) never touches this module.
"""

from __future__ import annotations

from collections.abc import Sequence
from functools import lru_cache
from pathlib import Path
from typing import Any

# ENCODER_VOCAB v2 — 12-way codec one-hot ordering frozen by ADR-0291.
# Keep in sync with ai/scripts/train_fr_regressor_v2.py; if either drifts,
# ProxyError is raised at inference time before bad predictions ship.
ENCODER_VOCAB_V2: tuple[str, ...] = (
    "libx264",
    "libx265",
    "libsvtav1",
    "libvvenc",
    "libvpx-vp9",
    "h264_nvenc",
    "hevc_nvenc",
    "av1_nvenc",
    "h264_qsv",
    "hevc_qsv",
    "av1_qsv",
    "unknown",
)

#: Default registry id (matches `model/tiny/registry.json`).
DEFAULT_PROXY_MODEL_ID: str = "fr_regressor_v2"


class ProxyError(RuntimeError):
    """Proxy inference unavailable or contract mismatch."""


def _import_onnxruntime() -> Any:
    try:
        import onnxruntime as ort
    except ImportError as exc:
        raise ProxyError(
            "onnxruntime is required for the vmaf-tune fast-path proxy; "
            "install with `pip install 'vmaf-tune[fast]'`"
        ) from exc
    return ort


def _import_numpy() -> Any:
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - defensive
        raise ProxyError("numpy is required for proxy inference") from exc
    return np


def _resolve_model_path(model_id: str = DEFAULT_PROXY_MODEL_ID) -> Path:
    """Resolve `model/tiny/<model_id>.onnx` from the repo root.

    The repo layout pins `model/tiny/` at `<repo>/model/tiny/`. We walk
    up from this file's location (tools/vmaf-tune/src/vmaftune/proxy.py)
    until we find a directory containing `model/tiny/<id>.onnx`.
    """
    here = Path(__file__).resolve()
    for ancestor in here.parents:
        candidate = ancestor / "model" / "tiny" / f"{model_id}.onnx"
        if candidate.exists():
            return candidate
    raise ProxyError(
        f"could not locate model/tiny/{model_id}.onnx walking up from "
        f"{here}; the proxy is shipped in-tree per ADR-0291 — check "
        f"that the registry-pinned ONNX exists."
    )


@lru_cache(maxsize=4)
def _load_session(model_id: str, model_path: str) -> Any:
    """Cache one InferenceSession per (id, path) pair.

    Caching is critical for the fast-path: TPE invokes the proxy 30–50
    times per recommendation, each call would otherwise pay the
    ~50–200 ms session-creation cost.
    """
    ort = _import_onnxruntime()
    return ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])


@lru_cache(maxsize=4)
def load_proxy_sidecar(model_id: str = DEFAULT_PROXY_MODEL_ID) -> dict[str, Any]:
    """Load and parse the `model/tiny/<model_id>.json` sidecar.

    Cached per model_id to avoid reading JSON on every prediction.
    """
    import json

    model_path = _resolve_model_path(model_id)
    sidecar_path = model_path.with_suffix(".json")
    if not sidecar_path.exists():
        raise ProxyError(f"proxy sidecar not found: {sidecar_path}")
    try:
        return json.loads(sidecar_path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise ProxyError(f"failed to parse proxy sidecar {sidecar_path}: {exc}") from exc


def normalise_features(
    features: Sequence[float],
    model_id: str = DEFAULT_PROXY_MODEL_ID,
) -> list[float]:
    """Apply the sidecar's StandardScaler to a canonical-6 feature vector.

    Calculates ``(x - feature_mean) / feature_std`` elementwise using the
    scaler parameters exported into the model sidecar (ADR-0291).
    A zero or missing std leaves the feature unscaled (mirrors ``pkg/fast``).
    """
    import math

    if len(features) != 6:
        raise ProxyError(f"features must be the canonical-6 vector; got length {len(features)}")
    sidecar = load_proxy_sidecar(model_id)
    mean = sidecar.get("feature_mean", [])
    std = sidecar.get("feature_std", [])
    out: list[float] = []
    for i, v in enumerate(features):
        val = float(v)
        if i < len(mean):
            val -= float(mean[i])
        if i < len(std):
            s = float(std[i])
            if s != 0.0 and not math.isnan(s):
                val /= s
        out.append(val)
    return out


def encode_codec_block(
    encoder: str,
    preset_norm: float,
    crf_norm: float,
    *,
    allow_unknown: bool = False,
) -> Any:
    """Build the 14-D codec block: 12-way one-hot + preset_norm + crf_norm.

    Encoder names outside ENCODER_VOCAB_V2 raise `ProxyError` unless
    `allow_unknown=True` is set and `unknown` is in the vocabulary, in
    which case the `unknown` slot is activated (mirrors `pkg/fast`).
    Preset and CRF are caller-normalised to `[0, 1]`.
    """
    np = _import_numpy()
    if encoder not in ENCODER_VOCAB_V2:
        if allow_unknown and "unknown" in ENCODER_VOCAB_V2:
            idx = ENCODER_VOCAB_V2.index("unknown")
        else:
            raise ProxyError(
                f"encoder {encoder!r} not in ENCODER_VOCAB_V2 (frozen by "
                f"ADR-0291). Allowed: {', '.join(ENCODER_VOCAB_V2)}."
            )
    else:
        idx = ENCODER_VOCAB_V2.index(encoder)
    one_hot = np.zeros(len(ENCODER_VOCAB_V2), dtype=np.float32)
    one_hot[idx] = 1.0
    block = np.concatenate(
        [one_hot, np.asarray([preset_norm, crf_norm], dtype=np.float32)],
        axis=0,
    )
    assert block.shape == (14,), f"codec block shape mismatch: {block.shape}"
    return block


def run_proxy(
    features: Sequence[float],
    *,
    encoder: str,
    preset_norm: float,
    crf_norm: float,
    model_id: str = DEFAULT_PROXY_MODEL_ID,
    session_factory: Any | None = None,
    allow_unknown: bool = False,
    normalise: bool = False,
) -> float:
    """Run `fr_regressor_v2` over `(features, codec_block)` → scalar VMAF.

    Parameters
    ----------
    features
        Six canonical libvmaf features in the canonical-6 order:
        ``(adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2)``.
        If ``normalise=True``, :func:`normalise_features` is applied first.
        Otherwise, caller is responsible for StandardScaler normalisation.
    encoder
        Codec name; must be in :data:`ENCODER_VOCAB_V2` (or maps to
        ``unknown`` when ``allow_unknown=True``).
    preset_norm
        Preset axis normalised to ``[0, 1]`` (caller-mapped).
    crf_norm
        CRF axis normalised to ``[0, 1]`` over the adapter's quality range.
    model_id
        Registry id; defaults to :data:`DEFAULT_PROXY_MODEL_ID`.
    session_factory
        Test seam — when provided, called as ``session_factory(model_path)``
        and must return an object with the onnxruntime ``InferenceSession``
        ``.run(output_names, input_feed)`` interface. Production callers
        leave this default.
    allow_unknown
        When True, an unknown encoder is mapped to the ``unknown`` one-hot slot.
    normalise
        When True, applies the model sidecar's StandardScaler normalisation
        to ``features``.

    Returns
    -------
    float
        Predicted VMAF score on the standard ``[0, 100]`` scale.

    Raises
    ------
    ProxyError
        Onnxruntime missing, model file missing, or contract mismatch.
    """
    np = _import_numpy()
    if len(features) != 6:
        raise ProxyError(f"features must be the canonical-6 vector; got length {len(features)}")

    if normalise:
        features = normalise_features(features, model_id=model_id)

    feat = np.asarray(features, dtype=np.float32).reshape(1, 6)
    codec_block = encode_codec_block(
        encoder,
        preset_norm,
        crf_norm,
        allow_unknown=allow_unknown,
    ).reshape(1, 14)
    # The v2 ONNX graph was exported with two *separate* named inputs —
    # "features" (shape [N, 6]) and "codec" (shape [N, 14]) — matching the
    # FRRegressor.forward(features, codec_onehot) signature in
    # ai/src/vmaf_train/models/fr_regressor.py.  Concatenating them into a
    # single 20-D tensor and feeding it as input[0] was wrong: the first
    # linear layer of the exported graph reads the 6-D "features" port only,
    # so the 14 codec dims were silently interpreted as batch padding and the
    # "codec" port received nothing, breaking fast-path production mode.
    if session_factory is None:
        model_path = _resolve_model_path(model_id)
        session = _load_session(model_id, str(model_path))
    else:
        session = session_factory(_resolve_model_path(model_id))

    input_names = [inp.name for inp in session.get_inputs()]
    if len(input_names) == 2:
        # Two-input graph: "features" + "codec" (standard v2 export).
        features_name, codec_name = input_names
        outputs = session.run(None, {features_name: feat, codec_name: codec_block})
    else:
        # Fallback: single-input graph (legacy smoke or future merged export).
        combined = np.concatenate([feat, codec_block], axis=1)
        outputs = session.run(None, {input_names[0]: combined})
    score = float(np.asarray(outputs[0]).reshape(-1)[0])
    return score


__all__ = [
    "DEFAULT_PROXY_MODEL_ID",
    "ENCODER_VOCAB_V2",
    "ProxyError",
    "encode_codec_block",
    "load_proxy_sidecar",
    "normalise_features",
    "run_proxy",
]
