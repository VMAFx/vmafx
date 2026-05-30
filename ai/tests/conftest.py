# Copyright 2026 Lusoris and Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared pytest fixtures for ``ai/tests/``.

Builds tiny synthetic YUV files so unit tests run without the real 37 GB
corpus. The fixtures are byte-stable across runs (fixed RNG seed) so
cache-hit tests can compare deterministic outputs.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from types import ModuleType

import numpy as np
import pytest

# Make the top-level ``ai`` package importable regardless of how pytest
# was invoked (``pytest ai/tests`` from the repo root, or ``cd ai &&
# pytest tests``).
_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

# Directory that holds the standalone scripts the test suite exercises via
# ``load_ai_script``. Scripts under ``ai/scripts/`` are not part of an
# importable package (they're top-level entry points), so tests load them
# dynamically through ``importlib`` rather than ``import ai.scripts.…``.
_AI_DIR = _REPO_ROOT / "ai"
_AI_SCRIPTS_DIR = _AI_DIR / "scripts"


def _exec_at_path(script_path: Path, *, name: str) -> ModuleType:
    """Internal: build a spec for ``script_path`` and exec it under ``name``."""
    spec = importlib.util.spec_from_file_location(name, script_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot build spec for {script_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def load_ai_script(rel_path: str, *, name: str | None = None) -> ModuleType:
    """Dynamically load an ``ai/scripts/<rel_path>`` module.

    ``rel_path`` is the script filename (with or without the ``.py``
    suffix), resolved against ``ai/scripts/``. ``name`` overrides the
    derived module name — pass it when the original test pinned a
    specific name in ``sys.modules`` (e.g. ``"ptq_dynamic_under_test"``).

    The loaded module is registered in ``sys.modules`` under its
    resolved name so that pickling, dataclass repr, and intra-script
    self-imports keep working. Returns the loaded module.

    Replaces the ``spec_from_file_location → module_from_spec →
    exec_module`` boilerplate that previously lived in each test file.
    For scripts that live outside ``ai/scripts/`` (e.g. ``ai/lpips_export.py``)
    use :func:`load_ai_module` instead.
    """
    if not rel_path.endswith(".py"):
        rel_path = rel_path + ".py"
    script_path = _AI_SCRIPTS_DIR / rel_path
    derived_name = name or rel_path[:-3].replace("/", "_")
    return _exec_at_path(script_path, name=derived_name)


def load_ai_module(rel_path: str, *, name: str | None = None) -> ModuleType:
    """Dynamically load any module under ``ai/<rel_path>``.

    Companion to :func:`load_ai_script` for files that live outside
    ``ai/scripts/`` — e.g. ``ai/lpips_export.py``. Resolves ``rel_path``
    against the ``ai/`` package root.
    """
    if not rel_path.endswith(".py"):
        rel_path = rel_path + ".py"
    script_path = _AI_DIR / rel_path
    derived_name = name or rel_path[:-3].replace("/", "_")
    return _exec_at_path(script_path, name=derived_name)


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
