#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for scripts/ci/check_python_floors.py."""

from __future__ import annotations

import io
import json
import os
import re
import stat
import sys
import tempfile
import unittest
import unittest.mock
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_python_floors  # noqa: E402


class TestParseFloor(unittest.TestCase):
    """Test parse_floor logic."""

    def test_range_spec(self) -> None:
        floor, err = check_python_floors.parse_floor(">=3.12,<3.15")
        self.assertEqual(floor, "3.12")
        self.assertIsNone(err)

    def test_bare_floor(self) -> None:
        floor, err = check_python_floors.parse_floor(">=3.11")
        self.assertEqual(floor, "3.11")
        self.assertIsNone(err)

    def test_exact_spec_refused(self) -> None:
        floor, err = check_python_floors.parse_floor("==3.12")
        self.assertIsNone(floor)
        self.assertIsNotNone(err)
        self.assertIn("does not declare a '>=' floor", err or "")

    def test_missing_spec_refused(self) -> None:
        floor, err = check_python_floors.parse_floor(None)
        self.assertIsNone(floor)
        self.assertIsNotNone(err)
        self.assertIn("missing requires-python", err or "")


class TestCheckPythonFloors(unittest.TestCase):
    """Test check_python_floors CLI behavior with mock and real uv."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="test-pyfloors-")
        self.tmp_path = Path(self.tmp.name)

        # Create fake uv scripts
        self.fake_uv_ok = self.tmp_path / "fake_uv_ok.sh"
        self.fake_uv_ok.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        self.fake_uv_ok.chmod(self.fake_uv_ok.stat().st_mode | stat.S_IXUSR)

        self.fake_uv_fail = self.tmp_path / "fake_uv_fail.sh"
        self.fake_uv_fail.write_text(
            '#!/bin/sh\necho "error: No solution found when resolving dependencies" >&2\nexit 1\n',
            encoding="utf-8",
        )
        self.fake_uv_fail.chmod(self.fake_uv_fail.stat().st_mode | stat.S_IXUSR)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _write_pyproject(self, content: str) -> Path:
        p = self.tmp_path / "pkg" / "pyproject.toml"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")
        return p

    def test_fake_uv_missing_exit_2(self) -> None:
        """Missing uv binary returns exit 2 (operational error)."""
        nonexistent = self.tmp_path / "no_such_uv"
        buf = io.StringIO()
        with redirect_stderr(buf):
            code = check_python_floors.main(
                ["--root", str(self.tmp_path), "--uv-binary", str(nonexistent)]
            )
        self.assertEqual(code, 2)
        self.assertIn("uv binary not found", buf.getvalue())

    def test_fake_uv_exiting_1_is_finding_exit_1(self) -> None:
        """When uv exits 1, resolution finding is emitted and exit code is 1."""
        self._write_pyproject("""[project]
name = "planted-pkg"
version = "0.0.1"
requires-python = ">=3.10"
dependencies = ["numpy>=2.5.3"]
""")
        out = io.StringIO()
        with redirect_stdout(out):
            code = check_python_floors.main(
                ["--root", str(self.tmp_path), "--uv-binary", str(self.fake_uv_fail)]
            )
        self.assertEqual(code, 1)
        output = out.getvalue()
        self.assertIn("floor 3.10 does not resolve", output)
        self.assertIn("error: No solution found when resolving dependencies", output)

    def test_boundary_floor_ok_with_fake_uv_exit_0(self) -> None:
        """Boundary test: valid floor 3.12 with uv exiting 0 exits 0."""
        self._write_pyproject("""[project]
name = "ok-pkg"
version = "0.0.1"
requires-python = ">=3.12"
dependencies = ["numpy>=2.5.3"]
""")
        code = check_python_floors.main(
            ["--root", str(self.tmp_path), "--uv-binary", str(self.fake_uv_ok)]
        )
        self.assertEqual(code, 0)

    def test_missing_requires_python_is_finding_exit_1(self) -> None:
        """Missing requires-python produces a finding and exits 1."""
        self._write_pyproject("""[project]
name = "no-req-python"
version = "0.0.1"
dependencies = []
""")
        out = io.StringIO()
        with redirect_stdout(out):
            code = check_python_floors.main(
                ["--root", str(self.tmp_path), "--uv-binary", str(self.fake_uv_ok)]
            )
        self.assertEqual(code, 1)
        self.assertIn("missing requires-python in [project]", out.getvalue())

    def test_exact_requires_python_is_finding_exit_1(self) -> None:
        """Exact requires-python ==3.12 produces a finding and exits 1."""
        self._write_pyproject("""[project]
name = "exact-python"
version = "0.0.1"
requires-python = "==3.12"
dependencies = []
""")
        out = io.StringIO()
        with redirect_stdout(out):
            code = check_python_floors.main(
                ["--root", str(self.tmp_path), "--uv-binary", str(self.fake_uv_ok)]
            )
        self.assertEqual(code, 1)
        self.assertIn("does not declare a '>=' floor", out.getvalue())

    @unittest.skipUnless(
        os.environ.get("VMAFX_NETWORK_TESTS") == "1",
        "real uv resolution requires VMAFX_NETWORK_TESTS=1",
    )
    def test_real_uv_planted_numpy_floor_310_fails(self) -> None:
        """Real uv fails with exit 1 on planted numpy>=2.5.3 package with floor 3.10."""
        self._write_pyproject("""[project]
name = "planted-numpy"
version = "0.0.1"
requires-python = ">=3.10"
dependencies = ["numpy>=2.5.3"]
""")
        out = io.StringIO()
        with redirect_stdout(out):
            code = check_python_floors.main(["--root", str(self.tmp_path)])
        self.assertEqual(code, 1)
        self.assertIn("floor 3.10 does not resolve", out.getvalue())

    def test_default_uv_is_the_lock_writer_lookup(self) -> None:
        """Without --uv-binary the floors check takes the lock writer's uv."""
        fake = str(self.fake_uv_ok)
        with unittest.mock.patch.object(
            check_python_floors, "find_default_uv", return_value=fake
        ) as lookup:
            code = check_python_floors.main(["--root", str(self.tmp_path)])
        lookup.assert_called_once_with(self.tmp_path.resolve())
        self.assertEqual(code, 0)

    def test_the_tree_passes(self) -> None:
        """The actual repository packages pass floor check."""
        code = check_python_floors.main(["--root", str(ROOT)])
        self.assertEqual(code, 0)


def tooling_uv_pin(text: str) -> str | None:
    """Return the uv version pinned in a tooling-tests lock input."""
    match = re.search(r"^uv==(\S+)$", text, re.MULTILINE)
    return match.group(1) if match else None


class TestToolingUvPin(unittest.TestCase):
    """The tooling suite installs the uv the lock manifest names."""

    def manifest_uv(self) -> str:
        manifest = ROOT / "requirements/locks/manifest.json"
        return str(json.loads(manifest.read_text(encoding="utf-8"))["uv_version"])

    def test_pin_equals_manifest(self) -> None:
        text = (ROOT / "requirements/locks/tooling-tests.in").read_text(encoding="utf-8")
        self.assertEqual(tooling_uv_pin(text), self.manifest_uv())

    def test_planted_other_version_is_refused(self) -> None:
        planted = "pyyaml==6.0.3\nuv==0.0.1\n"
        self.assertNotEqual(tooling_uv_pin(planted), self.manifest_uv())

    def test_missing_pin_is_refused(self) -> None:
        self.assertIsNone(tooling_uv_pin("pyyaml==6.0.3\nuvicorn==1.0\n"))


if __name__ == "__main__":
    unittest.main()
