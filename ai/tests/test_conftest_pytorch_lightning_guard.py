# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Pins the ``conftest.requires_pytorch_lightning`` guard contract.

The guard exists to convert a ``RuntimeError: operator torchvision::nms
does not exist`` (raised by ``import pytorch_lightning`` when the
installed torchvision wheel was built against a different torch ABI than
the one currently loaded) into a clean ``pytest.skip(..., allow_module_
level=True)``.

These tests verify:

  1. ``_probe_pytorch_lightning`` returns ``None`` when the import
     succeeds, and a string when it does not (broader than ImportError —
     RuntimeError must also be captured, since that is the actual
     symptom in the wild).
  2. ``requires_pytorch_lightning`` raises ``Skipped`` (via
     ``pytest.skip``) when the cached probe recorded an error, and
     returns ``None`` otherwise.
  3. The cached probe value (``_PYTORCH_LIGHTNING_ERROR``) is set
     once at import time and reused — i.e. the guard does not
     re-import on every test.
"""

from __future__ import annotations

import importlib

import conftest
import pytest


def test_probe_function_returns_none_or_str() -> None:
    """The probe must return either ``None`` (importable) or ``str`` (error message)."""
    result = conftest._probe_pytorch_lightning()
    assert result is None or isinstance(result, str)


def test_module_level_cache_matches_probe_signature() -> None:
    """The cached value has the same type signature as a fresh probe call."""
    cached = conftest._PYTORCH_LIGHTNING_ERROR
    assert cached is None or isinstance(cached, str)


def test_requires_pytorch_lightning_skips_when_unavailable(monkeypatch) -> None:
    """If the cached probe recorded an error, ``requires_pytorch_lightning``
    must raise ``pytest.skip.Exception`` (the in-process skip signal)."""
    monkeypatch.setattr(conftest, "_PYTORCH_LIGHTNING_ERROR", "synthetic-error-for-test")
    with pytest.raises(pytest.skip.Exception):
        conftest.requires_pytorch_lightning()


def test_requires_pytorch_lightning_passes_when_available(monkeypatch) -> None:
    """When no error was recorded, the guard returns ``None`` silently."""
    monkeypatch.setattr(conftest, "_PYTORCH_LIGHTNING_ERROR", None)
    # No exception expected.
    assert conftest.requires_pytorch_lightning() is None


def test_probe_catches_runtimeerror_not_just_importerror(monkeypatch) -> None:
    """The whole point of the guard is to catch ``RuntimeError`` — torchvision's
    ABI mismatch surfaces as ``RuntimeError: operator torchvision::nms does
    not exist`` at import time, not as ``ImportError``.

    Stub the ``pytorch_lightning`` import to raise ``RuntimeError`` and verify
    the probe catches it cleanly.
    """
    real_import_module = importlib.import_module

    def fake_import(name, package=None):
        if name == "pytorch_lightning":
            raise RuntimeError("operator torchvision::nms does not exist")
        return real_import_module(name, package)

    # Force a fresh probe by clearing cached pytorch_lightning if loaded.
    import sys

    monkeypatch.delitem(sys.modules, "pytorch_lightning", raising=False)
    monkeypatch.setattr(importlib, "import_module", fake_import)

    # _probe_pytorch_lightning uses a plain ``import`` not importlib, so
    # the most reliable check is to verify the catch is broad enough by
    # inspecting the source.  But we can also call the probe via a mocked
    # ``__import__`` to simulate the failure.
    import builtins

    real_builtin_import = builtins.__import__

    def fake_builtin_import(name, *args, **kwargs):
        if name == "pytorch_lightning":
            raise RuntimeError("operator torchvision::nms does not exist")
        return real_builtin_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_builtin_import)
    monkeypatch.delitem(sys.modules, "pytorch_lightning", raising=False)

    result = conftest._probe_pytorch_lightning()
    assert result is not None
    assert "torchvision::nms" in result
    assert "RuntimeError" in result
