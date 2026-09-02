# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Defaults for the AI feature-extraction binary path."""

from __future__ import annotations

from pathlib import Path

from ai.data import feature_extractor
from vmaf_train.data import feature_dump


def test_default_vmaf_binary_uses_fork_cpu_build(monkeypatch) -> None:
    monkeypatch.delenv("VMAF_BIN", raising=False)

    assert feature_extractor.default_vmaf_binary() == (
        Path("core") / "build-cpu" / "tools" / "vmaf"
    )


def test_default_vmaf_binary_respects_env(monkeypatch, tmp_path: Path) -> None:
    custom = tmp_path / "custom-vmaf"
    monkeypatch.setenv("VMAF_BIN", str(custom))

    assert feature_extractor.default_vmaf_binary() == custom


def test_vmaf_train_feature_dump_default_uses_fork_cpu_build() -> None:
    assert (Path("core") / "build-cpu" / "tools" / "vmaf") == feature_dump.DEFAULT_VMAF_BINARY
