#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hermetic contract tests for ``install-cuda-toolkit.sh``.

The production script runs unchanged against ``/etc/os-release`` and
``/usr/local``. These tests set its two explicit test seams to temporary paths
and place fake commands in a PATH containing no system executables. Therefore
they cannot invoke apt, curl the network, use sudo, or mutate ``/usr/local``.
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "ci" / "install-cuda-toolkit.sh"
BUILD_CONFIG = ROOT / "build-config.env"


def _config_value(name: str) -> str:
    pattern = re.compile(rf'^{re.escape(name)}="([^"]+)"(?:\s+#.*)?$')
    for line in BUILD_CONFIG.read_text(encoding="utf-8").splitlines():
        match = pattern.fullmatch(line)
        if match:
            return match.group(1)
    raise AssertionError(f"{name} not found in {BUILD_CONFIG}")


APT_PACKAGE = _config_value("CUDA_APT_PACKAGE")
SERIES = APT_PACKAGE.removeprefix("cuda-toolkit-")
DOTTED = SERIES.replace("-", ".")
MAJOR = SERIES.split("-", maxsplit=1)[0]


class FakeHost:
    """A filesystem and command boundary for one installer invocation."""

    def __init__(
        self,
        root: Path,
        *,
        uid: int = 0,
        has_sudo: bool = True,
        curl_exit: int = 0,
    ) -> None:
        self.root = root
        self.bin = root / "bin"
        self.prefix = root / "install-root"
        self.log = root / "calls.log"
        self.os_release = root / "os-release"
        self.bin.mkdir(parents=True)
        self.prefix.mkdir()
        self.log.write_text("", encoding="utf-8")
        self.os_release.write_text('ID=ubuntu\nVERSION_ID="26.04"\n', encoding="utf-8")

        self._command(
            "id",
            f'if [ "${{1:-}}" = "-u" ]; then echo "{uid}"; else echo testuser; fi',
        )
        self._command("apt-get", "exit 0")
        self._command("dpkg", "exit 0")
        self._command("curl", f"exit {curl_exit}")
        self._command("ln", "exit 0")
        self._command("mktemp", 'exec /usr/bin/mktemp "$@"')
        self._command("rm", 'exec /usr/bin/rm "$@"')
        if uid != 0 and has_sudo:
            self._command("sudo", 'exec "$@"')

        cuda_home = self.prefix / f"cuda-{DOTTED}"
        (cuda_home / "bin").mkdir(parents=True)
        (cuda_home / "lib64").mkdir()
        targets_lib = cuda_home / "targets" / "x86_64-linux" / "lib"
        targets_lib.mkdir(parents=True)
        self._write_executable(
            cuda_home / "bin" / "nvcc",
            (
                "#!/bin/bash\n"
                f"printf '%s\\n' 'nvcc $*' >> '{self.log}'\n"
                f"printf '%s\\n' 'nvcc (fake) {DOTTED}'\n"
            ),
        )
        (cuda_home / "lib64" / f"libcudart.so.{MAJOR}").write_text("", encoding="utf-8")
        (targets_lib / f"libcudart.so.{MAJOR}").write_text("", encoding="utf-8")

    @staticmethod
    def _write_executable(path: Path, content: str) -> None:
        path.write_text(content, encoding="utf-8")
        path.chmod(0o755)

    def _command(self, name: str, body: str) -> None:
        self._write_executable(
            self.bin / name,
            (f"#!/bin/bash\nprintf '%s %s\\n' '{name}' \"$*\" >> '{self.log}'\n{body}\n"),
        )

    def run(
        self,
        *args: str,
        config: Path = BUILD_CONFIG,
        env_updates: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env.update(
            {
                "PATH": str(self.bin),
                "TMPDIR": str(self.root),
                "VMAFX_CUDA_OS_RELEASE_FILE": str(self.os_release),
                "VMAFX_CUDA_PREFIX": str(self.prefix),
            }
        )
        if env_updates:
            env.update(env_updates)
        return subprocess.run(  # noqa: S603 -- fixed script; test-owned arguments
            ["/bin/bash", str(SCRIPT), *args, str(config)],
            capture_output=True,
            check=False,
            env=env,
            text=True,
            timeout=15,
        )

    def calls(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").splitlines()

    def apt_install_calls(self) -> str:
        return " ".join(line for line in self.calls() if line.startswith("apt-get install "))


class InstallCudaToolkitTest(unittest.TestCase):
    """Exercise root/non-root, package selection, and fail-closed paths."""

    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="vmafx-cuda-test-")
        self.tempdir = Path(self._temporary.name)

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def test_root_builder_succeeds_without_sudo(self) -> None:
        host = FakeHost(self.tempdir, uid=0, has_sudo=False)
        result = host.run("--mode=builder")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any(call.startswith("sudo ") for call in host.calls()))

    def test_nonroot_builder_runs_privileged_commands_through_sudo(self) -> None:
        host = FakeHost(self.tempdir, uid=1001, has_sudo=True)
        result = host.run("--mode=builder")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(any(call.startswith("sudo apt-get ") for call in host.calls()))
        self.assertTrue(any(call.startswith("sudo dpkg ") for call in host.calls()))

    def test_nonroot_without_sudo_fails_closed(self) -> None:
        host = FakeHost(self.tempdir, uid=1001, has_sudo=False)
        result = host.run("--mode=builder")
        self.assertEqual(result.returncode, 1)
        self.assertIn("non-root execution requires sudo", result.stderr)
        self.assertFalse(any(call.startswith("apt-get ") for call in host.calls()))

    def test_builder_installs_only_compiler_and_development_runtime(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--builder")
        self.assertEqual(result.returncode, 0, result.stderr)
        install = host.apt_install_calls()
        self.assertIn(f"cuda-nvcc-{SERIES}", install)
        self.assertIn(f"cuda-cudart-dev-{SERIES}", install)
        self.assertIsNone(re.search(rf"\bcuda-cudart-{re.escape(SERIES)}\b(?!-dev)", install))

    def test_runtime_installs_only_runtime_and_creates_both_links(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--runtime")
        self.assertEqual(result.returncode, 0, result.stderr)
        install = host.apt_install_calls()
        self.assertIn(f"cuda-cudart-{SERIES}", install)
        self.assertNotIn("cuda-nvcc", install)
        ln_calls = [call for call in host.calls() if call.startswith("ln ")]
        self.assertIn(
            "ln -sf "
            f"libcudart.so.{MAJOR} "
            f"{host.prefix}/cuda-{DOTTED}/targets/x86_64-linux/lib/libcudart.so",
            ln_calls,
        )
        self.assertIn(
            f"ln -sf {host.prefix}/cuda-{DOTTED} {host.prefix}/cuda",
            ln_calls,
        )

    def test_invalid_mode_exits_2_before_installing(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--mode=frobulate")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown mode", result.stderr)
        self.assertFalse(any(call.startswith("apt-get ") for call in host.calls()))

    def test_unknown_flag_exits_2_before_installing(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--no-such-option")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown option", result.stderr)
        self.assertFalse(any(call.startswith("apt-get ") for call in host.calls()))

    def test_missing_cuda_apt_package_fails_closed(self) -> None:
        config = self.tempdir / "missing-package.env"
        config.write_text('CUDA_VERSION="13.4.2"\n', encoding="utf-8")
        host = FakeHost(self.tempdir / "host")
        result = host.run("--builder", config=config)
        self.assertEqual(result.returncode, 2)
        self.assertIn("CUDA_APT_PACKAGE is unset", result.stderr)

    def test_malformed_cuda_apt_package_fails_closed(self) -> None:
        config = self.tempdir / "malformed-package.env"
        config.write_text(
            'CUDA_VERSION="13.4.2"\nCUDA_APT_PACKAGE="cuda-compiler-13-4"\n',
            encoding="utf-8",
        )
        host = FakeHost(self.tempdir / "host")
        result = host.run("--builder", config=config)
        self.assertEqual(result.returncode, 2)
        self.assertIn("not of the form cuda-toolkit-<major>-<minor>", result.stderr)

    def test_missing_os_release_fixture_fails_closed(self) -> None:
        host = FakeHost(self.tempdir)
        missing = self.tempdir / "does-not-exist"
        result = host.run(
            "--builder",
            env_updates={"VMAFX_CUDA_OS_RELEASE_FILE": str(missing)},
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("OS release file not found", result.stderr)

    def test_relative_install_prefix_is_rejected(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run(
            "--builder",
            env_updates={"VMAFX_CUDA_PREFIX": "relative/path"},
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("must be an absolute non-root path", result.stderr)

    def test_missing_nvidia_repository_fails_before_package_install(self) -> None:
        host = FakeHost(self.tempdir, curl_exit=22)
        result = host.run("--builder")
        self.assertEqual(result.returncode, 1)
        self.assertIn("publishes no CUDA repository for 'ubuntu2604'", result.stderr)
        self.assertFalse(host.apt_install_calls())


if __name__ == "__main__":
    unittest.main()
