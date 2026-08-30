# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Hardware-device discovery helpers for vmaf-tune.

The helpers stay small and filesystem-driven so unit tests can inject a
temporary ``/dev/dri`` + ``/sys/class/drm`` view without requiring real GPUs.
"""

from __future__ import annotations

from pathlib import Path

AUTO_VAAPI_DEVICE: str = "auto"
FALLBACK_VAAPI_DEVICE: str = "/dev/dri/renderD128"
INTEL_PCI_VENDOR_ID: str = "0x8086"


def _candidate_render_nodes(dri_dir: Path) -> tuple[Path, ...]:
    """Return render nodes, preferring stable udev by-path entries."""
    candidates: list[Path] = []
    seen: set[Path] = set()
    by_path = dri_dir / "by-path"
    for entry in sorted(by_path.glob("*-render")):
        target = entry.resolve()
        if target.name.startswith("renderD") and target not in seen:
            candidates.append(target)
            seen.add(target)
    for entry in sorted(dri_dir.glob("renderD*")):
        target = entry.resolve()
        if target not in seen:
            candidates.append(target)
            seen.add(target)
    return tuple(candidates)


def _vendor_id_for_render_node(render_node: Path, sys_class_drm: Path) -> str:
    vendor_path = sys_class_drm / render_node.name / "device" / "vendor"
    try:
        return vendor_path.read_text(encoding="utf-8").strip().lower()
    except OSError:
        return ""


def discover_intel_vaapi_device(
    *,
    dri_dir: Path = Path("/dev/dri"),
    sys_class_drm: Path = Path("/sys/class/drm"),
) -> str | None:
    """Return the first Intel VA-API render node, or ``None`` if absent."""
    for node in _candidate_render_nodes(dri_dir):
        if _vendor_id_for_render_node(node, sys_class_drm) == INTEL_PCI_VENDOR_ID:
            return str(node)
    return None


def resolve_vaapi_device(
    requested: str | None = None,
    *,
    dri_dir: Path = Path("/dev/dri"),
    sys_class_drm: Path = Path("/sys/class/drm"),
    fallback: str = FALLBACK_VAAPI_DEVICE,
) -> str:
    """Resolve ``auto`` to the Intel render node, preserving explicit paths."""
    value = (requested or "").strip()
    if value and value != AUTO_VAAPI_DEVICE:
        return value
    return discover_intel_vaapi_device(dri_dir=dri_dir, sys_class_drm=sys_class_drm) or fallback


__all__ = [
    "AUTO_VAAPI_DEVICE",
    "FALLBACK_VAAPI_DEVICE",
    "INTEL_PCI_VENDOR_ID",
    "discover_intel_vaapi_device",
    "resolve_vaapi_device",
]
