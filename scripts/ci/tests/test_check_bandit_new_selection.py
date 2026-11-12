#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Classification, shebang, size-limit, encoding, and pathspec tests for check_bandit_new."""

from __future__ import annotations

import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from identify.identify import tags_from_filename

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_bandit_new as check
from scripts.ci.tests.bandit_new_test_support import BASE, UNSAFE, BanditNewTestCase
from scripts.lib import new_scanner_findings as shared


class CheckBanditNew(BanditNewTestCase):
    def test_python_hook_types_and_shebang_are_scanned(self) -> None:
        """Selected suffixes and shebangs expose B603; plain text and missing files pass."""
        self.assertIn("python", tags_from_filename("x.pyw"))
        self.assertIn("pyi", tags_from_filename("x.pyi"))
        for name in ("fresh.pyw", "fresh.pyi", "fresh.pyt", "fresh.wsgi", "executable"):
            with self.subTest(name=name):
                source = "#!/usr/bin/env python3\n" + BASE + UNSAFE
                path = self.root / name
                path.write_text(source, encoding="utf-8", newline="\n")
                path.chmod(0o755)
                status, out, err = self.invoke(name)
                self.assertEqual(status, 1, err)
                self.assertIn("B603", out)
        self.assertEqual(self.run_check("plain.txt", BASE + UNSAFE)[0], 0)
        self.assertEqual(self.invoke("missing.py")[0], 0)

    def test_extensionless_python_shebang_scanned_without_x_ok(self) -> None:
        """An extensionless Python script reports B603 even when X_OK is denied."""
        source = "#!/usr/bin/env python3\n" + BASE + UNSAFE
        (self.root / "script").write_text(source, encoding="utf-8", newline="\n")
        temp_root = Path(tempfile.gettempdir()).resolve()
        real_access = os.access

        def noexec_access(
            path: int | str | bytes | os.PathLike[str] | os.PathLike[bytes],
            mode: int,
            *,
            dir_fd: int | None = None,
            effective_ids: bool = False,
            follow_symlinks: bool = True,
        ) -> bool:
            if mode & os.X_OK and not isinstance(path, int):
                if Path(os.fsdecode(path)).resolve().is_relative_to(temp_root):
                    return False
            return real_access(
                path,
                mode,
                dir_fd=dir_fd,
                effective_ids=effective_ids,
                follow_symlinks=follow_symlinks,
            )

        with mock.patch("os.access", side_effect=noexec_access):
            self.assertFalse(os.access(self.root / "script", os.X_OK))
            status, out, err = self.invoke("script")
        self.assertEqual(status, 1, err)
        self.assertIn("script:7: B603", out)

    def test_extensionless_non_python_and_unshebanged_not_scanned(self) -> None:
        """Extensionless shell scripts and unshebanged files never reach Bandit."""
        cases = {
            "shell_script": "#!/bin/sh\n" + BASE + UNSAFE,
            "plain_blob": BASE + UNSAFE,
        }
        with mock.patch.object(
            check, "findings", side_effect=AssertionError("non-Python file scanned")
        ) as scan:
            for name, content in cases.items():
                with self.subTest(name=name):
                    (self.root / name).write_text(content, encoding="utf-8", newline="\n")
                    status, out, err = self.invoke(name)
                    self.assertEqual(status, 0, err)
                    self.assertEqual(out, "")
        scan.assert_not_called()

    def test_extensionless_shebang_styles_and_empty_file(self) -> None:
        """Direct and flag-bearing Python shebangs scan; empty, bare, and overlong lines skip."""
        for name, header in (
            ("bare_python", "#!python\n"),
            ("unbuffered_python", "#!/usr/bin/python3 -u\n"),
        ):
            with self.subTest(name=name):
                (self.root / name).write_text(
                    header + BASE + UNSAFE, encoding="utf-8", newline="\n"
                )
                status, out, err = self.invoke(name)
                self.assertEqual(status, 1, err)
                self.assertIn(f"{name}:7: B603", out)
        skipped = {
            "empty_file": "",
            "bare_hashbang": "#!\n",
        }
        with mock.patch.object(
            check, "findings", side_effect=AssertionError("boundary non-Python file scanned")
        ) as scan:
            for name, content in skipped.items():
                with self.subTest(name=name):
                    (self.root / name).write_text(content, encoding="utf-8", newline="\n")
                    status, out, err = self.invoke(name)
                    self.assertEqual(status, 0, err)
                    self.assertEqual(out, "")
        scan.assert_not_called()

    def test_nix_shell_python_shebang_is_scanned(self) -> None:
        """An extensionless nix-shell script declaring -i python3 is tagged and scanned."""
        header = "#!/usr/bin/env nix-shell\n#! nix-shell -i python3 -p python3\n"
        target = self.root / "nix_python"
        target.write_text(header + BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.assertTrue(check.is_python_source(target))
        status, out, err = self.invoke("nix_python")
        self.assertEqual(status, 1, err)
        self.assertIn("nix_python:8: B603", out)

    def test_plain_python_and_shell_shebang_classification(self) -> None:
        """Bounded prefix reads still tag env python3 scripts and skip /bin/sh scripts."""
        py_script = self.root / "env_python"
        py_script.write_text(
            "#!/usr/bin/env python3\n" + BASE + UNSAFE, encoding="utf-8", newline="\n"
        )
        sh_script = self.root / "bin_sh"
        sh_script.write_text("#!/bin/sh\n" + BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.assertTrue(check.is_python_source(py_script))
        self.assertFalse(check.is_python_source(sh_script))
        status, out, err = self.invoke("env_python")
        self.assertEqual(status, 1, err)
        self.assertIn("env_python:7: B603", out)
        with mock.patch.object(
            check, "findings", side_effect=AssertionError("shell script scanned")
        ) as scan:
            self.assertEqual(self.invoke("bin_sh"), (0, "", ""))
        scan.assert_not_called()

    def test_nix_shell_interpreter_beyond_shebang_limit_not_tagged(self) -> None:
        """A nix-shell interpreter line past MAX_SHEBANG_BYTES stays untagged without full read."""
        first = "#!/usr/bin/env nix-shell\n"
        comment = "#! nix-shell -p "
        padding = "x" * (check.MAX_SHEBANG_BYTES - len(first) - len(comment) - 1)
        source = first + comment + padding + "\n#! nix-shell -i python3\n" + BASE + UNSAFE
        target = self.root / "nix_overlong"
        target.write_text(source, encoding="utf-8", newline="\n")
        self.assertEqual(source.index("#! nix-shell -i python3"), check.MAX_SHEBANG_BYTES)
        self.assertFalse(check.is_python_source(target))
        with mock.patch.object(
            check, "findings", side_effect=AssertionError("beyond-limit shebang scanned")
        ) as scan:
            self.assertEqual(self.invoke("nix_overlong"), (0, "", ""))
        scan.assert_not_called()

    def test_current_bytes_rejects_oversized_file(self) -> None:
        """A file one byte over MAX_SOURCE_BYTES or growing past it after stat raises ToolError."""
        git = shutil.which("git")
        assert git is not None
        comparison = shared.Comparison(git, self.root, "master")
        oversized = self.root / "oversized.py"
        with oversized.open("wb") as handle:
            handle.truncate(shared.MAX_SOURCE_BYTES + 1)
        real_open = Path.open
        opened: list[Path] = []
        pattern = rf"{re.escape(str(oversized))}.*{shared.MAX_SOURCE_BYTES}"

        def record_open(path: Path, *args: Any, **kwargs: Any) -> Any:
            opened.append(path)
            return real_open(path, *args, **kwargs)

        with (
            mock.patch.object(Path, "open", autospec=True, side_effect=record_open),
            self.assertRaisesRegex(shared.ToolError, pattern),
        ):
            comparison.current_bytes("oversized.py")
        self.assertEqual(opened, [])
        status, _, err = self.invoke("oversized.py")
        self.assertEqual(status, 2)
        self.assertIn(str(oversized), err)

        growing = self.root / "growing.py"
        growing.write_bytes(b"x\n")
        grow_pattern = rf"{re.escape(str(growing))}.*{shared.MAX_SOURCE_BYTES}"

        def grow_then_open(path: Path, *args: Any, **kwargs: Any) -> Any:
            if path == growing:
                os.truncate(growing, shared.MAX_SOURCE_BYTES + 64)
            return real_open(path, *args, **kwargs)

        with (
            mock.patch.object(Path, "open", autospec=True, side_effect=grow_then_open),
            self.assertRaisesRegex(shared.ToolError, grow_pattern),
        ):
            comparison.current_bytes("growing.py")

    def test_current_bytes_accepts_exact_size_limit(self) -> None:
        """A regular file of exactly MAX_SOURCE_BYTES returns its complete byte content."""
        git = shutil.which("git")
        assert git is not None
        comparison = shared.Comparison(git, self.root, "master")
        exact = self.root / "exact.py"
        with exact.open("wb") as handle:
            handle.write(b"#")
            handle.seek(shared.MAX_SOURCE_BYTES - 1)
            handle.write(b"\n")
        content = comparison.current_bytes("exact.py")
        assert content is not None
        self.assertEqual(len(content), shared.MAX_SOURCE_BYTES)
        self.assertEqual((content[:1], content[-1:]), (b"#", b"\n"))

    def test_current_bytes_non_file_and_missing_return_none(self) -> None:
        """Directories and missing paths still return None from current_bytes and copy."""
        git = shutil.which("git")
        assert git is not None
        comparison = shared.Comparison(git, self.root, "master")
        (self.root / "subdir").mkdir()
        for name in ("subdir", "missing_path.py"):
            with self.subTest(name=name):
                self.assertIsNone(comparison.current_bytes(name))
                self.assertIsNone(comparison.copy(self.root / name, self.root / "copied"))

    def test_pep263_current_and_base(self) -> None:
        """Encoded source reports a new finding, then passes when the finding exists at base."""
        source = "# coding: latin-1\n# café\n" + BASE + UNSAFE
        path = self.root / "harness.py"
        path.write_bytes(source.encode("latin-1"))
        self.assertEqual(self.invoke("harness.py")[0], 1)
        self.commit_base()
        path.write_bytes((source + "# café moved\n").encode("latin-1"))
        self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_utf8_bom_and_invalid_encoding(self) -> None:
        """A UTF-8 BOM preserves finding detection; an unknown encoding returns 2."""
        path = self.root / "harness.py"
        path.write_bytes((BASE + UNSAFE).encode("utf-8-sig"))
        self.assertEqual(self.invoke("harness.py")[0], 1)
        path.write_bytes(b"# coding: nonexistent-encoding\n")
        self.assertEqual(self.invoke("harness.py")[0], 2)

    def test_literal_metacharacter_base_path(self) -> None:
        """Bracketed filenames retain their base findings when source lines move."""
        for name in ("module[1].py", "module[2].py"):
            (self.root / name).write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.commit_base()
        for name in ("module[1].py", "module[2].py"):
            with self.subTest(name=name):
                self.assertEqual(self.run_check(name, "# moved\n" + BASE + UNSAFE)[0], 0)

    def test_literal_pathspec_magic_blob(self) -> None:
        # Build the tree directly: ':' and '*' filenames need no filesystem support.
        """A pathspec-like filename returns its literal blob; an absent path returns no bytes."""
        blob = self.git("hash-object", "-w", "--stdin", input_text=BASE + UNSAFE).strip()
        name = ":(glob)module*.py"
        tree = self.git("mktree", "-z", input_text=f"100644 blob {blob}\t{name}\0").strip()
        git = shutil.which("git")
        assert git is not None
        self.assertEqual(shared.base_text(git, self.root, tree, name), (BASE + UNSAFE).encode())
        self.assertEqual(shared.base_text(git, self.root, tree, "absent.py"), b"")

    def test_low_similarity_rename_fails_closed(self) -> None:
        """An unrecognized rename reports its retained B603 finding as new and returns 1."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.commit_base()
        self.git("mv", "harness.py", "renamed.py")
        status, out, err = self.run_check("renamed.py", BASE + UNSAFE + "# new\n" * 100)
        self.assertEqual(status, 1, err)
        self.assertIn("B603", out)


if __name__ == "__main__":
    unittest.main()
