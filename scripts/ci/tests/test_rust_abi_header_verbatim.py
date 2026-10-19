#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The cbindgen headers under core/src/rust/include/ stay byte-identical to cbindgen.

scripts/dev/rust-abi-header.sh --check (the Rust workflow, ADR-1713) compares
the committed headers with a fresh cbindgen 0.29.4 run. A formatter that
rewrites them makes that check fail on every head, so neither the clang-format
hooks nor `make format` may select them. With cbindgen 0.29.4 in PATH the
check itself runs too.
"""

from __future__ import annotations

import re
import shutil
import sys
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[3]
HEADERS = ("core/src/rust/include/vmafx_rs.h", "core/src/rust/include/vmafx_rs_predict.h")


def clang_format_hooks() -> list[dict[str, object]]:
    config = yaml.safe_load((REPO_ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8"))
    hooks: list[dict[str, object]] = []
    for repo in config["repos"]:
        hooks.extend(h for h in repo.get("hooks", []) if h.get("id") == "clang-format")
    return hooks


def hook_selects(hook: dict[str, object], path: str) -> bool:
    exclude = str(hook.get("exclude", "^$"))
    files = str(hook.get("files", ""))
    if re.search(exclude, path):
        return False
    if files and not re.search(files, path):
        return False
    return bool(files) or path.endswith((".c", ".h", ".cc", ".cpp", ".hpp", ".cu", ".cuh"))


class RustAbiHeaderVerbatimTest(unittest.TestCase):
    def test_clang_format_hooks_skip_the_generated_headers(self) -> None:
        hooks = clang_format_hooks()
        self.assertGreaterEqual(len(hooks), 2)
        for header in HEADERS:
            self.assertTrue((REPO_ROOT / header).is_file(), header)
            for hook in hooks:
                self.assertFalse(
                    hook_selects(hook, header), f"{hook.get('name', 'clang-format')}: {header}"
                )
        # A hand-written header next to them is still formatted.
        self.assertTrue(hook_selects(hooks[0], "core/src/rust/shim/rust_twins.h"))

    def test_make_format_skips_the_generated_headers(self) -> None:
        makefile = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
        block = makefile[makefile.index("CLANG_FORMAT_FILES =") :]
        block = block[: block.index("\n\n")]
        self.assertIn("grep -v '^core/src/rust/include/'", block)

    def test_headers_match_cbindgen(self) -> None:
        cbindgen = shutil.which("cbindgen")
        if cbindgen is None:
            self.skipTest(
                "cbindgen not in PATH (the Rust workflow installs 0.29.4 and runs the check)"
            )
        version = run_command(
            [cbindgen, "--version"],
            allowed_executables=(cbindgen,),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=60,
        )
        if "0.29.4" not in version.stdout:
            self.skipTest(f"cbindgen {version.stdout.strip()} is not the pinned 0.29.4")
        bash = shutil.which("bash") or "/bin/bash"
        result = run_command(
            [bash, str(REPO_ROOT / "scripts" / "dev" / "rust-abi-header.sh"), "--check"],
            allowed_executables=(bash,),
            capture_output=True,
            text=True,
            check=False,
            cwd=REPO_ROOT,
            timeout_seconds=600,
        )
        self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-2000:])


if __name__ == "__main__":
    unittest.main()
