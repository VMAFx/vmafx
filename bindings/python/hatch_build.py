# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hatchling custom build hook: set platform wheel tags when bundling libvmafx."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class CustomBuildHook(BuildHookInterface):
    """Set platform wheel tags only if compiled libraries are present."""

    def initialize(self, version: str, build_data: dict[str, Any]) -> None:
        if self.target_name != "wheel":
            return
        pkg_dir = Path(self.root) / "vmafx"
        has_binary = any(
            p.suffix in {".so", ".dylib", ".dll"} or ".so." in p.name
            for p in pkg_dir.iterdir()
            if p.is_file()
        )
        if has_binary:
            build_data["infer_tag"] = True
            build_data["pure_python"] = False
