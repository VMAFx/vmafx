# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the QAT training-loader selection in ``ai/scripts/qat_train.py``.

Research-2029 gap 4: ``_build_train_loader_factory`` used to hand every
config to ``VmafTrainDataModule``, which only materialises rank-2 tabular
feature rows. A 2D CNN (``learned_filter``, ``nr_metric``) needs NCHW image
batches, so ``learned_filter_v1_qat.yaml`` only ever "worked" because its
parquet cache is uncommitted and the missing-cache branch downgraded the run
to smoke mode. These tests pin the rank-driven dispatch and the rank-4
``.npz`` contract.
"""

from __future__ import annotations

import importlib.util
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "qat_train.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("qat_train_under_test", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


QT = _load_module()


@dataclass
class _FakeQatConfig:
    """Stand-in for ``ai.train.qat.QatConfig`` — only the fields the loader reads."""

    smoke: bool = False
    extra: dict[str, Any] = field(default_factory=dict)


def _write_npz(path: Path, n: int = 6, c: int = 1, h: int = 8, w: int = 8, **extra: Any) -> Path:
    rng = np.random.default_rng(0)
    payload: dict[str, Any] = {
        "x": rng.random((n, c, h, w), dtype=np.float32),
        "y": rng.random((n, c, h, w), dtype=np.float32),
    }
    payload.update(extra)
    np.savez(path, **payload)
    return path


# ---------------------------------------------------------------------------
# _config_input_rank
# ---------------------------------------------------------------------------


def test_input_rank_reads_qat_input_shape() -> None:
    cfg = {"qat": {"input_shape": [1, 1, 32, 32]}}
    assert QT._config_input_rank(cfg, _FakeQatConfig()) == 4
    cfg = {"qat": {"input_shape": [1, 6]}}
    assert QT._config_input_rank(cfg, _FakeQatConfig()) == 2


def test_input_rank_falls_back_to_extra_then_rank4() -> None:
    """Absent ``qat.input_shape``, the QatConfig copy wins; absent both, rank 4.

    Rank 4 is the correct default because ``_build_example_inputs`` traces with
    ``[1, 1, 32, 32]`` in exactly that case.
    """
    assert QT._config_input_rank({}, _FakeQatConfig(extra={"input_shape": [1, 6]})) == 2
    assert QT._config_input_rank({}, _FakeQatConfig()) == 4


# ---------------------------------------------------------------------------
# dispatch in _build_train_loader_factory
# ---------------------------------------------------------------------------


def test_smoke_mode_needs_no_loader() -> None:
    assert QT._build_train_loader_factory({"cache": "nope.npz"}, _FakeQatConfig(smoke=True)) is None


def test_missing_cache_downgrades_to_smoke(tmp_path: Path) -> None:
    qat_cfg = _FakeQatConfig()
    cfg = {"cache": str(tmp_path / "absent.npz"), "qat": {"input_shape": [1, 1, 8, 8]}}
    assert QT._build_train_loader_factory(cfg, qat_cfg) is None
    assert qat_cfg.smoke is True


def test_rank4_config_routes_to_the_image_loader(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    cache = _write_npz(tmp_path / "imgs.npz")
    seen: list[Path] = []
    monkeypatch.setattr(
        QT, "_build_image_loader_factory", lambda cfg, path: seen.append(path) or "SENTINEL"
    )
    cfg = {"cache": str(cache), "qat": {"input_shape": [1, 1, 8, 8]}}
    assert QT._build_train_loader_factory(cfg, _FakeQatConfig()) == "SENTINEL"
    assert seen == [cache]


def test_rank2_config_still_routes_to_the_tabular_datamodule(tmp_path: Path) -> None:
    """Rank-2 must keep using VmafTrainDataModule — the FR-regressor path."""
    pytest.importorskip("torch")
    from conftest import requires_pytorch_lightning

    requires_pytorch_lightning()
    cache = tmp_path / "features.parquet"
    cache.write_bytes(b"not-a-real-parquet")
    cfg = {"cache": str(cache), "qat": {"input_shape": [1, 6]}}
    factory = QT._build_train_loader_factory(cfg, _FakeQatConfig())
    assert factory is not None
    assert factory is not QT._build_image_loader_factory
    # The tabular factory imports the Lightning datamodule lazily; asserting the
    # dispatch (a callable, not None, not the image factory) is the contract here.


def test_unsupported_rank_downgrades_to_smoke(tmp_path: Path) -> None:
    cache = _write_npz(tmp_path / "imgs.npz")
    qat_cfg = _FakeQatConfig()
    cfg = {"cache": str(cache), "qat": {"input_shape": [1, 1, 4, 8, 8]}}
    assert QT._build_train_loader_factory(cfg, qat_cfg) is None
    assert qat_cfg.smoke is True


# ---------------------------------------------------------------------------
# _build_image_loader_factory contract
# ---------------------------------------------------------------------------


def test_image_loader_rejects_non_npz(tmp_path: Path) -> None:
    cache = tmp_path / "imgs.parquet"
    cache.write_bytes(b"x")
    with pytest.raises(SystemExit, match=r"must be a \.npz"):
        QT._build_image_loader_factory({}, cache)


def test_image_loader_rejects_unknown_array_names(tmp_path: Path) -> None:
    cache = tmp_path / "bad.npz"
    np.savez(cache, foo=np.zeros((2, 1, 4, 4), dtype=np.float32))
    with pytest.raises(SystemExit, match="needs an input array"):
        QT._build_image_loader_factory({}, cache)


def test_image_loader_rejects_non_4d_input(tmp_path: Path) -> None:
    cache = tmp_path / "flat.npz"
    np.savez(
        cache,
        x=np.zeros((4, 6), dtype=np.float32),
        y=np.zeros((4,), dtype=np.float32),
    )
    with pytest.raises(SystemExit, match="must be 4D NCHW"):
        QT._build_image_loader_factory({}, cache)


def test_image_loader_rejects_length_mismatch(tmp_path: Path) -> None:
    cache = tmp_path / "mismatch.npz"
    np.savez(
        cache,
        x=np.zeros((4, 1, 4, 4), dtype=np.float32),
        y=np.zeros((3, 1, 4, 4), dtype=np.float32),
    )
    with pytest.raises(SystemExit, match="length mismatch"):
        QT._build_image_loader_factory({}, cache)


def test_image_loader_rejects_empty_cache(tmp_path: Path) -> None:
    cache = tmp_path / "empty.npz"
    np.savez(
        cache,
        x=np.zeros((0, 1, 4, 4), dtype=np.float32),
        y=np.zeros((0, 1, 4, 4), dtype=np.float32),
    )
    with pytest.raises(SystemExit, match="empty"):
        QT._build_image_loader_factory({}, cache)


def test_image_loader_accepts_alias_array_names(tmp_path: Path) -> None:
    pytest.importorskip("torch")
    cache = tmp_path / "alias.npz"
    np.savez(
        cache,
        degraded=np.zeros((4, 1, 8, 8), dtype=np.float32),
        clean=np.zeros((4, 1, 8, 8), dtype=np.float32),
    )
    loader = QT._build_image_loader_factory({"batch_size": 2}, cache)()
    xb, yb = next(iter(loader))
    assert tuple(xb.shape) == (2, 1, 8, 8)
    assert tuple(yb.shape) == (2, 1, 8, 8)


def test_image_loader_yields_nchw_float32_batches(tmp_path: Path) -> None:
    """The synthetic-config end-to-end check: 6 samples, batch 4 -> 4 + 2."""
    torch = pytest.importorskip("torch")
    cache = _write_npz(tmp_path / "imgs.npz", n=6, c=1, h=8, w=8)
    loader = QT._build_image_loader_factory({"batch_size": 4}, cache)()
    batches = list(loader)
    assert [tuple(x.shape) for x, _ in batches] == [(4, 1, 8, 8), (2, 1, 8, 8)]
    assert batches[0][0].dtype is torch.float32
    assert batches[0][1].dtype is torch.float32


def test_image_loader_batches_feed_the_learned_filter_model(tmp_path: Path) -> None:
    """The gap this closes: rank-4 batches must survive the first Conv2d.

    Tabular rows from ``VmafTrainDataModule`` cannot; that is why
    ``learned_filter_v1_qat.yaml`` never trained for real.
    """
    pytest.importorskip("torch")
    from conftest import requires_pytorch_lightning

    requires_pytorch_lightning()
    import sys as _sys

    for extra in (_REPO_ROOT, _REPO_ROOT / "ai" / "src"):
        if str(extra) not in _sys.path:
            _sys.path.insert(0, str(extra))
    from ai.src.vmaf_train.models import LearnedFilter

    cache = _write_npz(tmp_path / "imgs.npz", n=4, c=1, h=32, w=32)
    loader = QT._build_image_loader_factory({"batch_size": 2}, cache)()
    model = LearnedFilter(channels=1, width=4, num_blocks=1)
    xb, _yb = next(iter(loader))
    out = model(xb)
    assert out.shape[0] == 2
