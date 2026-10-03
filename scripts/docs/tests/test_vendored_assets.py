#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/docs/check_vendored_assets.py."""

import contextlib
import hashlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.docs import check_vendored_assets as check
from scripts.docs import vendor_fonts


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class VendoredAssetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-vendor-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.dir = self.root / "docs/assets/fonts/example"
        self.dir.mkdir(parents=True)
        self.files = {"Example.woff2": b"font bytes", "LICENSE.txt": b"licence text"}
        for name, data in self.files.items():
            (self.dir / name).write_bytes(data)
        self.manifest: dict[str, Any] = {
            "name": "Example",
            "version": "1.0",
            "source": "https://example.invalid/example-1.0.zip",
            "source_sha256": "0" * 64,
            "license": "OFL-1.1",
            "license_file": "LICENSE.txt",
            "files": {
                name: {"from": name, "sha256": digest(data)} for name, data in self.files.items()
            },
        }
        self.write_manifest()

    def write_manifest(self) -> None:
        (self.dir / "vendor.json").write_text(json.dumps(self.manifest))

    def run_check(self) -> tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            status = check.main(["--root", str(self.root)])
        return status, err.getvalue()

    def test_matching_manifest_passes(self) -> None:
        self.assertEqual(self.run_check(), (0, ""))

    def test_no_manifest_passes(self) -> None:
        (self.dir / "vendor.json").unlink()
        for name in self.files:
            (self.dir / name).unlink()
        self.assertEqual(self.run_check()[0], 0)

    def test_changed_file_fails(self) -> None:
        (self.dir / "Example.woff2").write_bytes(b"font bytes, edited")
        status, err = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("Example.woff2: sha256", err)

    def test_missing_file_fails(self) -> None:
        (self.dir / "Example.woff2").unlink()
        status, err = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("missing", err)

    def test_unlisted_file_fails(self) -> None:
        (self.dir / "Extra.woff2").write_bytes(b"unrecorded")
        status, err = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("Extra.woff2: not listed", err)

    def test_unlisted_licence_fails(self) -> None:
        del self.manifest["files"]["LICENSE.txt"]
        self.write_manifest()
        status, err = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("licence file 'LICENSE.txt' is not listed", err)

    def test_entry_without_hash_fails(self) -> None:
        self.manifest["files"]["Example.woff2"] = {"from": "Example.woff2"}
        self.write_manifest()
        status, err = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("no sha256", err)

    def test_unreadable_manifest_is_a_usage_error(self) -> None:
        (self.dir / "vendor.json").write_text("{not json")
        self.assertEqual(self.run_check()[0], 2)

    def test_manifest_without_required_key_is_a_usage_error(self) -> None:
        del self.manifest["source_sha256"]
        self.write_manifest()
        status, err = self.run_check()
        self.assertEqual(status, 2)
        self.assertIn("source_sha256", err)


class VendorFontsTests(unittest.TestCase):
    """The writer: archive and member hashes, the copy path and --check."""

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-vendor-fonts-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.licence = b"SIL OPEN FONT LICENSE fixture"
        self.archive = self.root / "release.zip"
        with zipfile.ZipFile(self.archive, "w") as zf:
            zf.writestr("upstream/LICENSE.txt", self.licence)
        self.dir = self.root / "fonts/example"
        self.dir.mkdir(parents=True)
        self.manifest_path = self.dir / "vendor.json"
        self.manifest: dict[str, Any] = {
            "source_sha256": digest(self.archive.read_bytes()),
            "files": {
                "LICENSE.txt": {"from": "upstream/LICENSE.txt", "from_sha256": digest(self.licence)}
            },
        }
        self.manifest_path.write_text(json.dumps(self.manifest))

    def run_tool(self, *extra: str) -> int:
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            return vendor_fonts.main(
                ["--manifest", str(self.manifest_path), "--archive", str(self.archive), *extra]
            )

    def test_write_copies_and_records_the_hash(self) -> None:
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual((self.dir / "LICENSE.txt").read_bytes(), self.licence)
        written = json.loads(self.manifest_path.read_text())
        self.assertEqual(written["files"]["LICENSE.txt"]["sha256"], digest(self.licence))
        self.assertEqual(self.run_tool("--check"), 0)

    def test_check_finds_an_edited_file(self) -> None:
        self.assertEqual(self.run_tool(), 0)
        (self.dir / "LICENSE.txt").write_bytes(b"edited")
        self.assertEqual(self.run_tool("--check"), 1)

    def test_other_archive_is_refused(self) -> None:
        self.manifest["source_sha256"] = "0" * 64
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.assertEqual(self.run_tool(), 1)
        self.assertFalse((self.dir / "LICENSE.txt").exists())

    def test_other_member_is_refused(self) -> None:
        self.manifest["files"]["LICENSE.txt"]["from_sha256"] = "0" * 64
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.assertEqual(self.run_tool(), 1)

    def test_missing_archive_is_a_usage_error(self) -> None:
        self.archive.unlink()
        self.assertEqual(self.run_tool(), 2)


if __name__ == "__main__":
    unittest.main()
