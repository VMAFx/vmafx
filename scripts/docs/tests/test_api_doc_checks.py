#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for documentation C sample extraction and API symbol reachability checks.

Planted negatives ensure:
1. Samples marked with <!-- compile --> missing #include fail extraction.
2. Undocumented symbols in vmafx_symbols.txt fail reachability.
Positive cases verify that real repository documentation and valid samples pass.
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.docs import check_api_doc_reachability as reachability  # noqa: E402
from scripts.docs import check_api_doc_samples as samples  # noqa: E402


class ApiDocSamplesTests(unittest.TestCase):
    """Test C sample extraction from markdown documentation."""

    def test_planted_negative_missing_include_fails(self) -> None:
        """Marked C sample without #include must fail naming page and line."""
        with tempfile.TemporaryDirectory() as tmp:
            docs_dir = Path(tmp) / "docs"
            docs_dir.mkdir()
            bad_page = docs_dir / "bad_sample.md"
            bad_page.write_text(
                "# Title\n\n<!-- compile -->\n```c\nint foo(void) { return 0; }\n```\n",
                encoding="utf-8",
            )
            out_dir = Path(tmp) / "out"
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                status = samples.main(["--docs-dir", str(docs_dir), "--extract", str(out_dir)])
            self.assertEqual(status, 1)
            msg = err.getvalue()
            self.assertIn("bad_sample.md:4", msg)
            self.assertIn("missing #include", msg)

    def test_positive_extraction(self) -> None:
        """Marked C sample with #include extracts correctly."""
        with tempfile.TemporaryDirectory() as tmp:
            docs_dir = Path(tmp) / "docs"
            docs_dir.mkdir()
            good_page = docs_dir / "good_sample.md"
            good_page.write_text(
                "# Title\n\n<!-- compile -->\n```c\n#include <stdio.h>\nint f(void) { return 0; }\n```\n",
                encoding="utf-8",
            )
            out_dir = Path(tmp) / "out"
            status = samples.main(["--docs-dir", str(docs_dir), "--extract", str(out_dir)])
            self.assertEqual(status, 0)
            target = out_dir / "good_sample_1.c"
            self.assertTrue(target.is_file())
            content = target.read_text(encoding="utf-8")
            self.assertIn("#include <stdio.h>", content)

    def test_list_mode(self) -> None:
        """List mode outputs paths of documents containing marked samples."""
        with tempfile.TemporaryDirectory() as tmp:
            docs_dir = Path(tmp) / "docs"
            docs_dir.mkdir()
            sample_page = docs_dir / "sample.md"
            sample_page.write_text(
                "# Title\n\n<!-- compile -->\n```c\n#include <stdio.h>\n```\n",
                encoding="utf-8",
            )
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = samples.main(["--docs-dir", str(docs_dir), "--list"])
            self.assertEqual(status, 0)
            self.assertIn("sample.md", out.getvalue())

    def test_unmarked_block_ignored(self) -> None:
        """Fenced C blocks without preceding <!-- compile --> are ignored."""
        with tempfile.TemporaryDirectory() as tmp:
            docs_dir = Path(tmp) / "docs"
            docs_dir.mkdir()
            unmarked_page = docs_dir / "unmarked.md"
            unmarked_page.write_text(
                "# Title\n\n```c\nint snippet_only = 1;\n```\n",
                encoding="utf-8",
            )
            out_dir = Path(tmp) / "out"
            status = samples.main(["--docs-dir", str(docs_dir), "--extract", str(out_dir)])
            self.assertEqual(status, 0)
            self.assertEqual(len(list(out_dir.glob("*.c"))), 0)

    def test_repo_docs_index_sample_extracted(self) -> None:
        """The real repo docs/api/vmafx/index.md sample is extracted properly."""
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            status = samples.main(["--docs-dir", str(ROOT / "docs"), "--extract", str(out_dir)])
            self.assertEqual(status, 0)
            target = out_dir / "api_vmafx_index_1.c"
            self.assertTrue(target.is_file())
            content = target.read_text(encoding="utf-8")
            self.assertIn("#include <vmafx/vmafx.h>", content)


class ApiDocReachabilityTests(unittest.TestCase):
    """Test reachability of exported VMAFx symbols in public documentation."""

    def test_planted_negative_missing_symbol_fails(self) -> None:
        """Undocumented symbol in symbols file must fail naming the symbol."""
        with tempfile.TemporaryDirectory() as tmp:
            sym_file = Path(tmp) / "fake_symbols.txt"
            sym_file.write_text("vmafx_planted_missing VMAFX_0.1\n", encoding="utf-8")
            docs_dir = Path(tmp) / "docs"
            docs_dir.mkdir()
            doc_file = docs_dir / "doc.md"
            doc_file.write_text("Some other vmafx_something documentation\n", encoding="utf-8")

            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                status = reachability.main(
                    ["--symbols", str(sym_file), "--docs-dirs", str(docs_dir)]
                )
            self.assertEqual(status, 1)
            msg = err.getvalue()
            self.assertIn("missing symbol from api docs: vmafx_planted_missing", msg)

    def test_repo_symbols_all_reachable(self) -> None:
        """All symbols in real repo core/src/vmafx_symbols.txt must be reachable."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = reachability.main([])
        self.assertEqual(status, 0)
        self.assertIn("137 symbols reached in docs", out.getvalue())


if __name__ == "__main__":
    unittest.main()
