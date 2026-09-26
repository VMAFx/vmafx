#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the real required-check script with Go outcomes and routing guards."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.required_aggregator_harness import run_required_aggregator

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
GO_CHECK = "go vet + go test"
LIBVMAF_GO = ROOT / "pkg" / "libvmaf" / "libvmaf.go"
MAKEFILE = ROOT / "Makefile"
SERVER_DOCKERFILE = ROOT / "Dockerfile.go-server"
NODE_DOCKERFILE = ROOT / "docker" / "Dockerfile.node"
CONTROLLER_DOCKERFILE = ROOT / "docker" / "Dockerfile.controller"
DEV_CONTAINERFILE = ROOT / "dev" / "Containerfile"


class GoWorkflowContract(unittest.TestCase):
    def test_cgo_linking_selects_the_fork_explicitly(self) -> None:
        wrapper = LIBVMAF_GO.read_text(encoding="utf-8")
        workflow = (WORKFLOWS / "go-ci.yml").read_text(encoding="utf-8")
        makefile = MAKEFILE.read_text(encoding="utf-8")

        self.assertNotRegex(wrapper, r"(?m)^#cgo\s+LDFLAGS:")
        self.assertIn(
            "CGO_LDFLAGS: -L${{ github.workspace }}/core/build-cpu/src -lvmaf -lm",
            workflow,
        )
        self.assertIn(
            'CGO_LDFLAGS="-L$(CURDIR)/core/build-cpu/src -lvmaf -lm"',
            makefile,
        )

        for container in (SERVER_DOCKERFILE, NODE_DOCKERFILE, DEV_CONTAINERFILE):
            with self.subTest(container=container.name):
                source = container.read_text(encoding="utf-8")
                self.assertIn('CGO_LDFLAGS="-L/usr/local/lib -lvmaf -lm"', source)

        controller = CONTROLLER_DOCKERFILE.read_text(encoding="utf-8")
        self.assertIn(
            'CGO_LDFLAGS="-L/usr/lib/x86_64-linux-gnu -lvmaf -lm"',
            controller,
        )

    def test_ready_pr_and_master_runs_are_routed_inside_the_job(self) -> None:
        workflow = (WORKFLOWS / "go-ci.yml").read_text(encoding="utf-8")
        trigger = workflow.split("\nconcurrency:", 1)[0]
        self.assertNotRegex(trigger, r"(?m)^\s+paths(?:-ignore)?:")
        self.assertIn("types: [opened, synchronize, reopened, ready_for_review]", trigger)
        self.assertIn("branches: [master]", trigger)
        self.assertIn("github.event.pull_request.draft == false", workflow)
        self.assertIn("fetch-depth: 0", workflow)
        self.assertIn("name: " + GO_CHECK, workflow)
        self.assertIn("# required-aggregator", workflow)
        self.assertIn('--base "$BASE_SHA" --head "$HEAD_SHA"', workflow)
        steps = workflow.split("\n      - ")[1:]
        heavy = [
            step
            for step in steps
            if not step.startswith(
                ("uses: actions/checkout@", "name: Plan CI", "name: Go checks not impacted")
            )
        ]
        self.assertGreater(len(heavy), 8)
        for step in heavy:
            self.assertIn("if: steps.impact.outputs.go_checks == 'true'", step)
            self.assertNotIn("continue-on-error", step)
        self.assertIn("if: steps.impact.outputs.go_checks != 'true'", workflow)

    def test_go_fix_modernization_gate_is_pinned(self) -> None:
        workflow = (WORKFLOWS / "go-ci.yml").read_text(encoding="utf-8")
        makefile = MAKEFILE.read_text(encoding="utf-8")

        # 1. Exact command plus fail-closed execution.  Match the named step and
        # its complete run line so `echo ...` and `... || true` cannot satisfy
        # this contract.
        steps = workflow.split("\n      - ")[1:]
        fix_steps = [s for s in steps if s.splitlines()[0] == "name: go fix (diff)"]
        self.assertEqual(len(fix_steps), 1, "expected exactly one go fix -diff step")
        fix_step = fix_steps[0]
        self.assertIn("        run: go fix -diff ./...", fix_step.splitlines())

        # 2. Impact condition and no error suppression.
        self.assertIn("if: steps.impact.outputs.go_checks == 'true'", fix_step)
        self.assertNotIn("continue-on-error", fix_step)

        # 3. Ordering: after setup-go, before native dependency/build work, before vet/test
        setup_idx = workflow.index("uses: actions/setup-go@")
        fix_idx = workflow.index("go fix -diff ./...")
        native_idx = workflow.index("Install meson + ninja for libvmaf build")
        vet_idx = workflow.index("go vet ./...")
        test_idx = workflow.index("go test ./...")

        self.assertLess(setup_idx, fix_idx, "go fix must run after setup-go")
        self.assertLess(fix_idx, native_idx, "go fix must run early, before native libvmaf build")
        self.assertLess(native_idx, vet_idx, "native build must precede go vet")
        self.assertLess(vet_idx, test_idx, "go vet must precede go test")

        # 4. Make targets and exact commands
        self.assertRegex(makefile, r"(?m)^\s*go-fix:")
        self.assertRegex(makefile, r"(?m)^\s*go-fix-check:")
        fix_recipe = makefile.split("\ngo-fix:\n", 1)[1].split("\n\n", 1)[0]
        check_recipe = makefile.split("\ngo-fix-check:\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("\tgo fix ./...", fix_recipe.splitlines())
        self.assertIn("\tgo fix -diff ./...", check_recipe.splitlines())
        self.assertNotIn("CGO_LDFLAGS", fix_recipe)
        self.assertNotIn("CGO_LDFLAGS", check_recipe)
        phony_section = makefile.split(".PHONY:", 1)[1].split("\n\n", 1)[0]
        phony_targets = set(phony_section.replace("\\", " ").split())
        self.assertIn("go-fix", phony_targets)
        self.assertIn("go-fix-check", phony_targets)

    def test_failed_go_scan_blocks_otherwise_green_checks(self) -> None:
        failures = run_required_aggregator(GO_CHECK, "failure")
        self.assertEqual(len(failures), 1)
        self.assertIn(GO_CHECK + ": failure", failures[0])

    def test_successful_go_checks_pass(self) -> None:
        self.assertEqual(run_required_aggregator(GO_CHECK, "success"), [])

    def test_unreported_check_keeps_existing_absence_semantics(self) -> None:
        self.assertEqual(run_required_aggregator(GO_CHECK, None), [])


if __name__ == "__main__":
    unittest.main()
