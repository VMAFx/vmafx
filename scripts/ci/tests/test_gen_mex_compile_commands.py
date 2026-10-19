# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for scripts/ci/gen-mex-compile-commands.py and the MATLAB lint stubs."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "gen-mex-compile-commands.py"


def _load() -> ModuleType:
    """Import the hyphenated script under test by path."""
    spec = importlib.util.spec_from_file_location("gen_mex_compile_commands", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot build an import spec for {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


gen: Any = _load()
REPO = Path(gen.REPO_ROOT)


def _run(build_dir: Path) -> tuple[int, str]:
    """Run main() on *build_dir*; return its exit status and stderr."""
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        status = gen.main(["gen-mex-compile-commands.py", str(build_dir)])
    return status, err.getvalue()


class GenerateEntries(unittest.TestCase):
    def test_every_mex_source_gets_one_entry_with_the_stub_include(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            keep = {"directory": tmp, "command": "cc x.c", "file": "/r/core/x.c"}
            (build / "compile_commands.json").write_text(json.dumps([keep]), encoding="utf-8")
            status, _ = _run(build)
            entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
        self.assertEqual(status, 0)
        mex = [e for e in entries if "/compat/python-vmaf/matlab/" in e["file"]]
        self.assertEqual(len(mex), len(gen.mex_sources(REPO)))
        self.assertGreaterEqual(len(mex), 10)
        self.assertIn(keep, entries, "an unrelated entry must survive")
        for entry in mex:
            argv = shlex.split(entry["command"])
            self.assertIn(f"-I{REPO / gen.STUB_DIR}", argv)

    def test_rerun_replaces_instead_of_duplicating(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "compile_commands.json").write_text("[]", encoding="utf-8")
            _run(build)
            _run(build)
            entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
        files = [e["file"] for e in entries]
        self.assertEqual(len(files), len(set(files)))

    def test_no_stub_header_is_an_error_not_a_clean_lane(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "compat/python-vmaf/matlab").mkdir(parents=True)
            (root / "compat/python-vmaf/matlab/a.c").write_text("int a;\n", encoding="utf-8")
            old, gen.REPO_ROOT = gen.REPO_ROOT, root
            try:
                (root / "compile_commands.json").write_text("[]", encoding="utf-8")
                status, err = _run(root)
            finally:
                gen.REPO_ROOT = old
        self.assertEqual(status, 1)
        self.assertIn("missing", err)

    def test_extra_sources_emitted_with_expected_flags(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "compile_commands.json").write_text("[]", encoding="utf-8")
            status, _ = _run(build)
            entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
        self.assertEqual(status, 0)
        by_file = {e["file"]: shlex.split(e["command"]) for e in entries}

        fault_path = str(REPO / gen.FAULT_INJECT_SOURCE)
        self.assertIn(fault_path, by_file)
        fault_cmd = by_file[fault_path]
        self.assertIn("-D_GNU_SOURCE", fault_cmd)
        self.assertIn(f"-I{build / 'include'}", fault_cmd)
        self.assertIn(f"-I{REPO / 'core/include'}", fault_cmd)

        harness_path = str(REPO / gen.HARNESS_SOURCE)
        self.assertIn(harness_path, by_file)
        harness_cmd = by_file[harness_path]
        self.assertIn("-O1", harness_cmd)
        self.assertIn("-DPARITY_COLLECTOR_ACCESSOR", harness_cmd)
        self.assertIn(f"-I{REPO / 'core/include'}", harness_cmd)
        self.assertIn(f"-I{build / 'include'}", harness_cmd)
        self.assertIn(f"-I{REPO / 'core/src'}", harness_cmd)
        self.assertIn(f"-I{build / 'src'}", harness_cmd)
        self.assertIn(f"-I{build}", harness_cmd)

        for src in (gen.ABI_CONSUMER_SOURCE, gen.ABI_PLANT_SOURCE):
            cmd = by_file[str(REPO / src)]
            self.assertIn(f"-I{REPO / 'core/include'}", cmd)
            self.assertNotIn(f"-I{REPO / 'core/src'}", cmd)
        self.assertIn("-D_GNU_SOURCE", by_file[str(REPO / gen.ABI_PLANT_SOURCE)])

    def test_missing_standalone_source_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / gen.STUB_DIR).mkdir(parents=True)
            for h in gen.STUB_HEADERS:
                (root / gen.STUB_DIR / h).write_text("/* stub */\n", encoding="utf-8")
            (root / "compat/python-vmaf/matlab").mkdir(parents=True)
            (root / "compat/python-vmaf/matlab/a.c").write_text("int a;\n", encoding="utf-8")
            old, gen.REPO_ROOT = gen.REPO_ROOT, root
            try:
                (root / "compile_commands.json").write_text("[]", encoding="utf-8")
                status, err = _run(root)
            finally:
                gen.REPO_ROOT = old
        self.assertEqual(status, 1)
        self.assertIn("missing standalone sources", err)

    def test_merge_boundary_empty_existing(self) -> None:
        added = [{"file": "/a.c", "command": "cc", "directory": "/b"}]
        self.assertEqual(gen.merge([], added), added)


@unittest.skipUnless(shutil.which("cc"), "no C compiler")
class StubsAreSufficient(unittest.TestCase):
    def test_every_mex_source_parses_against_the_stubs(self) -> None:
        for src in gen.mex_sources(REPO):
            entry = gen.entry_for(src, REPO, REPO)
            argv = [*shlex.split(entry["command"])[:-2], "-fsyntax-only"]
            done = subprocess.run(argv, capture_output=True, text=True, check=False)  # noqa: S603
            self.assertEqual(done.returncode, 0, f"{src}: {done.stderr[-400:]}")

    def test_standalone_sources_parse(self) -> None:
        for src in gen.STANDALONE_SOURCES:
            entry = gen.entry_for(src, REPO, REPO)
            argv = [*shlex.split(entry["command"])[:-2], "-fsyntax-only"]
            done = subprocess.run(argv, capture_output=True, text=True, check=False)  # noqa: S603
            self.assertEqual(done.returncode, 0, f"{src}: {done.stderr[-400:]}")


if __name__ == "__main__":
    unittest.main()
