# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary test coverage for check-copyright.sh (HISS-15)."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import tomllib

ROOT = Path(__file__).resolve().parents[3]
CHECK_SCRIPT = ROOT / "scripts/ci/check-copyright.sh"
SPDX_TAG = "SPDX-License" + "-Identifier:"
# The suffixes check-copyright.sh reads for the SPDX rule.
SPDX_SUFFIXES = frozenset(
    {
        ".c", ".h", ".cpp", ".cxx", ".cc", ".hpp", ".hxx", ".cu", ".cuh", ".hip", ".mm", ".metal",
        ".go", ".py", ".pyx", ".rs", ".sh",
    }
)  # fmt: skip


def live_spdx_exception() -> str:
    """A tracked file the repository's SPDX exception list holds and that has no SPDX line.

    Read from the list rather than named here: a fixed name stops testing anything the day
    its entry is retired (the ten Pelorus mirror entries went with ADR-2817).
    """
    listed = tomllib.loads(
        (ROOT / ".config/lint-exceptions.d/spdx.toml").read_text(encoding="utf-8")
    ).get("exception", [])
    for entry in listed:
        rel = str(entry["path"])
        path = ROOT / rel
        head = "".join(path.read_text(encoding="utf-8").splitlines(keepends=True)[:40])
        if path.suffix in SPDX_SUFFIXES and SPDX_TAG not in head:
            return rel
    raise AssertionError("spdx.toml lists no checked file without an SPDX line to test against")


