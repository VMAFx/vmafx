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
CUDA_VERSION = _config_value("CUDA_VERSION")
LOCK_RELEASE = _config_value("CUDA_APT_LOCK_RELEASE")
TOOLKIT_VERSION = _config_value("CUDA_APT_TOOLKIT_VERSION")
NVCC_VERSION = _config_value("CUDA_APT_NVCC_VERSION")
CUDART_VERSION = _config_value("CUDA_APT_CUDART_VERSION")
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
        installed_versions: dict[str, str] | None = None,
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
        versions = {
            APT_PACKAGE: TOOLKIT_VERSION,
            f"cuda-nvcc-{SERIES}": NVCC_VERSION,
            f"cuda-cudart-dev-{SERIES}": CUDART_VERSION,
            f"cuda-cudart-{SERIES}": CUDART_VERSION,
        }
        versions.update(installed_versions or {})

        self._command("apt-get", "exit 0")
        self._command("dpkg", "exit 0")
        query_cases = "\n".join(
            f'  {package}) printf "%s" "{version}" ;;' for package, version in versions.items()
        )
        self._command(
            "dpkg-query",
            'package="${@: -1}"\ncase "$package" in\n' f"{query_cases}\n" "  *) exit 1 ;;\nesac",
        )
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
        append_config: bool = True,
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
        command = ["/bin/bash", str(SCRIPT), *args]
        if append_config:
            command.append(str(config))
        return subprocess.run(  # noqa: S603 -- fixed script; test-owned arguments
            command,
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
        self.assertIn(f"cuda-nvcc-{SERIES}={NVCC_VERSION}", install)
        self.assertIn(f"cuda-cudart-dev-{SERIES}={CUDART_VERSION}", install)
        self.assertIn(f"cuda-cudart-{SERIES}={CUDART_VERSION}", install)
        self.assertNotIn(APT_PACKAGE, install)
        self.assertIn(
            f"ln -sfn {host.prefix}/cuda-{DOTTED} {host.prefix}/cuda",
            host.calls(),
        )

    def test_runtime_installs_only_runtime_and_creates_both_links(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--runtime")
        self.assertEqual(result.returncode, 0, result.stderr)
        install = host.apt_install_calls()
        self.assertIn(f"cuda-cudart-{SERIES}={CUDART_VERSION}", install)
        self.assertNotIn("cuda-nvcc", install)
        ln_calls = [call for call in host.calls() if call.startswith("ln ")]
        self.assertIn(
            "ln -sf "
            f"libcudart.so.{MAJOR} "
            f"{host.prefix}/cuda-{DOTTED}/targets/x86_64-linux/lib/libcudart.so",
            ln_calls,
        )
        self.assertIn(
            f"ln -sfn {host.prefix}/cuda-{DOTTED} {host.prefix}/cuda",
            ln_calls,
        )

    def test_runtime_replaces_a_stale_cuda_current_symlink(self) -> None:
        host = FakeHost(self.tempdir)
        (host.prefix / "cuda").symlink_to(host.prefix / "cuda-12.9")
        result = host.run("--runtime")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            f"ln -sfn {host.prefix}/cuda-{DOTTED} {host.prefix}/cuda",
            host.calls(),
        )

    def test_runtime_refuses_to_replace_a_real_cuda_directory(self) -> None:
        host = FakeHost(self.tempdir)
        (host.prefix / "cuda").mkdir()
        result = host.run("--runtime")
        self.assertEqual(result.returncode, 1)
        self.assertIn("exists but is not a symlink", result.stderr)
        self.assertFalse(any(call.startswith("ln -sfn ") for call in host.calls()))

    def test_full_mode_pins_toolkit_and_core_components(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--mode=full")
        self.assertEqual(result.returncode, 0, result.stderr)
        install = host.apt_install_calls()
        self.assertIn(f"{APT_PACKAGE}={TOOLKIT_VERSION}", install)
        self.assertIn(f"cuda-nvcc-{SERIES}={NVCC_VERSION}", install)
        self.assertIn(f"cuda-cudart-dev-{SERIES}={CUDART_VERSION}", install)
        self.assertIn(f"cuda-cudart-{SERIES}={CUDART_VERSION}", install)
        self.assertNotIn("libcuda1", install)

    def test_installed_version_mismatch_fails_closed(self) -> None:
        package = f"cuda-nvcc-{SERIES}"
        host = FakeHost(
            self.tempdir,
            installed_versions={package: "13.4.999-1"},
        )
        result = host.run("--builder")
        self.assertEqual(result.returncode, 1)
        self.assertIn(package, result.stderr)
        self.assertIn(NVCC_VERSION, result.stderr)
        self.assertIn("13.4.999-1", result.stderr)

    def test_invalid_mode_exits_2_before_installing(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--mode=frobulate")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown mode", result.stderr)
        self.assertFalse(any(call.startswith("apt-get ") for call in host.calls()))

    def test_missing_mode_value_exits_2_with_clear_error(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--mode", append_config=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--mode requires a value", result.stderr)
        self.assertNotIn("unbound variable", result.stderr)
        self.assertFalse(any(call.startswith("apt-get ") for call in host.calls()))

    def test_unknown_flag_exits_2_before_installing(self) -> None:
        host = FakeHost(self.tempdir)
        result = host.run("--no-such-option")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown option", result.stderr)
        self.assertFalse(any(call.startswith("apt-get ") for call in host.calls()))

    def test_missing_cuda_apt_package_fails_closed(self) -> None:
        config = self.tempdir / "missing-package.env"
        config.write_text(
            f'CUDA_VERSION="{CUDA_VERSION}"\n'
            f'CUDA_APT_LOCK_RELEASE="{LOCK_RELEASE}"\n'
            f'CUDA_APT_TOOLKIT_VERSION="{TOOLKIT_VERSION}"\n'
            f'CUDA_APT_NVCC_VERSION="{NVCC_VERSION}"\n'
            f'CUDA_APT_CUDART_VERSION="{CUDART_VERSION}"\n',
            encoding="utf-8",
        )
        host = FakeHost(self.tempdir / "host")
        result = host.run("--builder", config=config)
        self.assertEqual(result.returncode, 2)
        self.assertIn("CUDA_APT_PACKAGE is unset", result.stderr)

    def test_malformed_cuda_apt_package_fails_closed(self) -> None:
        config = self.tempdir / "malformed-package.env"
        config.write_text(
            f'CUDA_VERSION="{CUDA_VERSION}"\n'
            f'CUDA_APT_LOCK_RELEASE="{LOCK_RELEASE}"\n'
            'CUDA_APT_PACKAGE="cuda-compiler-13-4"\n'
            f'CUDA_APT_TOOLKIT_VERSION="{TOOLKIT_VERSION}"\n'
            f'CUDA_APT_NVCC_VERSION="{NVCC_VERSION}"\n'
            f'CUDA_APT_CUDART_VERSION="{CUDART_VERSION}"\n',
            encoding="utf-8",
        )
        host = FakeHost(self.tempdir / "host")
        result = host.run("--builder", config=config)
        self.assertEqual(result.returncode, 2)
        self.assertIn("not of the form cuda-toolkit-<major>-<minor>", result.stderr)

    def test_release_lock_mismatch_fails_before_repository_access(self) -> None:
        config = self.tempdir / "stale-lock.env"
        config.write_text(
            f'CUDA_VERSION="{CUDA_VERSION}"\n'
            f'CUDA_APT_LOCK_RELEASE="13.4.1"\n'
            f'CUDA_APT_PACKAGE="{APT_PACKAGE}"\n'
            f'CUDA_APT_TOOLKIT_VERSION="{TOOLKIT_VERSION}"\n'
            f'CUDA_APT_NVCC_VERSION="{NVCC_VERSION}"\n'
            f'CUDA_APT_CUDART_VERSION="{CUDART_VERSION}"\n',
            encoding="utf-8",
        )
        host = FakeHost(self.tempdir / "host")
        result = host.run("--builder", config=config)
        self.assertEqual(result.returncode, 2)
        self.assertIn("CUDA_APT_LOCK_RELEASE", result.stderr)
        self.assertFalse(any(call.startswith("curl ") for call in host.calls()))

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
