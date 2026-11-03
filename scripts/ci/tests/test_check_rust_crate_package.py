#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Packaged-crate acceptance: stub builds, planted failures and strict size boundaries."""

from __future__ import annotations

import io
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.ci import check_rust_crate_package as check


def write_archive(path: Path, members: dict[str, bytes]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(path, "w:gz") as archive:
        for name, contents in members.items():
            member = tarfile.TarInfo(name)
            member.size = len(contents)
            archive.addfile(member, io.BytesIO(contents))


class PackageChecks(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="test-rust-crate-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        source = self.root / "core/include/libvmaf"
        source.mkdir(parents=True)
        for name in check.DOC_HEADERS:
            (source / name).write_bytes(f"/* {name} */\n".encode())
        check.sync_headers(self.root, write=True)
        self.calls: list[tuple[list[str], Path, dict[str, str]]] = []
        self.fail_docs: str | None = None
        self.omit_header = False

    def runner(self, command: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
        cwd, env = kwargs["cwd"], kwargs["env"]
        self.calls.append((command, cwd, env))
        if command[:3] == ["rustc", "--print", "sysroot"]:
            return subprocess.CompletedProcess(command, 0, stdout=str(self.root / "rust"))
        if command[1] == "package":
            self.package_fixtures(Path(env["CARGO_TARGET_DIR"]) / "package")
        elif command[1] == "doc":
            self.assertTrue((cwd / "Cargo.toml").is_file())
            self.assertFalse(cwd.is_relative_to(self.root))
            self.assertEqual(env["DOCS_RS"], "1")
            for tool in ("nvcc", "hipcc", "dpcpp", "icpx"):
                self.assertIsNone(shutil.which(tool, path=env["PATH"]))
            missing = (
                cwd.name.startswith("vmafx-sys-")
                and not (cwd / "include/libvmaf/libvmaf.h").exists()
            )
            if cwd.name == f"{self.fail_docs}-0.1.0" or missing:
                raise subprocess.CalledProcessError(101, command, stderr="planted doc failure")
        return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

    def package_fixtures(self, package_dir: Path) -> None:
        for name in check.CRATES:
            base = f"{name}-0.1.0"
            members = {f"{base}/Cargo.toml": f'[package]\nname = "{name}"\n'.encode()}
            if name == "vmafx-sys" and not self.omit_header:
                members[f"{base}/include/libvmaf/libvmaf.h"] = b"/* packaged header */"
            write_archive(package_dir / f"{base}.crate", members)

    def test_packaged_docs_pass_with_stub_runner(self) -> None:
        check.check_packages(self.root, self.runner)
        package = [command for command, _, _ in self.calls if command[1] == "package"]
        self.assertEqual(len(package), 1)
        self.assertIn("--allow-dirty", package[0])
        for name in check.CRATES:
            self.assertIn(name, package[0])
        docs = [(command, cwd) for command, cwd, _ in self.calls if command[1] == "doc"]
        self.assertEqual(len(docs), len(check.CRATES))
        self.assertIn(str(docs[0][1]), docs[1][0][-1])

    def test_doc_failure_is_named_and_refused(self) -> None:
        for name in check.CRATES:
            with self.subTest(crate=name):
                self.fail_docs = name
                with self.assertRaisesRegex(
                    check.PackageError, f"DOCS_RS doc build failed.*{name}"
                ):
                    check.check_packages(self.root, self.runner)

    def test_planted_missing_packaged_header_is_refused(self) -> None:
        self.omit_header = True
        with self.assertRaisesRegex(check.PackageError, "planted doc failure"):
            check.check_packages(self.root, self.runner)

    def test_package_failure_is_named_and_refused(self) -> None:
        def runner(command, **kwargs):
            if command[1] == "package":
                raise subprocess.CalledProcessError(101, command, stderr="planted package failure")
            return self.runner(command, **kwargs)

        with self.assertRaisesRegex(check.PackageError, "cargo package failed.*planted"):
            check.check_packages(self.root, runner)

    def test_missing_artifact_is_refused(self) -> None:
        with self.assertRaisesRegex(check.PackageError, "missing or ambiguous packaged crate"):
            check.packaged_crate(self.root, "vmafx-sys")

    def test_temporary_directory_inside_checkout_is_refused(self) -> None:
        with patch.object(check.tempfile, "TemporaryDirectory") as temporary:
            temporary.return_value.__enter__.return_value = str(self.root / "inside")
            with self.assertRaisesRegex(check.PackageError, "outside the checkout"):
                check.check_packages(self.root, self.runner)
        self.assertEqual(self.calls, [])

    def sized_archive(self, size: int) -> Path:
        archive = self.root / "vmafx-sys-0.1.0.crate"
        with archive.open("wb") as output:
            output.truncate(size)
        return archive

    def test_oversized_crate_is_refused(self) -> None:
        archive = self.sized_archive(check.MAX_CRATE_BYTES + 1)
        with self.assertRaisesRegex(check.PackageError, "oversized crate"):
            check.check_size(archive)

    def test_exactly_ten_mib_is_refused(self) -> None:
        archive = self.sized_archive(check.MAX_CRATE_BYTES)
        with self.assertRaisesRegex(check.PackageError, "oversized crate"):
            check.check_size(archive)

    def test_one_byte_below_ten_mib_is_accepted(self) -> None:
        check.check_size(self.sized_archive(check.MAX_CRATE_BYTES - 1))

    def test_synced_headers_match(self) -> None:
        check.sync_headers(self.root)

    def test_planted_header_drift_is_refused(self) -> None:
        header = self.root / "core/include/libvmaf/libvmaf.h"
        header.write_bytes(b"/* changed upstream */")
        with self.assertRaisesRegex(check.PackageError, "header drift"):
            check.sync_headers(self.root)

    def test_missing_header_copy_is_refused(self) -> None:
        header = self.root / "bindings/rust/vmafx-sys/include/libvmaf/libvmaf.h"
        header.unlink()
        with self.assertRaisesRegex(check.PackageError, "header drift"):
            check.sync_headers(self.root)

    def test_path_traversal_is_refused(self) -> None:
        archive = self.root / "vmafx-sys-0.1.0.crate"
        write_archive(archive, {"vmafx-sys-0.1.0/../../escape": b"bad"})
        with self.assertRaisesRegex(check.PackageError, "unsafe crate member"):
            check.unpack_crate(archive, self.root / "extracted")

    def test_symlink_is_refused(self) -> None:
        archive = self.root / "vmafx-sys-0.1.0.crate"
        with tarfile.open(archive, "w:gz") as contents:
            member = tarfile.TarInfo("vmafx-sys-0.1.0/link")
            member.type = tarfile.SYMTYPE
            member.linkname = "../outside"
            contents.addfile(member)
        with self.assertRaisesRegex(check.PackageError, "unsafe crate member"):
            check.unpack_crate(archive, self.root / "extracted")


if __name__ == "__main__":
    unittest.main()