class CheckCopyrightTests(unittest.TestCase):
    """Test suite verifying check-copyright.sh enforces Copyright and SPDX."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work_dir = Path(self.tmp.name)

    def run_check(
        self, *files: Path | str, cwd: Path | None = None, today: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        if today:
            env["LINT_EXCEPTIONS_TODAY"] = today
        return subprocess.run(  # noqa: S603 -- fixed bash executable and script argv
            ["/bin/bash", str(CHECK_SCRIPT), *(str(f) for f in files)],
            cwd=str(cwd or self.work_dir),
            capture_output=True,
            text=True,
            check=False,
            env=env,
        )

    def test_valid_c_file_passes(self) -> None:
        file = self.work_dir / "sample.c"
        file.write_text(
            f"/*\n * Copyright 2026 Lusoris\n * {SPDX_TAG} EUPL-1.2\n */\nint x = 0;\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stderr, "")

    def test_valid_cpp_file_passes(self) -> None:
        file = self.work_dir / "sample.cpp"
        file.write_text(
            f"/*\n * Copyright 2016-2020 Netflix, Inc.\n * {SPDX_TAG} BSD-2-Clause-Patent\n */\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_valid_cuda_file_passes(self) -> None:
        file = self.work_dir / "sample.cu"
        file.write_text(
            f"// Copyright 2026 Lusoris\n// {SPDX_TAG} EUPL-1.2\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_valid_go_file_passes(self) -> None:
        file = self.work_dir / "sample.go"
        file.write_text(
            f"// {SPDX_TAG} EUPL-1.2\npackage main\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_valid_python_file_passes(self) -> None:
        file = self.work_dir / "sample.py"
        file.write_text(
            f"#!/usr/bin/env python3\n# {SPDX_TAG} EUPL-1.2\nimport sys\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_c_file_missing_copyright_fails(self) -> None:
        file = self.work_dir / "missing_copy.c"
        file.write_text(
            f"/*\n * {SPDX_TAG} EUPL-1.2\n */\nint x = 0;\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 1)
        self.assertIn("ADR-0105", res.stderr)
        self.assertIn("missing Copyright header", res.stderr)

    def test_c_file_missing_spdx_fails(self) -> None:
        file = self.work_dir / "missing_spdx.c"
        file.write_text(
            "/*\n * Copyright 2026 Lusoris\n */\nint x = 0;\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 1)
        self.assertIn("ADR-1250", res.stderr)
        self.assertIn(f"missing {SPDX_TAG[:-1]}", res.stderr)

    def test_python_file_missing_spdx_fails(self) -> None:
        file = self.work_dir / "missing_spdx.py"
        file.write_text(
            "import os\nprint('hello')\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 1)
        self.assertIn("ADR-1250", res.stderr)
        self.assertIn(f"missing {SPDX_TAG[:-1]}", res.stderr)

    def test_go_file_missing_spdx_fails(self) -> None:
        file = self.work_dir / "missing_spdx.go"
        file.write_text(
            "package main\nfunc main() {}\n",
            encoding="utf-8",
        )
        res = self.run_check(file)
        self.assertEqual(res.returncode, 1)
        self.assertIn("ADR-1250", res.stderr)
        self.assertIn(f"missing {SPDX_TAG[:-1]}", res.stderr)

    def test_header_in_every_adr_1250_language_passes(self) -> None:
        for name, comment in (
            ("k.hip", "//"),
            ("k.metal", "//"),
            ("k.mm", "//"),
            ("k.rs", "//"),
            ("k.sh", "#"),
            ("k.pyx", "#"),
        ):
            with self.subTest(name=name):
                file = self.work_dir / name
                file.write_text(
                    f"{comment} Copyright 2026 Lusoris\n{comment} {SPDX_TAG} EUPL-1.2\n",
                    encoding="utf-8",
                )
                res = self.run_check(file)
                self.assertEqual(res.returncode, 0, res.stderr)

    def test_missing_spdx_fails_in_every_adr_1250_language(self) -> None:
        # The hook once selected c, c++, cuda, go and python only: a headerless .hip, .metal,
        # .rs, .sh or .pyx passed unseen.
        for name in ("k.hip", "k.metal", "k.mm", "k.rs", "k.sh", "k.pyx"):
            with self.subTest(name=name):
                file = self.work_dir / name
                file.write_text("// Copyright 2026 Lusoris\nint x;\n", encoding="utf-8")
                res = self.run_check(file)
                self.assertEqual(res.returncode, 1)
                self.assertIn("ADR-1250", res.stderr)

    def test_hip_and_metal_missing_copyright_fail(self) -> None:
        for name in ("k.hip", "k.metal", "k.mm"):
            with self.subTest(name=name):
                file = self.work_dir / name
                file.write_text(f"// {SPDX_TAG} EUPL-1.2\n", encoding="utf-8")
                res = self.run_check(file)
                self.assertEqual(res.returncode, 1)
                self.assertIn("ADR-0105", res.stderr)

    def test_no_path_pattern_skips_a_headerless_file(self) -> None:
        # The script once skipped *generated*, *config.h.in, matlab and pelorus paths by name.
        for rel in (
            "core/src/interop/pelorus_test.c",
            "compat/python-vmaf/matlab/x/mex.c",
            "core/include/libvmaf/pelorus/x.h",
            "api/zz_generated_deepcopy.go",
        ):
            with self.subTest(rel=rel):
                file = self.work_dir / rel
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text("int y = 1;\n", encoding="utf-8")
                res = self.run_check(file)
                self.assertEqual(res.returncode, 1, res.stderr)

    def test_declared_exception_holds_until_it_expires(self) -> None:
        spdx_exception = live_spdx_exception()
        self.assertEqual(self.run_check(spdx_exception, cwd=ROOT).returncode, 0)
        late = self.run_check(spdx_exception, cwd=ROOT, today="2099-01-01")
        self.assertEqual(late.returncode, 1)
        self.assertIn("ADR-1250", late.stderr)

    def test_declared_exception_is_per_rule(self) -> None:
        # The MEX sources are excepted from the copyright rule only: their SPDX line is read.
        mex = "compat/python-vmaf/matlab/strred/matlabPyrTools/MEX/corrDn.c"
        self.assertEqual(self.run_check(mex, cwd=ROOT).returncode, 0)
        late = self.run_check(mex, cwd=ROOT, today="2099-01-01")
        self.assertEqual(late.returncode, 1)
        self.assertIn("ADR-0105", late.stderr)
        self.assertNotIn("ADR-1250", late.stderr)

    def test_nonexistent_file_skipped(self) -> None:
        nonexistent = self.work_dir / "does_not_exist.c"
        res = self.run_check(nonexistent)
        self.assertEqual(res.returncode, 0)

    def test_spdx_in_first_40_lines_passes(self) -> None:
        file = self.work_dir / "late_header.py"
        lines = ["# comment line\n"] * 38 + [
            f"# {SPDX_TAG} EUPL-1.2\n",
            "import os\n",
        ]
        file.write_text("".join(lines), encoding="utf-8")
        res = self.run_check(file)
        self.assertEqual(res.returncode, 0)

    def test_spdx_beyond_40_lines_fails(self) -> None:
        file = self.work_dir / "too_late_header.py"
        lines = ["# comment line\n"] * 41 + [
            f"# {SPDX_TAG} EUPL-1.2\n",
            "import os\n",
        ]
        file.write_text("".join(lines), encoding="utf-8")
        res = self.run_check(file)
        self.assertEqual(res.returncode, 1)
        self.assertIn("ADR-1250", res.stderr)


if __name__ == "__main__":
    unittest.main()
