#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Contract tests for .github/workflows/rust-ci.yml trigger paths.

Verifies that rust-ci.yml triggers on changes to public libvmaf C headers
under core/include/libvmaf/**, which are bound by bindings/rust/vmafx-sys
via bindgen.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

REPO_ROOT = Path(__file__).resolve().parents[3]
RUST_CI_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "rust-ci.yml"
CONFIG = REPO_ROOT / ".github" / "ci-impact.json"


class RustCIWorkflowContractTest(unittest.TestCase):
    """Ensure rust-ci workflow path filters cover public C library headers."""

    def test_rust_ci_paths_include_libvmaf_headers(self) -> None:
        self.assertTrue(RUST_CI_WORKFLOW.exists(), f"missing {RUST_CI_WORKFLOW}")
        raw = RUST_CI_WORKFLOW.read_text(encoding="utf-8")
        parsed = yaml.safe_load(raw)

        on = parsed.get("on") or parsed.get(True) or {}
        push_paths = on.get("push", {}).get("paths", [])
        pr_paths = on.get("pull_request", {}).get("paths", [])

        expected_pattern = "core/include/libvmaf/**"
        self.assertIn(
            expected_pattern,
            push_paths,
            f"{expected_pattern} missing from on.push.paths in {RUST_CI_WORKFLOW.name}",
        )
        self.assertIn(
            expected_pattern,
            pr_paths,
            f"{expected_pattern} missing from on.pull_request.paths in {RUST_CI_WORKFLOW.name}",
        )

    def test_ci_impact_json_rust_selector_includes_libvmaf_headers(self) -> None:
        self.assertTrue(CONFIG.exists(), f"missing {CONFIG}")
        config = json.loads(CONFIG.read_text(encoding="utf-8"))
        rust_patterns = config.get("selectors", {}).get("rust", {}).get("patterns", [])
        self.assertIn(
            "core/include/libvmaf/**",
            rust_patterns,
            "core/include/libvmaf/** missing from selectors.rust.patterns in ci-impact.json",
        )


if __name__ == "__main__":
    unittest.main()
