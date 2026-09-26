# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""R3-8 regression: stale / feature-set-mismatched per-clip caches must not
silently misalign feature columns in extract_full_features.

Before the fix, ``main()`` zipped the *current* global ``FULL_FEATURES``
against a *cached* per-frame row with ``strict=False``, so a cache produced
when ``FULL_FEATURES`` had fewer columns silently truncated the trailing
columns with no error.  The fix (a) bakes the feature count into the cache
filename, (b) validates the cached ``feature_names`` against ``FULL_FEATURES``
on a cache hit (recompute on mismatch), and (c) zips the payload's own
``feature_names`` with ``strict=True``.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

pd = pytest.importorskip("pandas")

from ai.data.feature_extractor import FULL_FEATURES  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "extract_full_features.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("extract_full_features_cache_test", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _fake_executable(path: Path) -> Path:
    path.write_text("#!/bin/sh\n", encoding="utf-8")
    path.chmod(0o755)
    return path


def test_cache_path_carries_feature_count() -> None:
    """The per-clip cache key must include the feature-set count so a cache
    produced under a different FULL_FEATURES length misses instead of aliasing."""
    mod = _load_module()
    p = mod._per_clip_cache_path(Path("/cache"), "clip-a", "dis_stem")
    assert f".f{len(FULL_FEATURES)}." in p.name


def test_load_or_compute_cache_uses_strict_json(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module()
    pair = SimpleNamespace(
        source="clip-a",
        ref_path=tmp_path / "ref.yuv",
        dis_path=tmp_path / "dis.yuv",
        width=16,
        height=16,
    )
    values = np.full((1, len(FULL_FEATURES)), np.nan, dtype=np.float32)
    monkeypatch.setattr(
        mod,
        "extract_features",
        lambda *_args, **_kwargs: SimpleNamespace(
            feature_names=tuple(FULL_FEATURES), per_frame=values
        ),
    )
    monkeypatch.setattr(
        mod,
        "teacher_scores",
        lambda *_args, **_kwargs: SimpleNamespace(
            per_frame=np.asarray([float("inf")], dtype=np.float32)
        ),
    )

    mod._load_or_compute(pair, tmp_path / "cache", _fake_executable(tmp_path / "vmaf"))

    teacher_name = mod.resolve_teacher_model(None).name
    cache = mod._per_clip_cache_path(tmp_path / "cache", "clip-a", "dis", teacher_name)
    raw = cache.read_text(encoding="utf-8")
    assert "NaN" not in raw
    assert "Infinity" not in raw
    parsed = json.loads(raw)
    assert parsed["per_frame"][0][0] is None
    assert parsed["teacher_per_frame"] == [None]


def _plant_stale_cache_and_mocks(mod, tmp_path: Path, monkeypatch) -> Path:
    """Plant a stale two-column cache and mock a fresh full-column compute."""
    cache_dir = tmp_path / "cache"
    pair = SimpleNamespace(
        source="clip-a",
        ref_path=tmp_path / "ref.yuv",
        dis_path=tmp_path / "dis.yuv",
        width=16,
        height=16,
    )

    stale_path = mod._per_clip_cache_path(cache_dir, pair.source, pair.dis_path.stem)
    stale_path.parent.mkdir(parents=True, exist_ok=True)
    stale_path.write_text(
        json.dumps(
            {
                "feature_names": list(FULL_FEATURES[:2]),
                "per_frame": [[1.0, 2.0], [3.0, 4.0]],
                "teacher_per_frame": [80.0, 81.0],
            }
        )
    )

    fresh_per_frame = np.asarray(
        [[float(i) for i in range(len(FULL_FEATURES))]] * 2, dtype=np.float32
    )
    monkeypatch.setattr(mod, "iter_pairs", lambda _root, max_pairs=None: [pair])
    monkeypatch.setattr(
        mod,
        "extract_features",
        lambda *_a, **_k: SimpleNamespace(
            feature_names=tuple(FULL_FEATURES), per_frame=fresh_per_frame
        ),
    )
    monkeypatch.setattr(
        mod,
        "teacher_scores",
        lambda *_a, **_k: SimpleNamespace(per_frame=np.asarray([80.0, 81.0], dtype=np.float32)),
    )
    return cache_dir


def test_stale_short_cache_is_not_silently_truncated(tmp_path: Path, monkeypatch) -> None:
    """A feature-set-mismatched cache must be recomputed, not truncated."""
    mod = _load_module()
    out = tmp_path / "full_features.parquet"
    vmaf_bin = _fake_executable(tmp_path / "vmaf")
    cache_dir = _plant_stale_cache_and_mocks(mod, tmp_path, monkeypatch)

    rc = mod.main(
        [
            "--data-root",
            str(tmp_path / "netflix"),
            "--cache-dir",
            str(cache_dir),
            "--vmaf-bin",
            str(vmaf_bin),
            "--out",
            str(out),
        ]
    )
    assert rc == 0
    frame = pd.read_parquet(out)
    for col in FULL_FEATURES:
        assert col in frame.columns, f"missing column {col!r} — stale cache was truncated"
    last_col = FULL_FEATURES[-1]
    assert float(frame[last_col].iloc[0]) == float(len(FULL_FEATURES) - 1)
