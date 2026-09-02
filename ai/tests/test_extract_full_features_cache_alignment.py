# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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


def test_stale_short_cache_is_not_silently_truncated(tmp_path: Path, monkeypatch) -> None:
    """A cache whose stored feature_names no longer match FULL_FEATURES must be
    recomputed, not zipped (truncated) against the current FULL_FEATURES."""
    mod = _load_module()
    cache_dir = tmp_path / "cache"
    out = tmp_path / "full_features.parquet"
    vmaf_bin = _fake_executable(tmp_path / "vmaf")
    pair = SimpleNamespace(
        source="clip-a",
        ref_path=tmp_path / "ref.yuv",
        dis_path=tmp_path / "dis.yuv",
        width=16,
        height=16,
    )

    # Plant a STALE cache at the *fixed-name* location with only the first two
    # FULL_FEATURES columns (simulating a pre-ADR-0559 22-col cache).  This is
    # the exact poison the old strict=False zip swallowed.
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

    # Fresh compute returns the FULL set (all columns) — proving the stale cache
    # was rejected and recomputed rather than used.
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
    # Every FULL_FEATURES column must be present and populated (no silent
    # truncation of the trailing speed_* columns).
    for col in FULL_FEATURES:
        assert col in frame.columns, f"missing column {col!r} — stale cache was truncated"
    # The last column must carry the fresh value, not be dropped.
    last_col = FULL_FEATURES[-1]
    assert float(frame[last_col].iloc[0]) == float(len(FULL_FEATURES) - 1)
