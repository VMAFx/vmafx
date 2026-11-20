#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Both setup scripts install shfmt through one per-architecture pin (issue 3101)."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

INSTALL = ROOT / "scripts/setup/install-shfmt.sh"
UBUNTU = ROOT / "scripts/setup/ubuntu.sh"
FEDORA = ROOT / "scripts/setup/fedora.sh"
CONFIG = ROOT / "build-config.env"
BASH = Path(r"C:\Program Files\Git\bin\bash.exe")
if not BASH.is_file():
    BASH = Path(shutil.which("bash") or "bash")


def posix(path: Path) -> str:
    text = path.as_posix()
    if ":" not in text:
        return text
    drive, rest = text.split(":", 1)
    return f"/{drive.lower()}{rest}"


CASES = {
    "x86_64": ("amd64", "SHFMT_SHA256_LINUX_AMD64"),
    "aarch64": ("arm64", "SHFMT_SHA256_LINUX_ARM64"),
    "armv7l": ("arm", "SHFMT_SHA256_LINUX_ARM"),
    "i686": ("386", "SHFMT_SHA256_LINUX_386"),
}


def config_value(name: str) -> str:
    prefix = f'{name}="'
    for line in CONFIG.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and line.endswith('"'):
            return line[len(prefix) : -1]
    raise AssertionError(f"build-config.env declares no {name}")


def resolve(machine: str) -> TextCommandResult:
    return run_command(
        [str(BASH), posix(INSTALL), "--resolve", machine],
        allowed_executables=(str(BASH),),
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
        timeout_seconds=30,
    )


class ShfmtSetup(unittest.TestCase):
    def test_the_pin_is_in_build_config(self) -> None:
        self.assertEqual(config_value("SHFMT_VERSION"), "v3.13.1")

    def test_each_architecture_maps_to_its_own_digest(self) -> None:
        for machine, (asset, digest_key) in CASES.items():
            with self.subTest(machine=machine):
                result = resolve(machine)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), f"{asset} {config_value(digest_key)}")

    def test_an_unknown_architecture_is_refused(self) -> None:
        result = resolve("riscv64")
        self.assertEqual(result.returncode, 2)
        self.assertIn("go install mvdan.cc/sh/v3/cmd/shfmt@v3.13.1", result.stderr)

    def test_an_installed_shfmt_skips_the_architecture_check(self) -> None:
        with tempfile.TemporaryDirectory(prefix="shfmt-installed-") as directory:
            temporary = Path(directory)
            binary = temporary / "bin"
            binary.mkdir()
            (binary / "shfmt").write_text("#!/usr/bin/env sh\nexit 0\n", encoding="utf-8")
            (binary / "shfmt").chmod(0o755)
            marker = temporary / "curl-called"
            (binary / "curl").write_text(
                f"#!/usr/bin/env sh\n: > {posix(marker)}\nexit 1\n", encoding="utf-8"
            )
            (binary / "curl").chmod(0o755)
            env = {
                "PATH": f"{posix(binary)}:/usr/bin:/bin",
                "HOME": os.environ.get("HOME", str(temporary)),
                "VMAFX_REPO_ROOT": posix(ROOT),
            }
            result = run_command(
                [str(BASH), posix(INSTALL), "riscv64"],
                allowed_executables=(str(BASH),),
                capture_output=True,
                text=True,
                errors="replace",
                check=False,
                env=env,
                cwd=temporary,
                timeout_seconds=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(marker.exists())

    def test_both_setup_scripts_call_the_shared_installer(self) -> None:
        for script in (UBUNTU, FEDORA):
            with self.subTest(script=script.name):
                text = script.read_text(encoding="utf-8")
                self.assertIn('bash "$SCRIPT_DIR/install-shfmt.sh"', text)
                self.assertNotIn("SHFMT_VERSION=", text)
                self.assertNotIn("linux_amd64", text)

    def test_the_digest_is_checked_before_install(self) -> None:
        text = INSTALL.read_text(encoding="utf-8")
        self.assertLess(text.index("sha256sum"), text.index("install -m 0755"))
        self.assertNotIn("$SUDO curl", text)


if __name__ == "__main__":
    unittest.main()
