# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""PyTorch :class:`Dataset` adapter over the Netflix corpus loader.

Each sample is one *frame* — feature vector :math:`x \\in \\mathbb{R}^{6}`
paired with the ``vmaf_v0.6.1`` per-frame score :math:`y \\in [0, 100]`.

Train / val split
-----------------

Per ADR-0203 we hold out **one source** (default ``Tennis_24fps``) for
validation. Rationale:

* The corpus has 9 sources × 70 distortions; 1-source-out gives ~88 %
  train / ~12 % val by clip count, which is close to the canonical 90/10
  split documented in ``docs/ai/training.md`` while keeping the val set
  *content-disjoint* from training.
* k-fold (leave-one-source-out, 9 folds) is more thorough but adds 9x
  training cost. The user can switch by setting ``--val-source`` to
  each source in turn and aggregating results manually.
* A pure random split would let frames from the same clip leak into
  both halves, inflating PLCC by 5-10 percentage points (well-known
  pitfall in VQA training).

The split is deterministic: the hold-out source name is the only knob.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np

try:  # torch is a heavy dep; tests can skip when unavailable.
    import torch
    from torch.utils.data import Dataset

    _HAS_TORCH = True
except ImportError:  # pragma: no cover - exercised only on stripped CI envs
    _HAS_TORCH = False

    class Dataset:  # type: ignore[no-redef]
        pass


from ..data.feature_extractor import DEFAULT_FEATURES, FeatureExtractionResult, extract_features
from ..data.netflix_loader import NetflixPair, iter_pairs, load_or_compute
from ..data.scores import TeacherScores, teacher_scores

DEFAULT_VAL_SOURCE = "Tennis"


def _make_zero_payload(pair: NetflixPair) -> dict:
    """Tiny zero-feature payload for ``--epochs 0`` smoke runs.

    Used by :func:`ai.train.train.main` when a fake corpus is supplied
    and there is no built ``vmaf`` binary. Yields one frame so the
    dataset is non-empty (downstream length-zero handling works either
    way, but a non-zero length exercises the full code path).
    """
    n_features = len(DEFAULT_FEATURES)
    return {
        "features": {
            "feature_names": list(DEFAULT_FEATURES),
            "per_frame": [[0.0] * n_features],
            "n_frames": 1,
        },
        "scores": {"per_frame": [0.0], "pooled": 0.0},
    }


@dataclass
class FrameSample:
    """One ``(features, target)`` pair plus debug-friendly metadata."""

    features: np.ndarray  # (n_features,) float32
    target: float
    source: str
    dis_basename: str
    frame_index: int


def _compute_pair_payload(
    pair: NetflixPair,
    *,
    features: tuple[str, ...],
    vmaf_binary: Path | None,
) -> dict:
    """Compute features + teacher scores for one pair (cache-miss path)."""
    feats: FeatureExtractionResult = extract_features(
        pair.ref_path,
        pair.dis_path,
        pair.width,
        pair.height,
        features=features,
        vmaf_binary=vmaf_binary,
    )
    teacher: TeacherScores = teacher_scores(
        pair.ref_path,
        pair.dis_path,
        pair.width,
        pair.height,
        vmaf_binary=vmaf_binary,
    )
    return {
        "features": feats.to_jsonable(),
        "scores": teacher.to_jsonable(),
    }


def _payload_to_arrays(
    payload: dict,
) -> tuple[np.ndarray, np.ndarray]:
    feats = FeatureExtractionResult.from_jsonable(payload["features"])
    teacher = TeacherScores.from_jsonable(payload["scores"])
    n = min(feats.per_frame.shape[0], teacher.per_frame.shape[0])
    return feats.per_frame[:n], teacher.per_frame[:n]


class NetflixFrameDataset(Dataset):  # type: ignore[misc]
    """One sample per frame across all eligible (ref, dis) pairs.

    Parameters
    ----------
    data_root:
        Root directory with ``ref/`` and ``dis/`` subdirectories.
    split:
        ``"train"`` or ``"val"``. Selects pairs whose source is *not*
        equal to ``val_source`` (train) or *equal* to it (val).
    val_source:
        Source name held out for validation. Defaults to
        :data:`DEFAULT_VAL_SOURCE`.
    sources:
        Optional whitelist of source names. Useful for tests.
    max_pairs:
        Cap on number of pairs (smoke / CI). ``None`` = all.
    use_cache:
        If True (default), persist per-clip JSON under
        ``$VMAF_TINY_AI_CACHE`` and reuse on repeated runs.
    payload_provider:
        Optional callable ``(NetflixPair) -> dict`` overriding the
        default libvmaf-driven path. Tests inject this to avoid the
        binary dependency.
    """

    def __init__(
        self,
        data_root: Path,
        *,
        split: str = "train",
        val_source: str = DEFAULT_VAL_SOURCE,
        sources: tuple[str, ...] | None = None,
        max_pairs: int | None = None,
        features: tuple[str, ...] = DEFAULT_FEATURES,
        vmaf_binary: Path | None = None,
        use_cache: bool = True,
        payload_provider=None,  # type: ignore[no-untyped-def]
        assume_dims: tuple[int, int] | None = None,
    ) -> None:
        if split not in ("train", "val"):
            raise ValueError(f"split must be 'train' or 'val', got {split!r}")
        self.data_root = Path(data_root)
        self.split = split
        self.val_source = val_source
        self.features = features
        self._samples: list[FrameSample] = []

        provider = payload_provider or (
            lambda p: _compute_pair_payload(p, features=features, vmaf_binary=vmaf_binary)
        )

        for pair in iter_pairs(
            self.data_root,
            sources=sources,
            max_pairs=max_pairs,
            assume_dims=assume_dims,
        ):
            is_val = pair.source == val_source
            if split == "train" and is_val:
                continue
            if split == "val" and not is_val:
                continue
            payload = load_or_compute(pair, provider, use_cache=use_cache)
            feat_arr, target_arr = _payload_to_arrays(payload)
            for i in range(feat_arr.shape[0]):
                self._samples.append(
                    FrameSample(
                        features=feat_arr[i].astype(np.float32),
                        target=float(target_arr[i]),
                        source=pair.source,
                        dis_basename=pair.dis_path.name,
                        frame_index=i,
                    )
                )

    def __len__(self) -> int:
        return len(self._samples)

    def __getitem__(self, idx: int):  # type: ignore[no-untyped-def]
        s = self._samples[idx]
        if _HAS_TORCH:
            x = torch.from_numpy(np.ascontiguousarray(s.features))
            y = torch.tensor(s.target, dtype=torch.float32)
            return x, y
        return s.features, np.float32(s.target)

    @property
    def feature_dim(self) -> int:
        return len(self.features)

    def numpy_arrays(self) -> tuple[np.ndarray, np.ndarray]:
        """Stack all samples into ``(X, y)`` for non-torch consumers."""
        if not self._samples:
            return (
                np.zeros((0, self.feature_dim), dtype=np.float32),
                np.zeros((0,), dtype=np.float32),
            )
        x = np.stack([s.features for s in self._samples], axis=0)
        y = np.asarray([s.target for s in self._samples], dtype=np.float32)
        return x, y
