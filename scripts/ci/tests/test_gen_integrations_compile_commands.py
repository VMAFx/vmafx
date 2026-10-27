# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for scripts/ci/gen-integrations-compile-commands.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import shlex
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "gen-integrations-compile-commands.py"


def _load() -> ModuleType:
    """Import the hyphenated script under test by path."""
    spec = importlib.util.spec_from_file_location("gen_integrations_compile_commands", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot build an import spec for {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


gen: Any = _load()
REPO = Path(gen.REPO_ROOT)
GST_SOURCES = sorted((REPO / gen.GST_SOURCE_DIR).glob("*.c"))


def _make_line(tree_path: str) -> str:
    return (
        f"gcc-15 -I. -I./ -DHAVE_AV_CONFIG_H -std=c17 -Werror=implicit-function-declaration "
        f"-MMD -MF {tree_path[:-2]}.d -MT {tree_path[:-2]}.o -c -o {tree_path[:-2]}.o {tree_path}"
    )


def _populate(build: Path, *, gst: list[Path], ffmpeg: list[str]) -> None:
    """Write a core database, a plugin database over *gst* and a log over *ffmpeg*."""
    (build / "compile_commands.json").write_text(
        json.dumps([{"directory": str(build), "command": "cc a.c", "file": "/r/core/a.c"}]),
        encoding="utf-8",
    )
    (build / "gst").mkdir()
    gst_db = [
        {
            "directory": str(build / "gst"),
            "command": f"cc -c {src}",
            "file": str(src),
        }
        for src in gst
    ]
    gst_db.append({"directory": str(build / "gst"), "command": "cc -c x.c", "file": "x.c"})
    (build / "gst" / "compile_commands.json").write_text(json.dumps(gst_db), encoding="utf-8")
    (build / "ffmpeg").mkdir()
    log = ["make: Entering directory", "CC\tlibavutil/foo.o"] + [_make_line(p) for p in ffmpeg]
    (build / "ffmpeg-objs.log").write_text("\n".join(log) + "\n", encoding="utf-8")


def _run(build_dir: Path) -> tuple[int, str]:
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        status = gen.main(["gen-integrations-compile-commands.py", str(build_dir)])
    return status, err.getvalue()


class GenerateEntries(unittest.TestCase):
    def test_every_source_gets_one_entry_and_core_entries_survive(self) -> None:
        self.assertGreaterEqual(len(GST_SOURCES), 1)
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            _populate(build, gst=GST_SOURCES, ffmpeg=list(gen.FFMPEG_UNITS))
            status, err = _run(build)
            entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
        self.assertEqual(status, 0, err)
        files = [e["file"] for e in entries]
        self.assertIn("/r/core/a.c", files)
        for src in GST_SOURCES:
            self.assertEqual(files.count(str(src.resolve())), 1)
        for name in gen.FFMPEG_UNITS.values():
            self.assertEqual(files.count(str(REPO / gen.FFMPEG_SOURCE_DIR / name)), 1)
        self.assertFalse(any(f.endswith("/x.c") for f in files), "out-of-tree gst entry kept")

    def test_ffmpeg_entry_points_at_repo_copy_without_dependency_flags(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            argv = shlex.split(_make_line("libavfilter/vf_vmafx.c"))
            entry = gen.ffmpeg_entry("libavfilter/vf_vmafx.c", argv, tree, REPO)
        args = shlex.split(entry["command"])
        self.assertEqual(entry["directory"], str(tree))
        self.assertEqual(args[-1], str(REPO / "ffmpeg-patches/src/vf_vmafx.c"))
        self.assertEqual(args[1:3], [f"-I{tree / 'libavfilter'}", f"-I{tree}"])
        for flag in ("-MMD", "-MF", "-MT", "libavfilter/vf_vmafx.d"):
            self.assertNotIn(flag, args)
        self.assertIn("-DHAVE_AV_CONFIG_H", args)
        self.assertNotIn("libavfilter/vf_vmafx.c", args)

    def test_rerun_replaces_instead_of_duplicating(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            _populate(build, gst=GST_SOURCES, ffmpeg=list(gen.FFMPEG_UNITS))
            self.assertEqual(_run(build)[0], 0)
            first = (build / "compile_commands.json").read_text(encoding="utf-8")
            self.assertEqual(_run(build)[0], 0)
            second = (build / "compile_commands.json").read_text(encoding="utf-8")
        self.assertEqual(first, second)


class RefusesToMeasureNothing(unittest.TestCase):
    def test_missing_ffmpeg_line_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            _populate(build, gst=GST_SOURCES, ffmpeg=["libavfilter/vf_vmafx.c"])
            status, err = _run(build)
        self.assertEqual(status, 1)
        self.assertIn("fftools/ffmpeg_dec_hwaccel.c", err)

    def test_missing_plugin_source_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            _populate(build, gst=GST_SOURCES[1:], ffmpeg=list(gen.FFMPEG_UNITS))
            status, err = _run(build)
        self.assertEqual(status, 1)
        self.assertIn(GST_SOURCES[0].name, err)

    def test_missing_plugin_database_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            _populate(build, gst=GST_SOURCES, ffmpeg=list(gen.FFMPEG_UNITS))
            (build / "gst" / "compile_commands.json").unlink()
            status, err = _run(build)
        self.assertEqual(status, 1)
        self.assertIn("gst", err)

    def test_missing_log_fails_and_leaves_database_alone(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            _populate(build, gst=GST_SOURCES, ffmpeg=list(gen.FFMPEG_UNITS))
            (build / "ffmpeg-objs.log").unlink()
            before = (build / "compile_commands.json").read_text(encoding="utf-8")
            status, _ = _run(build)
            after = (build / "compile_commands.json").read_text(encoding="utf-8")
        self.assertEqual(status, 1)
        self.assertEqual(before, after)

    def test_usage(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(gen.main(["gen-integrations-compile-commands.py"]), 2)


if __name__ == "__main__":
    unittest.main()
