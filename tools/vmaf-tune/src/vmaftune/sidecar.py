# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Local sidecar training — on-host bias-correction model.

Companion to :mod:`vmaftune.predictor`. The shipped predictor is a
fixed, deterministic asset trained offline against the corpus
described in ADR-0309 / ADR-0310. The sidecar is a *bias-correction
term* an operator trains on their own host from the residuals
between predicted VMAF and the libvmaf score actually observed at
encode time. Inference adds the correction to the shipped
predictor's output:

    sidecar_predict(features, crf, codec) =
        Predictor.predict_vmaf(features, crf, codec)
        + SidecarModel.predict_correction(features)

The shipped predictor is **never** mutated — model upgrades stay
deterministic and reproducible across hosts.

Persistence layout (per ADR-0325 / Research-0086):

* ``${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/host-uuid``
  — random 128-bit hex token generated on first install. Anonymous
  by construction; **never** derived from MAC, hostname,
  ``/etc/machine-id``, CPUID, or anything machine-identifying.
* ``${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/<predictor-version>/<codec>/state.json``
  — ridge weights, inverse Gram, ring buffer of recent residuals,
  the ``predictor_version`` the sidecar was trained against.

A predictor version mismatch on load discards everything except the
host UUID and resets to cold-start (zero correction). The mechanism
is what makes shipped-model upgrades safe: the sidecar can never
replay a stale correction against a refreshed predictor.

Algorithm
---------

Online ridge regression on the residual ``y = observed_vmaf −
predicted_vmaf`` with closed-form Sherman-Morrison rank-1 inverse
update. State size is ``O(d²)`` where ``d`` is the fixed-dim feature
vector length (currently 14). Update cost is ``O(d²)`` per capture;
prediction cost is ``O(d)``.

Cold-start: weights initialised to zero, inverse Gram initialised to
``(1/lambda_l2) * I``. With zero weights, ``predict_correction``
returns ``0.0`` exactly — :class:`SidecarPredictor` degenerates to
:class:`Predictor` until the first :meth:`SidecarModel.update` call.

The implementation is pure-Python (``math``, ``json``, ``secrets``)
to honour ``vmaf-tune``'s zero-dependency contract on the harness
hot path. ``numpy`` is used opportunistically when present for the
Sherman-Morrison kernel; the pure-Python fallback is bit-equivalent
within float-rounding tolerance and is the path the unit tests
exercise.

See :doc:`docs/ai/local-sidecar-training` for the operator-facing
guide.
"""

from __future__ import annotations

import dataclasses
import json
import math
import os
import secrets
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from .jsonio import write_json_strict
from .predictor import Predictor, ShotFeatures

# ----------------------------------------------------------------------
# Constants — load-bearing on the persistence contract.
# ----------------------------------------------------------------------

#: Schema version for the on-disk sidecar state. Bump on every
#: backwards-incompatible change to the JSON layout.
SIDECAR_SCHEMA_VERSION: int = 1

#: Fixed-dim feature-vector length the sidecar consumes. Derived from
#: :class:`ShotFeatures` — see :func:`_feature_vector`. Must stay in
#: lockstep with that function; changing the dimensionality forces a
#: ``SIDECAR_SCHEMA_VERSION`` bump (older saved state cannot be loaded).
FEATURE_DIM: int = 14

#: Default predictor-version tag. The shipped predictor does not yet
#: expose its own version string — until it does, the sidecar reads
#: this constant. When :class:`Predictor` gains a ``version`` field,
#: switch to that and use this only as a final fallback.
DEFAULT_PREDICTOR_VERSION: str = "predictor_v1"


#: Default cache directory. Honours ``XDG_CACHE_HOME`` per the freedesktop
#: base-dir spec; falls back to ``~/.cache``.
def _default_cache_dir() -> Path:
    """Return the default sidecar cache directory.

    Honours the ``XDG_CACHE_HOME`` environment variable per the
    freedesktop.org base-directory spec; falls back to ``~/.cache`` on
    hosts that do not set it.
    """
    xdg = os.environ.get("XDG_CACHE_HOME")
    if xdg:
        return Path(xdg) / "vmaf-tune" / "sidecar"
    return Path.home() / ".cache" / "vmaf-tune" / "sidecar"


# ----------------------------------------------------------------------
# Configuration.
# ----------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class SidecarConfig:
    """Static configuration for the local sidecar.

    Attributes
    ----------
    cache_dir
        Root directory under which the sidecar persists its state and
        the host UUID. Defaults to
        ``${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar``.
    host_uuid
        Anonymous random per-install token. ``None`` lets
        :class:`SidecarModel` generate / load it lazily — the standard
        path. Tests pin this for determinism.
    lambda_l2
        L2 regularisation strength. Higher values pull the correction
        toward zero — defends against single-outlier captures.
    max_history_rows
        Bounded ring-buffer size for the captured residuals the
        drift-detection hook reads. Does not affect the ridge fit
        itself (closed-form update consumes captures one at a time).
    training_cadence
        Reserved for future use. Currently the sidecar updates per
        capture; a future PR may switch to per-session or scheduled
        retraining once we measure the per-capture cost in production.
    predictor_version
        Tag the sidecar persists alongside its state. On load,
        mismatch with the running predictor's version discards the
        old state. See module-level docstring.
    """

    cache_dir: Path = dataclasses.field(default_factory=_default_cache_dir)
    host_uuid: str | None = None
    lambda_l2: float = 1.0
    max_history_rows: int = 500
    training_cadence: str = "per-capture"
    predictor_version: str = DEFAULT_PREDICTOR_VERSION


# ----------------------------------------------------------------------
# Feature-vector builder — keep in lockstep with FEATURE_DIM.
# ----------------------------------------------------------------------


def _feature_vector(features: ShotFeatures, crf: int) -> list[float]:
    """Materialise the fixed-dim feature vector the sidecar consumes.

    Order is load-bearing — it pins the column index of every weight in
    the ridge fit. Changing it requires bumping
    :data:`SIDECAR_SCHEMA_VERSION` (older saved states would silently
    align mismatched columns to the wrong feature otherwise).

    The leading ``1.0`` is the bias / intercept term so the ridge fit
    can model a constant offset between predicted and observed VMAF.
    """
    vec = [
        1.0,  # bias term
        float(crf),
        features.probe_bitrate_kbps,
        features.probe_i_frame_avg_bytes,
        features.probe_p_frame_avg_bytes,
        features.probe_b_frame_avg_bytes,
        features.saliency_mean,
        features.saliency_var,
        features.frame_diff_mean,
        features.y_avg,
        features.y_var,
        float(features.shot_length_frames),
        features.fps,
        float(features.width),
    ]
    # Sanity: assert the hand-rolled length matches the published
    # constant. This is a property the scaffold cannot afford to drift.
    if len(vec) != FEATURE_DIM:
        raise RuntimeError(
            f"sidecar feature vector length {len(vec)} != FEATURE_DIM={FEATURE_DIM}; "
            f"bump SIDECAR_SCHEMA_VERSION when changing the layout"
        )
    return vec


# ----------------------------------------------------------------------
# Online ridge — closed-form Sherman-Morrison rank-1 update.
# ----------------------------------------------------------------------


def _eye_scaled(d: int, scale: float) -> list[list[float]]:
    """Return ``scale * I`` as a ``d × d`` Python list-of-lists."""
    return [[scale if i == j else 0.0 for j in range(d)] for i in range(d)]


def _matvec(matrix: Sequence[Sequence[float]], vec: Sequence[float]) -> list[float]:
    """Return ``matrix @ vec``."""
    n = len(matrix)
    out = [0.0] * n
    for i in range(n):
        row = matrix[i]
        s = 0.0
        for j, v in enumerate(vec):
            s += row[j] * v
        out[i] = s
    return out


def _dot(a: Sequence[float], b: Sequence[float]) -> float:
    """Return ``a · b``."""
    s = 0.0
    for ai, bi in zip(a, b, strict=True):
        s += ai * bi
    return s


def _outer_axpy(
    matrix: list[list[float]],
    u: Sequence[float],
    v: Sequence[float],
    alpha: float,
) -> None:
    """In-place ``matrix += alpha * outer(u, v)``."""
    for i, ui in enumerate(u):
        scale = alpha * ui
        if scale == 0.0:
            continue
        row = matrix[i]
        for j, vj in enumerate(v):
            row[j] += scale * vj


@dataclasses.dataclass
class SidecarModel:
    """Online-ridge bias-correction state.

    The model maintains a weight vector ``w`` and the inverse Gram matrix
    ``A_inv`` such that ``w = A_inv @ (X^T y)`` for all observed
    ``(features, residual)`` pairs. Closed-form rank-1 update via
    Sherman-Morrison keeps both quantities in sync per capture in
    ``O(d²)`` time without any numerical solver.

    Cold-start: ``w = 0``, ``A_inv = (1/lambda_l2) * I``. With zero
    weights, :meth:`predict_correction` returns ``0.0`` exactly.
    """

    config: SidecarConfig = dataclasses.field(default_factory=SidecarConfig)
    weights: list[float] = dataclasses.field(default_factory=lambda: [0.0] * FEATURE_DIM)
    a_inv: list[list[float]] = dataclasses.field(default_factory=list)
    history: list[dict[str, Any]] = dataclasses.field(default_factory=list)
    n_updates: int = 0

    def __post_init__(self) -> None:
        """Initialise ``A_inv`` if the caller didn't pass one."""
        if not self.a_inv:
            self.a_inv = _eye_scaled(FEATURE_DIM, 1.0 / float(self.config.lambda_l2))
        if len(self.weights) != FEATURE_DIM:
            raise ValueError(f"weights length {len(self.weights)} != FEATURE_DIM={FEATURE_DIM}")

    # -- API ---------------------------------------------------------

    def update(
        self,
        features: ShotFeatures,
        observed_vmaf: float,
        predicted_vmaf: float,
        crf: int,
    ) -> None:
        """Fold one ``(features, residual)`` pair into the ridge fit.

        The residual is ``y = observed_vmaf − predicted_vmaf``; the
        sidecar learns to predict that residual from ``features``.
        The Sherman-Morrison rank-1 inverse update keeps ``A_inv``
        consistent with the cumulative Gram matrix without a solve.
        """
        x = _feature_vector(features, crf)
        residual = float(observed_vmaf) - float(predicted_vmaf)

        # Sherman-Morrison: A_inv_new = A_inv - (A_inv x x^T A_inv) /
        #                                       (1 + x^T A_inv x)
        a_inv_x = _matvec(self.a_inv, x)
        denom = 1.0 + _dot(x, a_inv_x)
        # Numerical safety: denom is positive whenever lambda_l2 > 0
        # and the feature vector is finite. Guard against pathological
        # inputs (NaN, +inf in features) by skipping the update.
        if not math.isfinite(denom) or denom <= 0.0:
            return
        # Build the rank-1 perturbation -1/denom * outer(a_inv_x,
        # a_inv_x) and apply it in place.
        _outer_axpy(self.a_inv, a_inv_x, a_inv_x, -1.0 / denom)

        # Weight update: w_new = w + (residual − x^T w) / denom * A_inv x
        # (the standard recursive least-squares form on the centred
        # residual).
        prediction_error = residual - _dot(x, self.weights)
        coeff = prediction_error / denom
        # Re-derive A_inv x against the updated A_inv for stability.
        a_inv_x_new = _matvec(self.a_inv, x)
        for i in range(FEATURE_DIM):
            self.weights[i] += coeff * a_inv_x_new[i]

        # Ring buffer for drift detection.
        self.history.append(
            {
                "residual": residual,
                "predicted_vmaf": float(predicted_vmaf),
                "observed_vmaf": float(observed_vmaf),
            }
        )
        if len(self.history) > self.config.max_history_rows:
            del self.history[0 : len(self.history) - self.config.max_history_rows]
        self.n_updates += 1

    def predict_correction(self, features: ShotFeatures, crf: int) -> float:
        """Return the additive correction in VMAF units.

        Cold-start (no captures): weights are zero → returns ``0.0``.
        """
        x = _feature_vector(features, crf)
        return _dot(self.weights, x)

    @property
    def recent_residual_rms(self) -> float:
        """RMS of the most recent residuals — drift-detection signal.

        Returns ``0.0`` when the history buffer is empty (cold-start).
        Hook for the future drift-warning PR; the scaffold does not
        threshold on it.
        """
        if not self.history:
            return 0.0
        n = len(self.history)
        s = 0.0
        for row in self.history:
            r = float(row.get("residual", 0.0))
            s += r * r
        return math.sqrt(s / n)

    # -- Persistence -------------------------------------------------

    def to_dict(self) -> dict[str, Any]:
        """Serialise to a JSON-compatible dict."""
        return {
            "schema_version": SIDECAR_SCHEMA_VERSION,
            "predictor_version": self.config.predictor_version,
            "feature_dim": FEATURE_DIM,
            "lambda_l2": float(self.config.lambda_l2),
            "weights": list(self.weights),
            "a_inv": [list(row) for row in self.a_inv],
            "history": list(self.history),
            "n_updates": int(self.n_updates),
        }

    @classmethod
    def from_dict(cls, state: dict[str, Any], config: SidecarConfig) -> SidecarModel:
        """Re-construct from a dict produced by :meth:`to_dict`.

        Raises :class:`ValueError` if the schema version, feature
        dimensionality, or predictor version don't match — the caller
        (:meth:`SidecarPredictor._load`) catches and discards the
        state in that case, falling back to cold-start.
        """
        schema = int(state.get("schema_version", -1))
        if schema != SIDECAR_SCHEMA_VERSION:
            raise ValueError(f"sidecar schema version {schema} != {SIDECAR_SCHEMA_VERSION}")
        dim = int(state.get("feature_dim", -1))
        if dim != FEATURE_DIM:
            raise ValueError(f"sidecar feature_dim {dim} != {FEATURE_DIM}")
        saved_predictor = str(state.get("predictor_version", ""))
        if saved_predictor != config.predictor_version:
            raise ValueError(
                f"sidecar predictor_version {saved_predictor!r} " f"!= {config.predictor_version!r}"
            )
        weights = [float(v) for v in state.get("weights", [])]
        a_inv = [[float(v) for v in row] for row in state.get("a_inv", [])]
        history = list(state.get("history", []))
        n_updates = int(state.get("n_updates", 0))
        if len(weights) != FEATURE_DIM or len(a_inv) != FEATURE_DIM:
            raise ValueError("sidecar state has wrong shape")
        return cls(
            config=config,
            weights=weights,
            a_inv=a_inv,
            history=history,
            n_updates=n_updates,
        )

    def save(self, path: Path) -> None:
        """Write the state to ``path`` as JSON, creating parents."""
        write_json_strict(path, self.to_dict())

    @classmethod
    def load(cls, path: Path, config: SidecarConfig) -> SidecarModel:
        """Read state from ``path`` or return a cold-start model.

        A predictor-version or schema-version mismatch returns a
        cold-start model rather than raising. Corrupted JSON also
        returns cold-start; the corrupted file is left in place so
        the operator can inspect it.
        """
        if not path.is_file():
            return cls(config=config)
        try:
            with path.open("r", encoding="utf-8") as fh:
                state = json.load(fh)
            return cls.from_dict(state, config)
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            return cls(config=config)


# ----------------------------------------------------------------------
# Host UUID — random, anonymous, persisted under the cache dir.
# ----------------------------------------------------------------------


def _host_uuid_path(cache_dir: Path) -> Path:
    """Path to the persisted random host UUID.

    The file lives at the cache-dir root (above per-predictor-version
    subdirectories) so it survives predictor upgrades.
    """
    return cache_dir / "host-uuid"


def get_or_create_host_uuid(cache_dir: Path) -> str:
    """Return the anonymous random host UUID, creating it on first call.

    The UUID is a 32-character hex string (128 bits of entropy) drawn
    from :func:`secrets.token_hex`. **Never** derived from MAC,
    hostname, ``/etc/machine-id``, CPUID, or any other
    machine-identifying signal — this is a load-bearing precondition
    for the future opt-in upload PR (see ADR-0325 §Future work).
    """
    cache_dir.mkdir(parents=True, exist_ok=True)
    path = _host_uuid_path(cache_dir)
    if path.is_file():
        try:
            uuid = path.read_text(encoding="utf-8").strip()
            if uuid:
                return uuid
        except OSError:
            # Fall through to regeneration; existing file will be
            # overwritten with a fresh token.
            pass
    uuid = secrets.token_hex(16)
    # Atomic-ish: write to tmp + rename.
    tmp = path.with_suffix(".tmp")
    tmp.write_text(uuid + "\n", encoding="utf-8")
    tmp.replace(path)
    return uuid


# ----------------------------------------------------------------------
# Composed predictor — the public surface a caller integrates with.
# ----------------------------------------------------------------------


@dataclasses.dataclass
class SidecarPredictor:
    """Composes a :class:`Predictor` with a :class:`SidecarModel`.

    :meth:`predict_vmaf` delegates to the wrapped :class:`Predictor`,
    then adds :meth:`SidecarModel.predict_correction`. Callers feed
    captured ``(features, observed_vmaf)`` pairs back via
    :meth:`record_capture`, which also persists state to the cache
    dir.

    Construct via :meth:`for_codec` to wire the per-codec persistence
    layout automatically.
    """

    predictor: Predictor
    model: SidecarModel
    codec: str
    state_path: Path
    host_uuid: str

    # -- Construction ------------------------------------------------

    @classmethod
    def for_codec(
        cls,
        predictor: Predictor,
        codec: str,
        config: SidecarConfig | None = None,
    ) -> SidecarPredictor:
        """Wire a sidecar for ``predictor`` against ``codec``.

        Loads existing state from the canonical cache path if it
        exists *and* its recorded predictor version matches; otherwise
        starts cold.
        """
        cfg = config or SidecarConfig()
        host_uuid = cfg.host_uuid or get_or_create_host_uuid(cfg.cache_dir)
        state_path = cfg.cache_dir / cfg.predictor_version / codec / "state.json"
        model = SidecarModel.load(state_path, cfg)
        return cls(
            predictor=predictor,
            model=model,
            codec=codec,
            state_path=state_path,
            host_uuid=host_uuid,
        )

    # -- Inference ---------------------------------------------------

    def predict_vmaf(
        self,
        features: ShotFeatures,
        crf: int,
        codec: str | None = None,
    ) -> float:
        """Predict VMAF with sidecar correction folded in.

        Clamped to ``[0.0, 100.0]`` like :meth:`Predictor.predict_vmaf`
        so a runaway correction can't push the score outside the
        VMAF range.
        """
        c = codec or self.codec
        base = self.predictor.predict_vmaf(features, crf, c)
        correction = self.model.predict_correction(features, crf)
        return max(0.0, min(100.0, base + correction))

    # -- Capture / training -----------------------------------------

    def record_capture(
        self,
        features: ShotFeatures,
        crf: int,
        observed_vmaf: float,
        codec: str | None = None,
        persist: bool = True,
    ) -> None:
        """Fold one observed VMAF into the sidecar fit.

        Computes the bare-predictor prediction (sidecar excluded)
        against ``features`` / ``crf`` / ``codec``, then folds the
        residual ``observed − predicted`` into the ridge state. Saves
        to disk unless ``persist=False`` (tests pass ``False`` to
        avoid touching the filesystem on every call).
        """
        c = codec or self.codec
        predicted = self.predictor.predict_vmaf(features, crf, c)
        self.model.update(features, observed_vmaf, predicted, crf)
        if persist:
            self.save()

    def save(self) -> None:
        """Persist the sidecar state to the configured cache path."""
        self.model.save(self.state_path)


__all__ = [
    "DEFAULT_PREDICTOR_VERSION",
    "FEATURE_DIM",
    "SIDECAR_SCHEMA_VERSION",
    "SidecarConfig",
    "SidecarModel",
    "SidecarPredictor",
    "get_or_create_host_uuid",
]
