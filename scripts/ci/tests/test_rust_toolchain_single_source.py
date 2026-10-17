#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify every Rust toolchain pin names build-config.env's RUST_VERSION (ADR-3318).

The Rust workflow installed "stable", developer hosts used whatever rustup had
and the dev container had no Rust at all, so the extractors were built by three
compilers. `check_rust_pins()` compares the copies that cannot source the
config: rust-toolchain.toml, both workspaces' `rust-version`, the workflow's
`toolchain:` input and the container's rustup install.
"""

from __future__ import annotations

import importlib.util
import tempfile
import textwrap
import unittest
from pathlib import Path
from typing import Protocol, cast

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RustPinGate(Protocol):
    def check_rust_pins(self, root: Path, rust_want: str | None) -> list[str]: ...

    def load_config(self, root: Path) -> dict[str, str]: ...


GATE = cast(RustPinGate, MODULE)
WANT = "1.99.0"
WORKFLOW = """\
    jobs:
      rust:
        steps:
          - uses: dtolnay/rust-toolchain@686976e1  # stable
            with:
              toolchain: {version}
    """
CONTAINER = """\
    RUN /tmp/rustup-init -y --profile minimal \\
        --default-toolchain "${{RUST_VERSION}}" --component rustfmt,clippy
    {extra}
    """


class RustToolchainSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        self.write("rust-toolchain.toml", f'[toolchain]\nchannel = "{WANT}"\n')
        for rel in ("Cargo.toml", "core/src/rust/Cargo.toml"):
            self.write(rel, f'[workspace.package]\nrust-version = "{WANT}"\n')
        self.write(".github/workflows/rust-ci.yml", textwrap.dedent(WORKFLOW).format(version=WANT))
        self.write("dev/Containerfile", textwrap.dedent(CONTAINER).format(extra=""))

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def check(self, want: str | None = WANT) -> list[str]:
        return GATE.check_rust_pins(self.repo, want)

    def test_repository_is_in_sync(self) -> None:
        """The tracked tree satisfies its own contract."""
        want = GATE.load_config(ROOT).get("RUST_VERSION")
        self.assertEqual(GATE.check_rust_pins(ROOT, want), [])

    def test_matching_pins_pass(self) -> None:
        self.assertEqual(self.check(), [])

    def test_missing_rust_version_fails(self) -> None:
        problems = self.check(None)
        self.assertEqual(len(problems), 1)
        self.assertIn("no RUST_VERSION", problems[0])

    def test_channel_drift_fails(self) -> None:
        self.write("rust-toolchain.toml", '[toolchain]\nchannel = "stable"\n')
        problems = self.check()
        self.assertEqual(len(problems), 1)
        self.assertIn("channel 'stable'", problems[0])

    def test_missing_toolchain_file_fails(self) -> None:
        (self.repo / "rust-toolchain.toml").unlink()
        self.assertIn("rust-toolchain.toml: no [toolchain] channel", self.check()[0])

    def test_manifest_drift_fails(self) -> None:
        self.write("core/src/rust/Cargo.toml", '[workspace.package]\nrust-version = "1.85"\n')
        problems = self.check()
        self.assertEqual(len(problems), 1)
        self.assertIn("core/src/rust/Cargo.toml", problems[0])
        self.assertIn("'1.85'", problems[0])

    def test_manifest_without_rust_version_fails(self) -> None:
        self.write("Cargo.toml", "[workspace]\nmembers = []\n")
        self.assertIn("rust-version is 'None'", self.check()[0])

    def test_workflow_stable_fails(self) -> None:
        """The unpinned "stable" input this gate replaced."""
        self.write(
            ".github/workflows/rust-ci.yml", textwrap.dedent(WORKFLOW).format(version="stable")
        )
        problems = self.check()
        self.assertEqual(len(problems), 1)
        self.assertIn("toolchain 'stable'", problems[0])

    def test_workflow_without_the_action_is_ignored(self) -> None:
        """A `toolchain:` key of another action is not a Rust pin (boundary)."""
        self.write(".github/workflows/other.yml", "with:\n  toolchain: 12.4\n")
        self.assertEqual(self.check(), [])

    def test_container_without_the_config_variable_fails(self) -> None:
        self.write("dev/Containerfile", "RUN rustup-init -y --default-toolchain 1.99.0\n")
        problems = self.check()
        self.assertEqual(len(problems), 2)
        self.assertIn('--default-toolchain "${RUST_VERSION}"', problems[0])
        self.assertIn("names Rust 1.99.0 literally", problems[1])

    def test_container_literal_next_to_the_variable_fails(self) -> None:
        self.write(
            "dev/Containerfile",
            textwrap.dedent(CONTAINER).format(extra="RUN rustup toolchain install +1.98.1"),
        )
        problems = self.check()
        self.assertEqual(len(problems), 1)
        self.assertIn("1.98.1", problems[0])

    def test_tree_without_container_passes(self) -> None:
        """A checkout without dev/ (boundary) has nothing to compare there."""
        (self.repo / "dev/Containerfile").unlink()
        self.assertEqual(self.check(), [])


STATICLIB_SPEC = importlib.util.spec_from_file_location(
    "build_staticlib", ROOT / "core/src/rust/build_staticlib.py"
)
assert STATICLIB_SPEC is not None and STATICLIB_SPEC.loader is not None
STATICLIB = importlib.util.module_from_spec(STATICLIB_SPEC)
STATICLIB_SPEC.loader.exec_module(STATICLIB)


class CargoRunsWhereTheToolchainFileIs(unittest.TestCase):
    """rustup reads rust-toolchain.toml from the working directory upwards.

    Meson runs the archive build in its build directory, outside the checkout,
    where rustup would pick its default toolchain; build_staticlib.py therefore
    runs cargo in the manifest's directory.
    """

    def run_stub(self, exit_code: int) -> tuple[int, Path, Path]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        base = Path(temporary.name)
        manifest = base / "checkout/core/src/rust/Cargo.toml"
        manifest.parent.mkdir(parents=True)
        manifest.write_text("[workspace]\n", encoding="utf-8")
        record = base / "cwd.txt"
        cargo = base / "cargo"
        cargo.write_text(f"#!/bin/sh\npwd > '{record}'\nexit {exit_code}\n", encoding="utf-8")
        cargo.chmod(0o755)
        build = base / "build"
        build.mkdir()
        args = STATICLIB.parse_args(
            ["--cargo", str(cargo), "--manifest", str(manifest), "--target-dir", str(build),
             "--output", str(build / "a.a"), "--depfile", str(build / "a.d")]
        )  # fmt: skip
        return STATICLIB.run_cargo(args), record, manifest.parent

    def test_cargo_runs_in_the_manifest_directory(self) -> None:
        code, record, manifest_dir = self.run_stub(0)
        self.assertEqual(code, 0)
        self.assertEqual(Path(record.read_text().strip()).resolve(), manifest_dir.resolve())

    def test_cargo_failure_is_returned(self) -> None:
        code, _, _ = self.run_stub(101)
        self.assertEqual(code, 101)


if __name__ == "__main__":
    unittest.main()
