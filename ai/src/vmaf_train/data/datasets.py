# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Dataset manifests for NFLX / KoNViD / LIVE-VQC / YouTube-UGC / BVI-DVC.

Manifests (`manifests/<name>.yaml`) declare the authoritative file list with
SHA-256 pins. The repo does not redistribute the data — consumers point
`VMAF_DATA_ROOT` at a pre-downloaded cache and populate their manifest with::

    vmaf-train manifest-scan --dataset <name> --root $VMAF_DATA_ROOT/<name>

See `vmaf_train.data.manifest_scan` for the scanner implementation.
"""

from __future__ import annotations

import os
from pathlib import Path

import yaml
from pydantic import BaseModel, ConfigDict, field_validator

DATASETS: dict[str, dict[str, str]] = {
    "nflx": {"capability": "C1/C2", "license": "Netflix research"},
    "konvid-1k": {"capability": "C2", "license": "CC BY 4.0"},
    "live-vqc": {"capability": "C2", "license": "Academic"},
    "youtube-ugc": {"capability": "C2", "license": "CC BY 3.0"},
    "bvi-dvc": {"capability": "C3", "license": "Academic"},
}


class ManifestEntry(BaseModel):
    """One row in a YAML dataset manifest (key + path + SHA-256 + MOS).

    Migrated from ``@dataclass(frozen=True)`` to ``pydantic.BaseModel``
    so the YAML loader (``load_manifest``) surfaces structured errors
    for missing/malformed SHA-256 strings or non-numeric MOS values.
    See ADR-0934.
    """

    model_config = ConfigDict(
        extra="forbid",
        frozen=True,
        validate_assignment=True,
    )

    key: str
    path: str
    sha256: str
    mos: float | None = None

    @field_validator("sha256")
    @classmethod
    def _sha256_shape(cls, v: str) -> str:
        # Hex digest of SHA-256 is always 64 lowercase hex chars; reject
        # eagerly so the malformed-line diagnostic points at the manifest
        # row instead of the much later SHA-mismatch failure.
        if len(v) != 64 or any(c not in "0123456789abcdef" for c in v.lower()):
            raise ValueError("sha256 must be a 64-char hex digest")
        return v.lower()


def data_root() -> Path:
    return Path(os.environ.get("VMAF_DATA_ROOT", Path.home() / ".cache" / "vmaf-train"))


def manifest_path(name: str) -> Path:
    here = Path(__file__).resolve().parent / "manifests"
    return here / f"{name}.yaml"


def load_manifest(name: str) -> list[ManifestEntry]:
    if name not in DATASETS:
        raise KeyError(f"unknown dataset: {name}")
    p = manifest_path(name)
    if not p.exists():
        return []
    # Pin UTF-8 explicitly so the parse does not silently drift across hosts
    # with mismatched LC_ALL / LANG (the open()-default encoding inherits the
    # process locale, which in CI sub-processes can land on ASCII and choke
    # on legitimate non-ASCII keys / paths in fork-added corpora).
    with p.open(encoding="utf-8") as fh:
        doc = yaml.safe_load(fh) or {}
    return [
        ManifestEntry(
            key=e["key"],
            path=e["path"],
            sha256=e["sha256"],
            mos=e.get("mos"),
        )
        for e in doc.get("entries", [])
    ]
