# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Hardware device discovery tests."""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.hw_devices import (  # noqa: E402
    AUTO_VAAPI_DEVICE,
    FALLBACK_VAAPI_DEVICE,
    discover_intel_vaapi_device,
    resolve_vaapi_device,
)


def _write_vendor(sys_class: Path, node: str, vendor: str) -> None:
    vendor_path = sys_class / node / "device" / "vendor"
    vendor_path.parent.mkdir(parents=True)
    vendor_path.write_text(vendor + "\n", encoding="utf-8")


def test_discover_intel_vaapi_device_prefers_by_path(tmp_path):
    dri = tmp_path / "dev" / "dri"
    sys_class = tmp_path / "sys" / "class" / "drm"
    dri.mkdir(parents=True)
    (dri / "by-path").mkdir()
    intel = dri / "renderD129"
    nvidia = dri / "renderD128"
    intel.touch()
    nvidia.touch()
    (dri / "by-path" / "pci-0000:06:00.0-render").symlink_to("../renderD128")
    (dri / "by-path" / "pci-0000:03:00.0-render").symlink_to("../renderD129")
    _write_vendor(sys_class, "renderD128", "0x10de")
    _write_vendor(sys_class, "renderD129", "0x8086")

    assert discover_intel_vaapi_device(dri_dir=dri, sys_class_drm=sys_class) == str(intel)


def test_resolve_vaapi_device_keeps_explicit_path(tmp_path):
    explicit = "/dev/dri/renderD777"
    assert resolve_vaapi_device(explicit, dri_dir=tmp_path, sys_class_drm=tmp_path) == explicit


def test_resolve_vaapi_device_falls_back_without_intel(tmp_path):
    dri = tmp_path / "dev" / "dri"
    sys_class = tmp_path / "sys" / "class" / "drm"
    dri.mkdir(parents=True)
    node = dri / "renderD128"
    node.touch()
    _write_vendor(sys_class, "renderD128", "0x10de")

    assert (
        resolve_vaapi_device(AUTO_VAAPI_DEVICE, dri_dir=dri, sys_class_drm=sys_class)
        == FALLBACK_VAAPI_DEVICE
    )
