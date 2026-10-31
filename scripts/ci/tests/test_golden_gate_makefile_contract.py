# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests for Netflix golden gate makefile target and build directory isolation."""

import os
import re
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

ROOT = Path(__file__).resolve().parent.parent.parent.parent
MAKEFILE = ROOT / "Makefile"
PREFLIGHT = ROOT / "scripts" / "ci" / "golden-arm64-preflight.sh"
SETUP_GOLDEN = ROOT / "scripts" / "ci" / "setup-golden-build.sh"


class GoldenGateMakefileContractTest(unittest.TestCase):
    """Pin Makefile target and isolation variables for test-netflix-golden."""

    makefile_text: ClassVar[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile_text = MAKEFILE.read_text(encoding="utf-8")

    def test_golden_build_dir_variable_defined(self) -> None:
        """Makefile must define GOLDEN_BUILD_DIR with default under core/ (or LIBVMAF_DIR)."""
        pattern = r"^GOLDEN_BUILD_DIR\s*(\?=|:=|=)\s*(.+)$"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Makefile must define GOLDEN_BUILD_DIR")
        assert match is not None
        val = match.group(2).strip()
        self.assertTrue(
            "build-golden" in val,
            f"GOLDEN_BUILD_DIR must point to a dedicated build-golden dir, got: {val}",
        )

    def test_build_golden_target_defined(self) -> None:
        """Makefile must define a build-golden target."""
        pattern = r"^build-golden\s*:"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Makefile must define a build-golden target")

    def test_test_netflix_golden_depends_on_build_golden_not_build(self) -> None:
        """test-netflix-golden must depend on build-golden, never on generic developer 'build'."""
        pattern = r"^test-netflix-golden\s*:\s*(.+)$"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Makefile must define test-netflix-golden target")
        assert match is not None
        prereqs = match.group(1).split()
        self.assertIn(
            "build-golden",
            prereqs,
            f"test-netflix-golden must depend on build-golden, got: {prereqs}",
        )
        self.assertNotIn(
            "build",
            prereqs,
            f"test-netflix-golden must NOT depend on generic developer 'build', got: {prereqs}",
        )

    def test_test_netflix_golden_exports_vmaf_build_dir(self) -> None:
        """test-netflix-golden recipe must export VMAF_BUILD_DIR to point pytest at isolated build."""
        # Locate recipe for test-netflix-golden
        pattern = r"^test-netflix-golden\s*:.*?\n((?:\t.*\n)+)"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Could not extract test-netflix-golden recipe")
        assert match is not None
        recipe = match.group(1)
        self.assertIn(
            "VMAF_BUILD_DIR=",
            recipe,
            "test-netflix-golden recipe must pass VMAF_BUILD_DIR to isolate python runner from core/build",
        )
        self.assertIn(
            "GOLDEN_BUILD_DIR",
            recipe,
            "test-netflix-golden recipe must reference GOLDEN_BUILD_DIR",
        )

    def test_clean_target_removes_golden_build_dir(self) -> None:
        """clean target in Makefile must clean GOLDEN_BUILD_DIR."""
        pattern = r"^clean\s*:.*?\n((?:\t.*\n)+)"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Could not extract clean recipe")
        assert match is not None
        recipe = match.group(1)
        self.assertTrue(
            "GOLDEN_BUILD_DIR" in recipe or "build-golden" in recipe,
            f"clean target recipe must clean GOLDEN_BUILD_DIR, got: {recipe}",
        )

    def test_test_netflix_golden_checks_pytest_presence(self) -> None:
        """test-netflix-golden recipe must check pytest presence before execution."""
        pattern = r"^test-netflix-golden\s*:.*?\n((?:\t.*\n)+)"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Could not extract test-netflix-golden recipe")
        assert match is not None
        recipe = match.group(1)
        self.assertIn(
            "pytest --version",
            recipe,
            "test-netflix-golden recipe must probe pytest presence before invoking test suite",
        )
        self.assertIn(
            "docs/development/languages.md",
            recipe,
            "test-netflix-golden recipe must cite documented install instructions on missing pytest",
        )


def _recipe(makefile_text: str, target: str) -> str:
    match = re.search(rf"^{re.escape(target)}\s*:.*?\n((?:\t.*\n)+)", makefile_text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"Could not extract the {target} recipe")
    return match.group(1)


class GoldenGateArm64MakefileContractTest(unittest.TestCase):
    """Pin the aarch64 golden gate target (ADR-1461): isolated, preflighted, same tests."""

    makefile_text: ClassVar[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile_text = MAKEFILE.read_text(encoding="utf-8")

    def test_arm64_variables_are_overridable_and_isolated(self) -> None:
        for name in ("GOLDEN_ARM64_CC", "GOLDEN_ARM64_CROSS_FILE", "GOLDEN_ARM64_BUILD_DIR"):
            with self.subTest(name=name):
                self.assertRegex(self.makefile_text, rf"(?m)^{name}\s*\?=")
        build_dir = re.search(r"(?m)^GOLDEN_ARM64_BUILD_DIR\s*\?=\s*(.+)$", self.makefile_text)
        assert build_dir is not None
        # One directory per compiler, and never the native gate's.
        self.assertIn("build-golden-arm64-$(GOLDEN_ARM64_CC)", build_dir.group(1))
        self.assertRegex(self.makefile_text, r"(?m)^QEMU_LD_PREFIX\s*\?=\s*\$\(AARCH64_SYSROOT\)")

    def test_build_runs_the_preflight_before_it_configures(self) -> None:
        recipe = _recipe(self.makefile_text, "build-golden-arm64")
        self.assertIn("scripts/ci/golden-arm64-preflight.sh", recipe)
        self.assertIn("scripts/ci/setup-golden-build.sh", recipe)
        self.assertLess(
            recipe.index("golden-arm64-preflight.sh"), recipe.index("setup-golden-build.sh")
        )
        self.assertIn('GOLDEN_CROSS_FILE="$(GOLDEN_ARM64_CROSS_FILE)"', recipe)
        self.assertIn("$(GOLDEN_ARM64_BUILD_DIR)", recipe)

    def test_gate_depends_on_its_own_build_only(self) -> None:
        match = re.search(r"(?m)^test-netflix-golden-arm64\s*:\s*(.+)$", self.makefile_text)
        self.assertIsNotNone(match, "Makefile must define test-netflix-golden-arm64")
        assert match is not None
        self.assertEqual(match.group(1).split(), ["build-golden-arm64"])

    def test_gate_runs_the_native_gate_tests_against_the_cross_build(self) -> None:
        recipe = _recipe(self.makefile_text, "test-netflix-golden-arm64")
        native = _recipe(self.makefile_text, "test-netflix-golden")
        for needle in (
            'QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)"',
            'VMAF_BUILD_DIR="$(CURDIR)/$(GOLDEN_ARM64_BUILD_DIR)"',
            "VMAF_FORCE_BACKEND=cpu",
            "pytest --version",
            "docs/development/languages.md",
        ):
            with self.subTest(needle=needle):
                self.assertIn(needle, recipe)
        # One list of tests for both gates: neither recipe spells a file.
        for text in (recipe, native):
            self.assertIn("$(GOLDEN_PYTEST_ARGS)", text)
            self.assertNotIn("python/test/", text)
        args = re.search(r"(?m)^GOLDEN_PYTEST_ARGS\s*:=((?:.*\\\n)*.*)$", self.makefile_text)
        assert args is not None
        self.assertEqual(len(re.findall(r"python/test/\w+\.py", args.group(1))), 7)
        self.assertNotIn("GOLDEN_BUILD_DIR)", recipe.replace("GOLDEN_ARM64_BUILD_DIR)", ""))

    def test_clean_removes_both_arm64_build_dirs(self) -> None:
        recipe = _recipe(self.makefile_text, "clean")
        for compiler in ("gcc", "clang"):
            self.assertIn(f"build-golden-arm64-{compiler}", recipe)

    def test_default_cross_file_exists_per_compiler(self) -> None:
        self.assertTrue((ROOT / "build-aux" / "aarch64-linux-gnu.ini").is_file())
        clang = (ROOT / "build-aux" / "aarch64-linux-gnu-clang.ini").read_text(encoding="utf-8")
        self.assertIn("--target=aarch64-linux-gnu", clang)
        self.assertIn("cpu_family = 'aarch64'", clang)


class GoldenArm64PreflightTest(unittest.TestCase):
    """The preflight names each missing piece and builds nothing without all of them."""

    def _host(
        self,
        root: Path,
        *,
        tools: tuple[str, ...] = (),
        loader: bool = True,
        binfmt: str | None = None,
    ) -> dict[str, str]:
        bin_dir = root / "bin"
        bin_dir.mkdir()
        # The two utilities the script calls, and nothing else of the host: a
        # real cross compiler in /usr/bin must not satisfy a fixture without one.
        for utility in ("head", "grep"):
            found = shutil.which(utility)
            assert found is not None
            (bin_dir / utility).symlink_to(found)
        for tool in tools:
            path = bin_dir / tool
            path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        sysroot = root / "sysroot"
        (sysroot / "lib").mkdir(parents=True)
        if loader:
            (sysroot / "lib" / "ld-linux-aarch64.so.1").write_text("", encoding="utf-8")
        cross = root / "cross.ini"
        cross.write_text("[binaries]\n", encoding="utf-8")
        entry = root / "binfmt-entry"
        if binfmt is not None:
            entry.write_text(binfmt, encoding="utf-8")
        return {
            "path": str(bin_dir),
            "sysroot": str(sysroot),
            "cross": str(cross),
            "entry": str(entry),
        }

    def _run(
        self, host: dict[str, str], compiler: str = "gcc", cross: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- fixed interpreter and script path.
            ["/bin/bash", str(PREFLIGHT), compiler, cross or host["cross"], host["sysroot"]],
            env={"PATH": host["path"], "GOLDEN_ARM64_BINFMT_ENTRY": host["entry"]},
            capture_output=True,
            check=False,
            encoding="utf-8",
        )

    GCC = ("aarch64-linux-gnu-gcc", "aarch64-linux-gnu-g++")
    CLANG = ("clang", "clang++", "ld.lld", "aarch64-linux-gnu-ar")
    ENABLED = "enabled\ninterpreter /usr/bin/qemu-aarch64-static\nflags: PF\n"

    def test_complete_host_passes_for_each_compiler(self) -> None:
        for compiler, tools in (("gcc", self.GCC), ("clang", self.CLANG)):
            with self.subTest(compiler=compiler), tempfile.TemporaryDirectory() as tmp:
                host = self._host(Path(tmp), tools=tools, loader=True, binfmt=self.ENABLED)
                result = self._run(host, compiler)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")

    def test_each_missing_piece_is_named(self) -> None:
        def host(root: Path, name: str) -> dict[str, str]:
            return self._host(
                root,
                tools=self.GCC[:1] if name == "cross compiler" else self.GCC,
                loader=name != "sysroot",
                binfmt={"no handler": None, "disabled handler": "disabled\n"}.get(
                    name, self.ENABLED
                ),
            )

        cases = (
            ("cross compiler", "aarch64-linux-gnu-g++"),
            ("sysroot", "ld-linux-aarch64.so.1"),
            ("no handler", "binfmt handler"),
            ("disabled handler", "is not enabled"),
        )
        for name, needle in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                result = self._run(host(Path(tmp), name))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(needle, result.stderr)
                self.assertIn("nothing was built", result.stderr)

    def test_all_missing_pieces_are_reported_together(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            host = self._host(Path(tmp), tools=(), loader=False, binfmt=None)
            result = self._run(host, cross=str(Path(tmp) / "absent.ini"))
            self.assertEqual(result.returncode, 1)
            for needle in ("cross compiler", "cross file", "sysroot", "binfmt handler"):
                self.assertIn(needle, result.stderr)

    def test_unknown_compiler_and_wrong_arity_are_usage_errors(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            host = self._host(Path(tmp), tools=self.GCC, loader=True, binfmt=self.ENABLED)
            self.assertEqual(self._run(host, "icx").returncode, 2)
        short = subprocess.run(  # noqa: S603 -- fixed interpreter and script path.
            ["/bin/bash", str(PREFLIGHT), "gcc"], capture_output=True, check=False, encoding="utf-8"
        )
        self.assertEqual(short.returncode, 2)
        self.assertIn("usage:", short.stderr)

    def test_setup_script_refuses_a_missing_cross_file_before_meson(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(  # noqa: S603 -- fixed interpreter and script path.
                ["/bin/bash", str(SETUP_GOLDEN), str(Path(tmp) / "build"), "core", "ninja"],
                env={"PATH": os.environ.get("PATH", ""), "GOLDEN_CROSS_FILE": "/nonexistent.ini"},
                capture_output=True,
                check=False,
                encoding="utf-8",
                cwd=ROOT,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("GOLDEN_CROSS_FILE", result.stderr)
            self.assertFalse((Path(tmp) / "build").exists())


if __name__ == "__main__":
    unittest.main()
