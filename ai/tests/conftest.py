# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared pytest fixtures for ``ai/tests/``.

Builds tiny synthetic YUV files so unit tests run without the real 37 GB
corpus. The fixtures are byte-stable across runs (fixed RNG seed) so
cache-hit tests can compare deterministic outputs.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

# Make the top-level ``ai`` package importable regardless of how pytest
# was invoked (``pytest ai/tests`` from the repo root, or ``cd ai &&
# pytest tests``).
_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

# Make this conftest.py importable as ``conftest`` from individual test
# files (the pytest harness adds tests-dir to sys.path normally, but only
# *after* rootdir resolution — ensure the import works deterministically
# from any invocation directory so module-level ``from conftest import
# requires_pytorch_lightning`` succeeds).
_TESTS_DIR = Path(__file__).resolve().parent
if str(_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(_TESTS_DIR))


def _probe_pytorch_lightning() -> str | None:
    """Try importing pytorch_lightning. Return None on success, error str on failure.

    Importing pytorch_lightning eagerly pulls torchmetrics → torchvision.
    When the installed torchvision wheel was built against a different torch
    ABI than the one currently loaded, the import raises a ``RuntimeError``
    (``operator torchvision::nms does not exist``) at module-load time.

    ``pytest.importorskip`` only catches ``ImportError``, so a plain
    ``importorskip("pytorch_lightning")`` re-raises the RuntimeError and
    surfaces as a hard failure instead of a clean skip.

    This helper catches the broader ``Exception`` so tests can be skipped
    cleanly when the wheel pair is incompatible (e.g. torchvision 0.26.0
    against torch 2.12.0 — the matched pair is torchvision 0.27.0). The
    fix on the deployment side is ``pip install -U torchvision`` to pick
    up the wheel that matches the installed torch.
    """
    try:
        import pytorch_lightning  # noqa: F401
    except Exception as exc:  # pragma: no cover - depends on env
        return f"{type(exc).__name__}: {exc}"
    return None


# Memoise the probe — pytorch_lightning's import side-effects are global; if
# it fails once it will keep failing for the rest of the pytest session.
_PYTORCH_LIGHTNING_ERROR: str | None = _probe_pytorch_lightning()


def requires_pytorch_lightning() -> None:
    """Module-level guard for tests that need ``pytorch_lightning``.

    Call at the top of a test file (after the ``pytest`` import). If
    pytorch_lightning is unimportable for any reason — missing wheel,
    torch/torchvision ABI mismatch, etc. — the test module is skipped
    with a clear message instead of erroring at collection time.

    Usage::

        from conftest import requires_pytorch_lightning

        requires_pytorch_lightning()
    """
    if _PYTORCH_LIGHTNING_ERROR is not None:
        pytest.skip(
            f"pytorch_lightning unavailable: {_PYTORCH_LIGHTNING_ERROR}",
            allow_module_level=True,
        )


# Synthetic 16x16 yuv420p 8-bit frames keep the corpus tiny (384 B / frame).
SYNTH_W = 16
SYNTH_H = 16
SYNTH_FRAMES = 4
_FRAME_BYTES = SYNTH_W * SYNTH_H * 3 // 2


def _write_synth_yuv(path: Path, seed: int) -> None:
    rng = np.random.default_rng(seed)
    buf = rng.integers(0, 256, size=(SYNTH_FRAMES, _FRAME_BYTES), dtype=np.uint8)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(buf.tobytes())


@pytest.fixture(scope="session")
def mock_corpus(tmp_path_factory) -> Path:
    """Build a 2 ref + 4 dis synthetic corpus at a session-scoped tmp dir.

    The synthetic YUV is **not** valid for libvmaf (16x16 is below the
    feature-extractor minimum), but it exercises the pure-Python loader
    code paths (filename parsing, ref pairing, dimension probe).
    """
    root = tmp_path_factory.mktemp("netflix_mock")
    ref_dir = root / "ref"
    dis_dir = root / "dis"
    ref_dir.mkdir()
    dis_dir.mkdir()

    # 2 reference sources.
    _write_synth_yuv(ref_dir / "AlphaSrc_25fps.yuv", seed=1)
    _write_synth_yuv(ref_dir / "BetaSrc_30fps.yuv", seed=2)

    # 4 distorted clips (2 per source).
    _write_synth_yuv(dis_dir / "AlphaSrc_20_288_375.yuv", seed=10)
    _write_synth_yuv(dis_dir / "AlphaSrc_50_480_1050.yuv", seed=11)
    _write_synth_yuv(dis_dir / "BetaSrc_30_384_550.yuv", seed=12)
    _write_synth_yuv(dis_dir / "BetaSrc_85_1080_3800.yuv", seed=13)

    return root
