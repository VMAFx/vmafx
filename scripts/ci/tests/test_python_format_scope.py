# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""black and ruff read every Python file; the exceptions are declared and unexpired (HISS-15).

The pre-commit hooks select files by type (python, pyi), so the only way a file leaves their
scope is the hook `exclude`. That regex, the declared list
(.config/lint-exceptions.d/{black,ruff}.toml) and pyproject.toml's extend-exclude must name the
same files, every listed file must still be tracked and must still fail its tool (a stale entry
hides nothing), and no entry may be past its expiry. The date can be overridden with
LINT_EXCEPTIONS_TODAY for the expiry case.
"""

from __future__ import annotations

import datetime as dt
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any, cast

import tomllib
import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs in the hook env, as in scripts/ci/check-helm-selector-isolation.py

ROOT = Path(__file__).resolve().parents[3]
LIST_DIR = ROOT / ".config" / "lint-exceptions.d"
HOOKS = {"black": "black", "ruff": "ruff-check"}
MAX_DAYS = 400


def declared(rule: str) -> list[dict[str, Any]]:
    data = tomllib.loads((LIST_DIR / f"{rule}.toml").read_text(encoding="utf-8"))
    return cast("list[dict[str, Any]]", data.get("exception", []))


def hook(rule: str) -> dict[str, Any]:
    config = yaml.safe_load((ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8"))
    found = [h for r in config["repos"] for h in r["hooks"] if h["id"] == HOOKS[rule]]
    if len(found) != 1:
        raise AssertionError(f"expected one {HOOKS[rule]} hook, found {len(found)}")
    return cast("dict[str, Any]", found[0])


def run_tool(exe: str, *args: str, cwd: Path | None = None) -> int:
    """Exit status of a resolved tool with a fixed argv."""
    return subprocess.run(  # noqa: S603 -- resolved executable, fixed argv
        [exe, *args], cwd=cwd, capture_output=True, timeout=120, check=False
    ).returncode


def tracked_python() -> list[str]:
    git = shutil.which("git")
    if git is None:
        raise AssertionError("git is not on PATH")
    out = subprocess.run(  # noqa: S603 -- resolved git, fixed argv
        [git, "-C", str(ROOT), "ls-files", "*.py", "*.pyi"],
        capture_output=True,
        text=True,
        check=True,
        timeout=60,
    ).stdout
    return out.split()


def today() -> dt.date:
    override = os.environ.get("LINT_EXCEPTIONS_TODAY")
    return dt.date.fromisoformat(override) if override else dt.date.today()


class PythonFormatScopeTests(unittest.TestCase):
    def test_hooks_have_no_files_filter(self) -> None:
        # A `files:` regex is how the hooks once read python|ai|scripts|tools only.
        for rule in HOOKS:
            self.assertNotIn("files", hook(rule), f"{rule} hook is scoped by a files: regex again")

    def test_hook_exclude_is_exactly_the_declared_files(self) -> None:
        tracked = tracked_python()
        for rule in HOOKS:
            exclude = re.compile(hook(rule)["exclude"])
            excluded = sorted(p for p in tracked if exclude.search(p))
            self.assertEqual(excluded, sorted(e["path"] for e in declared(rule)), rule)

    def test_every_entry_is_complete_tracked_and_unexpired(self) -> None:
        tracked = set(tracked_python())
        now = today()
        for rule in HOOKS:
            for entry in declared(rule):
                with self.subTest(rule=rule, path=entry["path"]):
                    self.assertTrue(entry.get("reason"), "no reason")
                    self.assertIsInstance(entry.get("expires"), dt.date)
                    self.assertIn(entry["path"], tracked)
                    self.assertGreaterEqual(entry["expires"], now, f"expired on {entry['expires']}")
                    self.assertLessEqual((entry["expires"] - now).days, MAX_DAYS)

    def test_pyproject_excludes_the_same_files(self) -> None:
        config = tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))
        ruff_exclude = set(config["tool"]["ruff"]["extend-exclude"])
        black_exclude = re.compile(config["tool"]["black"]["extend-exclude"], re.VERBOSE)
        for entry in declared("ruff"):
            self.assertIn(entry["path"], ruff_exclude)
        for entry in declared("black"):
            self.assertTrue(black_exclude.search("/" + entry["path"]), entry["path"])
        listed = {e["path"] for e in declared("ruff")}
        self.assertEqual({p for p in ruff_exclude if p.endswith(".py")}, listed)

    def test_a_listed_file_still_fails_its_tool(self) -> None:
        # No --force-exclude: the file is named explicitly, so the tool reads it despite
        # extend-exclude and must report a finding, or the entry is stale.
        config = str(ROOT / "pyproject.toml")
        for rule, args in (
            ("ruff", ["check", "--no-cache", "--quiet", "--config", config]),
            ("black", ["--check", "-q"]),
        ):
            exe = shutil.which(rule)
            if exe is None:
                self.skipTest(f"{rule} is not on PATH; the pre-commit hook runs this test with it")
            for entry in declared(rule):
                with self.subTest(rule=rule, path=entry["path"]):
                    status = run_tool(exe, *args, entry["path"], cwd=ROOT)
                    self.assertNotEqual(status, 0, "no finding: the entry is stale")

    def test_an_unlisted_file_is_read(self) -> None:
        exclude_black = re.compile(hook("black")["exclude"])
        exclude_ruff = re.compile(hook("ruff")["exclude"])
        for path in (
            "core/test/test_x.py",
            "compat/python-vmaf/resource/param/vmaf_v1.py",
            "x.pyi",
        ):
            self.assertIsNone(exclude_black.search(path), path)
            self.assertIsNone(exclude_ruff.search(path), path)

    def test_a_planted_defect_outside_the_old_scope_is_refused(self) -> None:
        ruff, black = shutil.which("ruff"), shutil.which("black")
        if ruff is None or black is None:
            self.skipTest(
                "ruff and black are not on PATH; the pre-commit hook runs this test with them"
            )
        with tempfile.TemporaryDirectory() as tmp:
            planted = Path(tmp) / "core" / "test" / "planted.py"
            planted.parent.mkdir(parents=True)
            planted.write_text("import os,sys\nx = [1,2 ,3]\n", encoding="utf-8")
            config = str(ROOT / "pyproject.toml")
            self.assertNotEqual(
                run_tool(ruff, "check", "--no-cache", "-q", "--config", config, str(planted)), 0
            )
            self.assertNotEqual(run_tool(black, "--check", "-q", str(planted)), 0)


if __name__ == "__main__":
    unittest.main()
