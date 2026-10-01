# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary test coverage for check-copyright.sh (HISS-15)."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHECK_SCRIPT = ROOT / "scripts/ci/check-copyright.sh"
SPDX_TAG = "SPDX-License" + "-Identifier:"


class CheckCopyrightTests(unittest.TestCase):
    """Test suite verifying check-copyright.sh enforces Copyright and SPDX."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work_dir = Path(self.tmp.name)

    def run_check(self, *files: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- fixed bash executable and script argv
            ["/bin/bash", str(CHECK_SCRIPT), *(str(f) for f in files)],
            cwd=str(self.work_dir),
            capture_output=True,
            text=True,
            check=False,
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

    def test_generated_file_skipped(self) -> None:
        file = self.work_dir / "config.h.in"
        file.write_text("#mesondefine HAVE_CUDA\n", encoding="utf-8")
        res = self.run_check(file)
        self.assertEqual(res.returncode, 0)

    def test_pelorus_mirror_file_skipped(self) -> None:
        pelorus_dir = self.work_dir / "core/src/interop"
        pelorus_dir.mkdir(parents=True, exist_ok=True)
        file = pelorus_dir / "pelorus_test.c"
        file.write_text("/* vendored mirror code without spdx */\nint y = 1;\n", encoding="utf-8")
        res = self.run_check(file)
        self.assertEqual(res.returncode, 0)

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
