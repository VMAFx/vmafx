#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared direct-invocation bootstrap for ``ai/scripts`` modules."""

import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ScriptBootstrapPaths:
    """Resolved paths for an AI script invocation."""

    script_path: Path
    repo_root: Path
    ai_dir: Path
    ai_src: Path
    ai_scripts: Path
    vmaf_tune_src: Path


def _prepend_path(path: Path) -> None:
    path_text = str(path)
    if path_text not in sys.path:
        sys.path.insert(0, path_text)


def bootstrap_ai_script(
    file_path: str | Path,
    *,
    include_repo_root: bool = False,
    include_ai_src: bool = True,
    include_ai_scripts: bool = False,
    include_vmaf_tune_src: bool = False,
) -> ScriptBootstrapPaths:
    """Add standard repo-local import roots for a directly executed AI script.

    ``python ai/scripts/foo.py`` starts with ``ai/scripts`` on ``sys.path`` but
    not necessarily ``ai/src`` or the repository root. Scripts that import
    ``aiutils`` or sibling materializers should call this once before those
    imports instead of repeating ad hoc path mutation blocks.
    """
    script_path = Path(file_path).resolve()
    repo_root = script_path.parents[2]
    ai_dir = repo_root / "ai"
    paths = ScriptBootstrapPaths(
        script_path=script_path,
        repo_root=repo_root,
        ai_dir=ai_dir,
        ai_src=ai_dir / "src",
        ai_scripts=ai_dir / "scripts",
        vmaf_tune_src=repo_root / "tools" / "vmaf-tune" / "src",
    )

    if include_repo_root:
        _prepend_path(paths.repo_root)
    if include_ai_src:
        _prepend_path(paths.ai_src)
    if include_ai_scripts:
        _prepend_path(paths.ai_scripts)
    if include_vmaf_tune_src:
        _prepend_path(paths.vmaf_tune_src)
    return paths
