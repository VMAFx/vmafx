#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Add the GStreamer plugin and the FFmpeg filter sources to compile_commands.json.

The `integrations` tidy lane (#2860) measures two trees the core's meson build never
compiles; scripts/ci/tidy-integrations-build.sh builds both next to the core build:

1. ``<build>/gst/compile_commands.json``: the GStreamer plugin (``gstreamer/src/``),
   whose entries are taken as meson wrote them.
2. ``<build>/ffmpeg-objs.log``: FFmpeg's ``make V=1`` lines for the objects built from
   ``ffmpeg-patches/src/``. The patch replay copies each source into the FFmpeg tree,
   so an entry's ``file`` is pointed back at the repository copy and the include
   directory the tree copy found its neighbours in is added; the dependency-file
   switches are dropped.

Usage: gen-integrations-compile-commands.py <build-dir>

Existing entries for the same files are replaced. A missing input, a missing plugin
source or a missing FFmpeg compile line exits 1, so the lane cannot go clean by
measuring nothing.
"""

from __future__ import annotations

import json
import shlex
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GST_SOURCE_DIR = Path("gstreamer/src")
FFMPEG_SOURCE_DIR = Path("ffmpeg-patches/src")
# Tree path of each FFmpeg source -> its file under ffmpeg-patches/src/ (the SOURCES
# map of scripts/ci/ffmpeg_patch_stack.py, .c files only).
FFMPEG_UNITS = {
    "libavfilter/vf_vmafx.c": "vf_vmafx.c",
    "libavfilter/vf_vmafx_pre.c": "vf_vmafx_pre.c",
    "fftools/ffmpeg_dec_hwaccel.c": "ffmpeg_dec_hwaccel.c",
}
# Switches that only write make's dependency files; each of the -MF / -MT forms
# takes the next argument.
DEPENDENCY_FLAGS = frozenset({"-MMD", "-MD", "-MP"})
DEPENDENCY_FLAGS_WITH_VALUE = frozenset({"-MF", "-MT", "-MQ"})
EXPECTED_ARGC = 2  # the script and the build directory


def without_dependency_flags(argv: list[str]) -> list[str]:
    """*argv* without make's dependency-file switches."""
    kept: list[str] = []
    skip_next = False
    for arg in argv:
        if skip_next:
            skip_next = False
        elif arg in DEPENDENCY_FLAGS_WITH_VALUE:
            skip_next = True
        elif arg not in DEPENDENCY_FLAGS:
            kept.append(arg)
    return kept


def ffmpeg_compile_lines(log: str) -> dict[str, list[str]]:
    """The compile argv of each FFMPEG_UNITS source found in a ``make V=1`` log."""
    found: dict[str, list[str]] = {}
    for line in log.splitlines():
        if " -c " not in f" {line} ":
            continue
        try:
            argv = shlex.split(line)
        except ValueError:
            continue
        for tree_path in FFMPEG_UNITS:
            if argv and argv[-1] in (tree_path, f"./{tree_path}"):
                found[tree_path] = argv
    return found


def ffmpeg_entry(tree_path: str, argv: list[str], tree: Path, repo_root: Path) -> dict[str, str]:
    """The entry measuring the repository copy of *tree_path* with FFmpeg's flags."""
    source = repo_root / FFMPEG_SOURCE_DIR / FFMPEG_UNITS[tree_path]
    compile_argv = without_dependency_flags(argv[:-1])
    # The tree copy found `"filters.h"` and friends next to itself; the repository
    # copy needs that directory spelled out, ahead of the tree root.
    compile_argv[1:1] = [f"-I{tree / Path(tree_path).parent}", f"-I{tree}"]
    return {
        "directory": str(tree),
        "command": shlex.join([*compile_argv, str(source)]),
        "file": str(source),
    }


def gst_entries(gst_build: Path, repo_root: Path) -> list[dict[str, str]]:
    """The plugin build's entries for files under GST_SOURCE_DIR, file made absolute."""
    entries = json.loads((gst_build / "compile_commands.json").read_text(encoding="utf-8"))
    source_dir = (repo_root / GST_SOURCE_DIR).resolve()
    kept: list[dict[str, str]] = []
    for entry in entries:
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        path = path.resolve()
        if path.parent == source_dir:
            kept.append({**entry, "file": str(path)})
    return kept


def collect(build_dir: Path, repo_root: Path) -> tuple[list[dict[str, str]], list[str]]:
    """Return (entries, errors) for both integrations."""
    errors: list[str] = []
    entries: list[dict[str, str]] = []
    gst_build = build_dir / "gst"
    if (gst_build / "compile_commands.json").is_file():
        entries += gst_entries(gst_build, repo_root)
        measured = {Path(e["file"]).name for e in entries}
        expected = {p.name for p in (repo_root / GST_SOURCE_DIR).glob("*.c")}
        if not expected:
            errors.append(f"no source under {GST_SOURCE_DIR}")
        if expected - measured:
            errors.append(f"plugin sources not compiled: {sorted(expected - measured)}")
    else:
        errors.append(f"missing {gst_build / 'compile_commands.json'}")
    log = build_dir / "ffmpeg-objs.log"
    tree = build_dir / "ffmpeg"
    if log.is_file() and tree.is_dir():
        lines = ffmpeg_compile_lines(log.read_text(encoding="utf-8", errors="replace"))
        missing = sorted(set(FFMPEG_UNITS) - set(lines))
        if missing:
            errors.append(f"no FFmpeg compile line for {missing}")
        entries += [ffmpeg_entry(p, argv, tree, repo_root) for p, argv in sorted(lines.items())]
    else:
        errors.append(f"missing {log} or {tree}")
    return entries, errors


def merge(existing: list[dict[str, str]], added: list[dict[str, str]]) -> list[dict[str, str]]:
    """*existing* without the entries *added* replaces, then *added*."""
    replaced = {entry["file"] for entry in added}
    return [entry for entry in existing if entry.get("file") not in replaced] + added


def main(argv: list[str]) -> int:
    if len(argv) != EXPECTED_ARGC:
        print(f"usage: {argv[0]} <build-dir>", file=sys.stderr)
        return 2
    build_dir = Path(argv[1]).resolve()
    added, errors = collect(build_dir, REPO_ROOT)
    if errors:
        print(f"error: {'; '.join(errors)}", file=sys.stderr)
        return 1
    compdb = build_dir / "compile_commands.json"
    existing = json.loads(compdb.read_text(encoding="utf-8"))
    compdb.write_text(json.dumps(merge(existing, added), indent=2) + "\n", encoding="utf-8")
    print(
        f"gen-integrations-compile-commands: {len(added)} extra entries in {compdb}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
