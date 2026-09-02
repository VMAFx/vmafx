# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage tests for :mod:`vmaf_train.data.datasets`.

The module is a thin dataset-manifest loader (no torch, no ONNX). It had no
direct test file before this; coverage came incidentally through importers.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from pydantic import ValidationError

from vmaf_train.data import datasets


def test_datasets_table_lists_all_five_corpora() -> None:
    assert set(datasets.DATASETS.keys()) == {
        "nflx",
        "konvid-1k",
        "live-vqc",
        "youtube-ugc",
        "bvi-dvc",
    }
    for entry in datasets.DATASETS.values():
        assert "capability" in entry
        assert "license" in entry


def test_data_root_honours_vmaf_data_root_env(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setenv("VMAF_DATA_ROOT", str(tmp_path / "custom"))
    assert datasets.data_root() == Path(str(tmp_path / "custom"))


def test_data_root_falls_back_to_home_cache(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("VMAF_DATA_ROOT", raising=False)
    root = datasets.data_root()
    assert root == Path.home() / ".cache" / "vmaf-train"


def test_manifest_path_returns_yaml_under_module_dir() -> None:
    p = datasets.manifest_path("nflx")
    assert p.name == "nflx.yaml"
    assert p.parent.name == "manifests"


def test_load_manifest_rejects_unknown_dataset() -> None:
    with pytest.raises(KeyError, match="unknown dataset"):
        datasets.load_manifest("does-not-exist")


def test_load_manifest_returns_empty_when_no_yaml(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Branch: ``manifest_path(name).exists()`` is False → return ``[]``."""
    missing = tmp_path / "nope.yaml"
    monkeypatch.setattr(datasets, "manifest_path", lambda name: missing)
    assert datasets.load_manifest("nflx") == []


# Valid 64-char lowercase hex sha256 digests for test fixtures.
# ManifestEntry._sha256_shape enforces the 64-char hex contract (added in PR #506).
_SHA256_A = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
_SHA256_B = "cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe"
_SHA256_K = "0" * 64


def test_load_manifest_parses_entries(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    dst = tmp_path / "nflx.yaml"
    dst.write_text(
        "entries:\n"
        "  - key: a\n"
        "    path: a.yuv\n"
        f"    sha256: {_SHA256_A}\n"
        "    mos: 50.5\n"
        "  - key: b\n"
        "    path: b.yuv\n"
        f"    sha256: {_SHA256_B}\n"
        # mos omitted → defaults to None
    )
    monkeypatch.setattr(datasets, "manifest_path", lambda name: dst)
    entries = datasets.load_manifest("nflx")
    assert len(entries) == 2
    assert entries[0] == datasets.ManifestEntry(key="a", path="a.yuv", sha256=_SHA256_A, mos=50.5)
    assert entries[1].mos is None


def test_load_manifest_handles_empty_yaml_document(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Branch: ``yaml.safe_load`` returns ``None`` for empty doc → coerced to ``{}``."""
    dst = tmp_path / "nflx.yaml"
    dst.write_text("")
    monkeypatch.setattr(datasets, "manifest_path", lambda name: dst)
    assert datasets.load_manifest("nflx") == []


def test_load_manifest_handles_yaml_missing_entries_key(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Branch: doc exists but lacks the ``entries`` key."""
    dst = tmp_path / "nflx.yaml"
    dst.write_text("name: nflx\n")
    monkeypatch.setattr(datasets, "manifest_path", lambda name: dst)
    assert datasets.load_manifest("nflx") == []


def test_manifest_entry_is_frozen() -> None:
    entry = datasets.ManifestEntry(key="k", path="p", sha256=_SHA256_K)
    # ManifestEntry uses pydantic.BaseModel with frozen=True (ConfigDict).
    # Pydantic raises ValidationError (not dataclasses.FrozenInstanceError)
    # when a frozen field is reassigned — pydantic's __setattr__ calls
    # _check_frozen which raises ValidationError.from_exception_data with
    # type='frozen_instance'.  dataclasses.FrozenInstanceError is an
    # AttributeError subclass and was the right check before the migration
    # to pydantic in PR #506 (ADR-0934).
    with pytest.raises(ValidationError):
        entry.key = "other"  # type: ignore[misc]


def test_manifest_entry_mos_defaults_to_none() -> None:
    assert datasets.ManifestEntry(key="k", path="p", sha256=_SHA256_K).mos is None
