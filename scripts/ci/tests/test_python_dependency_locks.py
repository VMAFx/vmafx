#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression tests for hash-locked Python install policy."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, cast

ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "scripts/ci/check_python_dependency_locks.py"


def load_checker() -> Any:
    spec = importlib.util.spec_from_file_location("python_dependency_locks", CHECKER)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {CHECKER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class InstallCommandTests(unittest.TestCase):
    checker: Any

    @classmethod
    def setUpClass(cls) -> None:
        cls.checker = load_checker()

    def findings(self, text: str, path: str = ".github/workflows/example.yml") -> list[str]:
        return cast(list[str], self.checker.scan_install_commands(Path(path), text))

    def test_hash_locked_requirements_install_is_accepted(self) -> None:
        text = "run: python -m pip install --require-hashes -r requirements/locks/build.txt\n"
        self.assertEqual(self.findings(text), [])

    def test_unhashed_install_is_rejected(self) -> None:
        findings = self.findings("run: sudo pip3 install --quiet meson\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("--require-hashes", findings[0])

    def test_multiline_unhashed_install_is_rejected(self) -> None:
        text = """run: |
  python3 -m pip install --user --upgrade \\
    pre-commit ruff black
"""
        self.assertEqual(len(self.findings(text)), 1)

    def test_venv_pip_path_is_scanned(self) -> None:
        text = 'run: "${venv}/bin/pip" install pytest\n'
        self.assertEqual(len(self.findings(text)), 1)

    def test_local_editable_install_requires_no_deps_and_no_build_isolation(self) -> None:
        self.assertEqual(
            self.findings("run: pip install --no-deps --no-build-isolation -e ai\n"), []
        )
        self.assertEqual(len(self.findings("run: pip install --no-deps -e ai\n")), 1)
        self.assertEqual(len(self.findings("run: pip install --no-build-isolation -e ai\n")), 1)
        self.assertEqual(len(self.findings("run: pip install -e ai\n")), 1)

    def test_local_wheel_install_requires_no_deps(self) -> None:
        self.assertEqual(self.findings("RUN pip install --no-deps /wheels/*.whl\n"), [])
        targeted = "RUN pip install --no-deps --target /root /wheels/pkg.whl\n"
        self.assertEqual(self.findings(targeted), [])
        self.assertEqual(len(self.findings("RUN pip install /wheels/*.whl\n")), 1)

    def test_local_source_tree_requires_no_deps_and_no_build_isolation(self) -> None:
        self.assertEqual(
            self.findings(
                "RUN pip install --no-deps --no-build-isolation /opt/vmaf-mcp\n", "Dockerfile"
            ),
            [],
        )
        self.assertEqual(
            len(self.findings("RUN pip install --no-deps /opt/vmaf-mcp\n", "Dockerfile")), 1
        )
        self.assertEqual(
            len(
                self.findings("RUN pip install --no-build-isolation /opt/vmaf-mcp\n", "Dockerfile")
            ),
            1,
        )
        self.assertEqual(len(self.findings("RUN pip install /opt/vmaf-mcp\n", "Dockerfile")), 1)

    def test_documentation_echo_is_not_an_install(self) -> None:
        text = 'echo "Install with: pip install -r docs/requirements.txt" >&2\n'
        self.assertEqual(self.findings(text, "scripts/setup/example.sh"), [])

    def test_quoted_error_message_is_not_an_install(self) -> None:
        text = 'fail "torch unavailable" "pip install -r requirements.txt"\n'
        self.assertEqual(self.findings(text, "scripts/setup/example.sh"), [])

    def test_remote_wheel_is_not_treated_as_a_local_pin(self) -> None:
        text = "RUN pip install --no-deps https://example.invalid/pkg.whl\n"
        self.assertEqual(len(self.findings(text, "Dockerfile")), 1)

    def test_makefile_pip_variable_expansion_is_scanned(self) -> None:
        text = "\t$(VENV_PIP) install pytest\n"
        self.assertEqual(len(self.findings(text, "Makefile")), 1)
        text_brace = "\t${VENV_PIP} install pytest\n"
        self.assertEqual(len(self.findings(text_brace, "Makefile")), 1)

    def test_makefile_hash_locked_install_is_accepted(self) -> None:
        text = "\t$(VENV_PIP) install --require-hashes -r requirements/locks/build.txt\n"
        self.assertEqual(self.findings(text, "Makefile"), [])

    def test_tracked_consumer_paths_includes_makefile(self) -> None:
        paths = self.checker.tracked_consumer_paths(ROOT)
        self.assertIn(Path("Makefile"), paths)


class LockValidationTests(unittest.TestCase):
    checker: Any

    @classmethod
    def setUpClass(cls) -> None:
        cls.checker = load_checker()

    def test_unpinned_requirement_is_rejected(self) -> None:
        manifest = {"uv_version": "0.12.18", "locks": []}
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            input_file = temp_root / "dummy.in"
            input_file.write_text("pytest\n", encoding="utf-8")
            entry = {"output": "dummy.txt", "inputs": ["dummy.in"], "compile_args": ["dummy.in"]}
            lock_path = temp_root / "dummy.txt"
            lock_path.write_text(
                "# VMAFx hash lock; regenerate with: make python-locks-write\n"
                "# vmafx-uv-version: 0.12.18\n"
                f"# vmafx-input-sha256: {self.checker.entry_digest(temp_root, entry)}\n"
                "pytest>=8.0.0 --hash=sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
                encoding="utf-8",
            )
            problems = self.checker.validate_lock(temp_root, manifest, entry)
            self.assertTrue(any("not exactly version-pinned" in p for p in problems))

    def test_missing_hash_is_rejected(self) -> None:
        manifest = {"uv_version": "0.12.18", "locks": []}
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            input_file = temp_root / "dummy.in"
            input_file.write_text("pytest\n", encoding="utf-8")
            entry = {"output": "dummy.txt", "inputs": ["dummy.in"], "compile_args": ["dummy.in"]}
            lock_path = temp_root / "dummy.txt"
            lock_path.write_text(
                "# VMAFx hash lock; regenerate with: make python-locks-write\n"
                "# vmafx-uv-version: 0.12.18\n"
                f"# vmafx-input-sha256: {self.checker.entry_digest(temp_root, entry)}\n"
                "pytest==8.0.0\n",
                encoding="utf-8",
            )
            problems = self.checker.validate_lock(temp_root, manifest, entry)
            self.assertTrue(any("has no sha256 artifact hash" in p for p in problems))

    def test_generator_version_mismatch_is_rejected(self) -> None:
        manifest = {"uv_version": "0.12.18", "locks": []}
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            input_file = temp_root / "dummy.in"
            input_file.write_text("pytest\n", encoding="utf-8")
            entry = {"output": "dummy.txt", "inputs": ["dummy.in"], "compile_args": ["dummy.in"]}
            lock_path = temp_root / "dummy.txt"
            lock_path.write_text(
                "# VMAFx hash lock; regenerate with: make python-locks-write\n"
                "# vmafx-uv-version: 0.11.0\n"
                f"# vmafx-input-sha256: {self.checker.entry_digest(temp_root, entry)}\n"
                "pytest==8.0.0 --hash=sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
                encoding="utf-8",
            )
            problems = self.checker.validate_lock(temp_root, manifest, entry)
            self.assertTrue(any("generator version does not match manifest" in p for p in problems))

    def test_remote_output_is_rejected(self) -> None:
        entry = {
            "output": "https://example.com/lock.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in"],
        }
        problems = self.checker.validate_manifest_entry(entry)
        self.assertTrue(any("cannot be remote, absolute, or traverse" in p for p in problems))

    def test_traversal_output_is_rejected(self) -> None:
        entry_parent = {
            "output": "../outside.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in"],
        }
        problems_parent = self.checker.validate_manifest_entry(entry_parent)
        self.assertTrue(
            any("cannot be remote, absolute, or traverse" in p for p in problems_parent)
        )

        entry_abs = {
            "output": "/var/outside.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in"],
        }
        problems_abs = self.checker.validate_manifest_entry(entry_abs)
        self.assertTrue(any("cannot be remote, absolute, or traverse" in p for p in problems_abs))

    def test_duplicate_inputs_is_rejected(self) -> None:
        entry = {
            "output": "dummy.txt",
            "inputs": ["dummy.in", "dummy.in"],
            "compile_args": ["dummy.in"],
        }
        problems = self.checker.validate_manifest_entry(entry)
        self.assertTrue(any("contains duplicate paths" in p for p in problems))

    def test_compile_args_output_override_is_rejected(self) -> None:
        entry_dash_o = {
            "output": "dummy.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in", "-o", "override.txt"],
        }
        problems_dash_o = self.checker.validate_manifest_entry(entry_dash_o)
        self.assertTrue(
            any("cannot specify -o or --output-file override" in p for p in problems_dash_o)
        )

        entry_eq = {
            "output": "dummy.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in", "--output-file=override.txt"],
        }
        problems_eq = self.checker.validate_manifest_entry(entry_eq)
        self.assertTrue(
            any("cannot specify -o or --output-file override" in p for p in problems_eq)
        )

    def test_unregistered_lock_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            manifest_dir = temp_root / "requirements" / "locks"
            manifest_dir.mkdir(parents=True)
            input_file = temp_root / "dummy.in"
            input_file.write_text("pytest\n", encoding="utf-8")
            entry = {
                "output": "dummy-lock.txt",
                "inputs": ["dummy.in"],
                "compile_args": ["dummy.in"],
            }
            lock_path = temp_root / "dummy-lock.txt"
            lock_path.write_text(
                "# VMAFx hash lock; regenerate with: make python-locks-write\n"
                "# vmafx-uv-version: 0.12.18\n"
                f"# vmafx-input-sha256: {self.checker.entry_digest(temp_root, entry)}\n"
                "pytest==8.0.0 --hash=sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
                encoding="utf-8",
            )
            manifest_file = manifest_dir / "manifest.json"
            manifest_file.write_text(
                json.dumps({"uv_version": "0.12.18", "locks": [entry]}),
                encoding="utf-8",
            )

            # Valid setup passes check
            self.assertEqual(self.checker.check(temp_root), 0)

            # Creating an unregistered lock file causes failure
            unregistered = temp_root / "unregistered-lock.txt"
            unregistered.write_text("# dummy lock\n", encoding="utf-8")
            self.assertEqual(self.checker.check(temp_root), 1)

    def test_manifest_input_traversal_rejected(self) -> None:
        entry = {
            "output": "dummy.txt",
            "inputs": ["../outside.in"],
            "compile_args": ["dummy.in"],
        }
        problems = self.checker.validate_manifest_entry(entry)
        self.assertTrue(any("cannot be remote, absolute, or traverse" in p for p in problems))

    def test_manifest_input_absolute_rejected(self) -> None:
        entry = {
            "output": "dummy.txt",
            "inputs": ["/etc/passwd"],
            "compile_args": ["dummy.in"],
        }
        problems = self.checker.validate_manifest_entry(entry)
        self.assertTrue(any("cannot be remote, absolute, or traverse" in p for p in problems))

    def test_manifest_input_url_rejected(self) -> None:
        entry = {
            "output": "dummy.txt",
            "inputs": ["https://evil.com/deps.in"],
            "compile_args": ["dummy.in"],
        }
        problems = self.checker.validate_manifest_entry(entry)
        self.assertTrue(any("cannot be remote, absolute, or traverse" in p for p in problems))

    def test_compile_args_insecure_flags_rejected(self) -> None:
        insecure_flags = [
            ["--index-url", "https://pypi.evil.org/simple"],
            ["--extra-index-url", "https://pypi.evil.org/simple"],
            ["--find-links", "insecure-wheels/"],
            ["--trusted-host", "evil.org"],
            ["--no-index"],
            ["https://evil.com/packages.in"],
            ["../outside.in"],
        ]
        for flags in insecure_flags:
            with self.subTest(flags=flags):
                entry = {
                    "output": "dummy.txt",
                    "inputs": ["dummy.in"],
                    "compile_args": ["dummy.in", *flags],
                }
                problems = self.checker.validate_manifest_entry(entry)
                self.assertTrue(
                    any(
                        "insecure index/find-links/host argument" in p
                        or "cannot reference remote URLs" in p
                        or "cannot traverse outside the repository" in p
                        for p in problems
                    ),
                    f"Expected rejection for {flags}, got: {problems}",
                )

    def test_scan_install_remote_requirement_rejected(self) -> None:
        text = "pip install --require-hashes -r https://evil.com/lock.txt\n"
        findings = self.checker.scan_install_commands(Path("ci.sh"), text)
        self.assertEqual(len(findings), 1)

    def test_lock_directives_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            entry = {
                "output": "dummy-lock.txt",
                "inputs": ["dummy.in"],
                "compile_args": ["dummy.in"],
            }
            (temp_root / "dummy.in").write_text("pytest\n", encoding="utf-8")
            digest = self.checker.entry_digest(temp_root, entry)
            lock_path = temp_root / "dummy-lock.txt"
            lock_path.write_text(
                "# VMAFx hash lock; regenerate with: make python-locks-write\n"
                "# vmafx-uv-version: 0.12.18\n"
                f"# vmafx-input-sha256: {digest}\n"
                "--index-url https://evil.com/simple\n"
                "pytest==8.0.0 --hash=sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
                encoding="utf-8",
            )
            manifest = {"uv_version": "0.12.18", "locks": [entry]}
            problems = self.checker.validate_lock(temp_root, manifest, entry)
            self.assertTrue(
                any("lock file cannot contain unpinned directive" in p for p in problems)
            )


class RepositoryContractTests(unittest.TestCase):
    def test_repository_contract_is_current(self) -> None:
        result = subprocess.run(  # noqa: S603
            [sys.executable, str(CHECKER), "--root", str(ROOT), "check"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
