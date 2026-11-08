#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/check_api_doc_coverage.py."""

from __future__ import annotations

import io
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.ci import check_api_doc_coverage as coverage  # noqa: E402


class CheckApiDocCoverageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.headers_dir = self.root / "include" / "vmafx"
        self.docs_dir = self.root / "docs" / "api"
        self.headers_dir.mkdir(parents=True)
        self.docs_dir.mkdir(parents=True)

    def _run_main(self, *extra_args: str) -> tuple[int, str, str]:
        cmd = [
            "--headers-dir",
            str(self.headers_dir),
            "--docs-dir",
            str(self.docs_dir),
            *extra_args,
        ]
        out_buf = io.StringIO()
        err_buf = io.StringIO()
        with redirect_stdout(out_buf), redirect_stderr(err_buf):
            code = coverage.main(cmd)
        return code, out_buf.getvalue(), err_buf.getvalue()

    def test_positive_all_documented(self) -> None:
        header = self.headers_dir / "sample.h"
        header.write_text(
            "#ifndef VMAFX_SAMPLE_H\n"
            "#define VMAFX_SAMPLE_H\n"
            "#define VMAFX_SAMPLE_MACRO 42\n"
            "typedef struct VmafxSample VmafxSample;\n"
            "int vmafx_sample_call(void);\n"
            "#endif\n",
            encoding="utf-8",
        )
        doc = self.docs_dir / "sample.md"
        doc.write_text(
            "# Sample\n" "Use `VmafxSample` with `vmafx_sample_call` and `VMAFX_SAMPLE_MACRO`.\n",
            encoding="utf-8",
        )
        code, out, err = self._run_main()
        self.assertEqual(code, 0, f"Expected 0, got {code}; stderr: {err}")
        self.assertIn("all 3 public symbols documented", out)

    def test_negative_undocumented_function(self) -> None:
        header = self.headers_dir / "feature.h"
        header.write_text(
            "int vmafx_documented_fn(void);\n" "int vmafx_undocumented_fn(void);\n",
            encoding="utf-8",
        )
        doc = self.docs_dir / "guide.md"
        doc.write_text("Only `vmafx_documented_fn` is here.\n", encoding="utf-8")
        code, _out, err = self._run_main()
        self.assertEqual(code, 1)
        self.assertIn("vmafx_undocumented_fn", err)

    def test_boundary_comment_and_code_fence(self) -> None:
        header = self.headers_dir / "boundary.h"
        header.write_text(
            "// vmafx_in_line_comment(void);\n"
            "/* vmafx_in_block_comment(void); */\n"
            "/*\n"
            " * VMAFX_MULTI_LINE_COMMENT\n"
            " */\n"
            "int vmafx_real_fn(void);\n",
            encoding="utf-8",
        )
        doc = self.docs_dir / "boundary.md"
        doc.write_text(
            "# Boundary Doc\n" "```c\n" "int x = vmafx_real_fn();\n" "```\n",
            encoding="utf-8",
        )
        code, out, err = self._run_main()
        self.assertEqual(code, 0, f"Expected 0, got {code}; stderr: {err}")
        self.assertIn("all 1 public symbols documented", out)

    def test_allowlist_suppresses_missing(self) -> None:
        header = self.headers_dir / "allow.h"
        header.write_text("int vmafx_skipped_fn(void);\n", encoding="utf-8")
        allow_file = self.root / "allow.txt"
        allow_file.write_text(
            "# Comment line\n" "vmafx_skipped_fn internal_testing_only\n",
            encoding="utf-8",
        )
        code, out, err = self._run_main("--allow", str(allow_file))
        self.assertEqual(code, 0, f"Expected 0, got {code}; stderr: {err}")
        self.assertIn("all 1 public symbols documented", out)

    def test_allowlist_requires_nonempty_reason(self) -> None:
        header = self.headers_dir / "allow.h"
        header.write_text("int vmafx_skipped_fn(void);\n", encoding="utf-8")
        allow_file = self.root / "allow_bad.txt"
        allow_file.write_text("vmafx_skipped_fn\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            coverage.load_allowlist(allow_file)
        code, _out, err = self._run_main("--allow", str(allow_file))
        self.assertEqual(code, 1)
        self.assertIn("missing required reason column", err)

    def test_libvmaf_bridge_exclusion(self) -> None:
        bridge = self.headers_dir / "libvmaf_bridge.h"
        bridge.write_text(
            "#ifndef VMAFX_LIBVMAF_BRIDGE_H\n"
            "#define VMAFX_LIBVMAF_BRIDGE_H\n"
            "int legacy_bridge_only(void);\n"
            "typedef struct VmafxBridge VmafxBridge;\n"
            "#endif\n",
            encoding="utf-8",
        )
        doc = self.docs_dir / "bridge.md"
        doc.write_text("Documenting `VmafxBridge`.\n", encoding="utf-8")
        code, out, err = self._run_main()
        self.assertEqual(code, 0, f"Expected 0, got {code}; stderr: {err}")
        self.assertIn("all 1 public symbols documented", out)

    def test_multiline_macro_directive_continuation(self) -> None:
        header = self.headers_dir / "multiline.h"
        header.write_text(
            "#ifndef VMAFX_MULTILINE_H\n"
            "#define VMAFX_MULTILINE_H\n"
            "#define VMAFX_MULTILINE_MACRO(x) \\\n"
            "    vmafx_internal_macro_helper(x)\n"
            "#endif\n",
            encoding="utf-8",
        )
        doc = self.docs_dir / "multiline.md"
        doc.write_text("Documenting `VMAFX_MULTILINE_MACRO` here.\n", encoding="utf-8")
        code, out, err = self._run_main()
        self.assertEqual(code, 0, f"Expected 0, got {code}; stderr: {err}")
        self.assertIn("all 1 public symbols documented", out)

    def test_anonymous_aggregate_and_array_typedefs(self) -> None:
        header = self.headers_dir / "anon.h"
        header.write_text(
            "#ifndef VMAFX_ANON_H\n"
            "#define VMAFX_ANON_H\n"
            "typedef struct { int x; } VmafxAnonPoint;\n"
            "typedef enum { VMAFX_ANON_VAL = 0 } VmafxAnonEnum;\n"
            "typedef union { int i; float f; } VmafxAnonUnion;\n"
            "typedef uint8_t VmafxDigest[32];\n"
            "#endif\n",
            encoding="utf-8",
        )
        doc = self.docs_dir / "anon.md"
        doc.write_text("Only `VmafxAnonPoint` and `VMAFX_ANON_VAL`.\n", encoding="utf-8")
        code, _out, err = self._run_main()
        self.assertEqual(code, 1)
        self.assertIn("VmafxAnonEnum", err)
        self.assertIn("VmafxAnonUnion", err)
        self.assertIn("VmafxDigest", err)

        doc.write_text(
            "`VmafxAnonPoint`, `VmafxAnonEnum`, `VMAFX_ANON_VAL`, `VmafxAnonUnion`, `VmafxDigest`\n",
            encoding="utf-8",
        )
        code, out, err = self._run_main()
        self.assertEqual(code, 0, f"Expected 0, got {code}; stderr: {err}")
        self.assertIn("all 5 public symbols documented", out)

    def test_hidden_markdown_html_comments_and_link_destinations(self) -> None:
        header = self.headers_dir / "hidden.h"
        header.write_text(
            "#ifndef VMAFX_HIDDEN_H\n"
            "#define VMAFX_HIDDEN_H\n"
            "int vmafx_hidden_fn(void);\n"
            "int vmafx_visible_fn(void);\n"
            "#endif\n",
            encoding="utf-8",
        )
        doc = self.docs_dir / "hidden.md"
        doc.write_text(
            "# Hidden Test\n"
            "<!-- vmafx_hidden_fn in comment -->\n"
            "[Visible](https://example.com/vmafx_hidden_fn)\n"
            "[label]: https://example.com/vmafx_hidden_fn\n"
            "Visible mention of `vmafx_visible_fn`.\n",
            encoding="utf-8",
        )
        code, _out, err = self._run_main()
        self.assertEqual(code, 1)
        self.assertIn("vmafx_hidden_fn", err)

        doc.write_text(
            "```c\n" "vmafx_hidden_fn();\n" "vmafx_visible_fn();\n" "```\n",
            encoding="utf-8",
        )
        code, out, err = self._run_main()
        self.assertEqual(code, 0, f"Expected 0, got {code}; stderr: {err}")
        self.assertIn("all 2 public symbols documented", out)

    def test_header_guard_distinguished_from_conditional_macro(self) -> None:
        header = self.headers_dir / "guards.h"
        header.write_text(
            "#ifndef VMAFX_GUARDS_H\n"
            "#define VMAFX_GUARDS_H\n"
            "#ifndef VMAFX_CONDITIONAL_FEATURE\n"
            "#define VMAFX_CONDITIONAL_FEATURE 1\n"
            "#endif\n"
            "#endif\n",
            encoding="utf-8",
        )
        doc = self.docs_dir / "guards.md"
        doc.write_text("No macro mentioned here.\n", encoding="utf-8")
        code, _out, err = self._run_main()
        self.assertEqual(code, 1)
        self.assertIn("VMAFX_CONDITIONAL_FEATURE", err)

        doc.write_text("Feature flag: `VMAFX_CONDITIONAL_FEATURE`.\n", encoding="utf-8")
        code, out, err = self._run_main()
        self.assertEqual(code, 0, f"Expected 0, got {code}; stderr: {err}")
        self.assertIn("all 1 public symbols documented", out)

    def test_unterminated_nested_aggregate_is_linear(self) -> None:
        """An unterminated typedef with many nested braces must not backtrack exponentially."""
        text = "typedef struct {" + " {int a;}" * 24 + " } VmafxNoSemicolon"
        started = time.monotonic()
        types = coverage.collect_types(text)
        self.assertLess(time.monotonic() - started, 1.0)
        self.assertNotIn("VmafxNoSemicolon", types)

    def test_nested_aggregate_typedef_is_collected(self) -> None:
        text = "typedef struct { struct { int a; } inner; int b; } VmafxNested;"
        self.assertIn("VmafxNested", coverage.collect_types(text))


if __name__ == "__main__":
    unittest.main()
