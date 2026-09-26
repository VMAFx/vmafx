#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Protect the ownership and stable-enum contracts in public GPU headers."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

INCLUDE_DIR = Path(__file__).resolve().parents[1] / "include" / "libvmaf"


def _doxygen_before(source: str, declaration: str) -> str:
    declaration_start = source.index(declaration)
    comment_start = source.rfind("/**", 0, declaration_start)
    if comment_start < 0:
        raise AssertionError(f"no Doxygen block before {declaration}")
    comment_end = source.find("*/", comment_start, declaration_start)
    if comment_end < 0:
        raise AssertionError(f"unterminated Doxygen block before {declaration}")
    if source[comment_end + 2 : declaration_start].strip():
        raise AssertionError(f"Doxygen block is not attached to {declaration}")
    block = source[comment_start : comment_end + 2]
    block = re.sub(r"^\s*/\*\*\s*$", "", block, flags=re.MULTILINE)
    block = re.sub(r"^\s*\*\s?", "", block, flags=re.MULTILINE)
    block = re.sub(r"\s*\*/\s*$", "", block, flags=re.MULTILINE)
    return re.sub(r"\s+", " ", block).strip()


class GpuPublicHeaderDocsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.cuda = (INCLUDE_DIR / "libvmaf_cuda.h").read_text(encoding="utf-8")
        self.sycl = (INCLUDE_DIR / "libvmaf_sycl.h").read_text(encoding="utf-8")

    def assert_patterns(self, text: str, *patterns: str) -> None:
        for pattern in patterns:
            with self.subTest(pattern=pattern):
                self.assertRegex(text, re.compile(pattern, re.IGNORECASE | re.DOTALL))

    def test_cuda_init_documents_output_ownership(self) -> None:
        block = _doxygen_before(self.cuda, "VMAF_EXPORT int vmaf_cuda_state_init(")
        self.assert_patterns(
            block,
            r"@param\[out\]\s+cu_state",
            r"caller\s+(?:owns|retains ownership)",
            r"vmaf_cuda_state_free.*after.*vmaf_close",
        )

    def test_cuda_free_documents_single_pointer_contract(self) -> None:
        block = _doxygen_before(self.cuda, "VMAF_EXPORT int vmaf_cuda_state_free(")
        self.assert_patterns(
            block,
            r"single[- ]pointer",
            r"does\s+not\s+(?:clear|null|zero)",
            r"vmaf_close.*before|after.*vmaf_close",
        )

    def test_cuda_import_documents_copy_and_call_order(self) -> None:
        block = _doxygen_before(self.cuda, "VMAF_EXPORT int vmaf_cuda_import_state(")
        self.assert_patterns(
            block,
            r"cop(?:y|ies|ied)\s+.*by\s+value",
            r"ownership.*not\s+transferred|caller.*retain",
            r"vmaf_cuda_state_free.*after.*vmaf_close",
            r"before.*vmaf_read_pictures",
        )

    def test_sycl_preallocation_enum_documents_stable_allocators(self) -> None:
        block = _doxygen_before(
            self.sycl,
            "enum VmafSyclPicturePreallocationMethod {",
        )
        self.assert_patterns(
            block,
            r"@enum\s+VmafSyclPicturePreallocationMethod",
            r"NONE.*vmaf_picture_alloc",
            r"DEVICE.*malloc_device",
            r"HOST.*malloc_host",
            r"stable.*append-only",
        )

    def test_sycl_preallocation_enum_values_are_explicit(self) -> None:
        self.assert_patterns(
            self.sycl,
            r"VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_NONE\s*=\s*0",
            r"VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE\s*=\s*1",
            r"VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_HOST\s*=\s*2",
        )


class PublicHeaderDoxygenContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[2]
        self.headers = [
            p for p in INCLUDE_DIR.glob("*.h")
            if "pelorus" not in p.parts
        ]
        self.doxyfile = self.repo_root / "core" / "doc" / "Doxyfile.public-api"
        self.workflow = self.repo_root / ".github" / "workflows" / "doxygen-public-api.yml"

    def test_no_field_tags_in_public_headers(self) -> None:
        """ADR-0953 / ADR-1315: @field is not a Doxygen command; use /**< ... */."""
        for header in self.headers:
            content = header.read_text(encoding="utf-8")
            matches = re.findall(r"@field\b", content)
            self.assertEqual(
                matches,
                [],
                f"{header.name} contains deprecated @field tags; use inline /**< ... */ instead",
            )

    def test_no_unknown_thread_command_in_public_headers(self) -> None:
        """ADR-1315: @thread-safety parses as @thread with -safety; use @note Thread safety:."""
        for header in self.headers:
            content = header.read_text(encoding="utf-8")
            matches = re.findall(r"@thread(?:-safety)?\b", content)
            self.assertEqual(
                matches,
                [],
                f"{header.name} contains invalid @thread command; use '@note Thread safety:' instead",
            )

    def test_no_unresolved_scope_links_in_public_headers(self) -> None:
        """Doxygen parses .py::IDENTIFIER as an unresolved C++ explicit link request."""
        for header in self.headers:
            content = header.read_text(encoding="utf-8")
            matches = re.findall(r"\.py::[A-Za-z0-9_]+", content)
            self.assertEqual(
                matches,
                [],
                f"{header.name} contains .py:: explicit link syntax that breaks Doxygen resolution",
            )

    def test_doxyfile_contract(self) -> None:
        """ADR-1315: Doxyfile.public-api must fail closed with WARN_AS_ERROR = YES and exclude pelorus mirror."""
        content = self.doxyfile.read_text(encoding="utf-8")
        self.assertRegex(content, r"WARN_AS_ERROR\s*=\s*YES")
        self.assertRegex(content, re.compile(r"EXCLUDE_PATTERNS\s*=.*?\*/pelorus/\*", re.DOTALL))

    def test_workflow_ceiling_zero(self) -> None:
        """ADR-1315: doxygen-public-api.yml warning ceiling must be 0."""
        content = self.workflow.read_text(encoding="utf-8")
        self.assertIn('DOXYGEN_WARNING_CEILING: "0"', content)

    def test_doxygen_execution_zero_warnings(self) -> None:
        """ADR-1315: running Doxygen against Doxyfile.public-api must produce zero warnings."""
        import shutil
        import subprocess
        import tempfile

        if not shutil.which("doxygen"):
            self.skipTest("doxygen binary not found on PATH")

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_doxyfile = Path(tmpdir) / "Doxyfile"
            warn_log = Path(tmpdir) / "warnings.log"
            content = self.doxyfile.read_text(encoding="utf-8")
            content += f"\nOUTPUT_DIRECTORY = {tmpdir}\n"
            content += f"WARN_LOGFILE = {warn_log}\n"
            tmp_doxyfile.write_text(content, encoding="utf-8")

            proc = subprocess.run(
                ["doxygen", str(tmp_doxyfile)],
                cwd=str(self.repo_root),
                capture_output=True,
                text=True,
            )
            self.assertEqual(proc.returncode, 0, f"doxygen failed: {proc.stderr}\n{proc.stdout}")
            if warn_log.exists():
                warnings = warn_log.read_text(encoding="utf-8").strip()
                self.assertEqual(warnings, "", f"doxygen produced warnings:\n{warnings}")


if __name__ == "__main__":
    unittest.main()
