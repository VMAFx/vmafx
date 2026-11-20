#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""The dev container is pushed only into a private GHCR package (ADR-1564).

The step under test is the workflow's own `run:` line, executed with a stub
`gh` that plays the package API, so a planted public answer fails exactly the
command the publish job runs.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, cast

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

_BASH = shutil.which("bash")
BASH = str(Path(_BASH).resolve(strict=True)) if _BASH is not None else "bash"
WORKFLOW = ROOT / ".github/workflows/dev-container-publish.yml"
GUARD_STEP = "Refuse to push unless the package is private"
PACKAGE = "ghcr.io/vidiko/vmafx-dev-mcp"
STUB_GH = """#!/usr/bin/env bash
printf '%s\\n' "$*" > "$STUB_DIR/gh-args"
if [[ -n ${STUB_ANSWER:-} ]]; then printf '%s' "$STUB_ANSWER"; fi
exit "${STUB_STATUS:-0}"
"""


def publish_steps() -> list[dict[str, Any]]:
    workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    return cast(list[dict[str, Any]], workflow["jobs"]["publish"]["steps"])


def guard_step() -> dict[str, Any]:
    matches = [step for step in publish_steps() if step.get("name") == GUARD_STEP]
    if len(matches) != 1:
        raise AssertionError(f"{WORKFLOW} has {len(matches)} steps named {GUARD_STEP!r}")
    return matches[0]


class PrivatePackageGuard(unittest.TestCase):
    def setUp(self) -> None:
        self.stub_dir = Path(tempfile.mkdtemp(prefix="ghcr-guard-"))
        self.addCleanup(shutil.rmtree, self.stub_dir)
        stub = self.stub_dir / "gh"
        stub.write_text(STUB_GH, encoding="utf-8")
        stub.chmod(0o755)

    def run_step(
        self, answer: str, status: int = 0, command: str | None = None
    ) -> TextCommandResult:
        """Run the guard step's `run:` command against the stub package API."""
        env = {
            "PATH": f"{self.stub_dir}{os.pathsep}{os.environ['PATH']}",
            "HOME": str(self.stub_dir),
            "GH_TOKEN": "stub-token",
            "STUB_DIR": str(self.stub_dir),
            "STUB_ANSWER": answer,
            "STUB_STATUS": str(status),
        }
        return run_command(
            [BASH, "-e", "-c", command or guard_step()["run"]],
            allowed_executables=(BASH,),
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=60,
        )

    def test_a_private_package_passes_and_the_endpoint_is_the_org_package(self) -> None:
        result = self.run_step(json.dumps({"name": "vmafx-dev-mcp", "visibility": "private"}))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"{PACKAGE} is private", result.stdout)
        args = (self.stub_dir / "gh-args").read_text(encoding="utf-8")
        self.assertIn("api", args.split())
        self.assertIn("/orgs/vidiko/packages/container/vmafx-dev-mcp", args)

    def test_a_planted_public_answer_fails_the_step(self) -> None:
        result = self.run_step(json.dumps({"name": "vmafx-dev-mcp", "visibility": "public"}))
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"::error::{PACKAGE} is public, not private", result.stderr)

    def test_any_other_visibility_fails_the_step(self) -> None:
        for visibility in ("internal", "Private", ""):
            with self.subTest(visibility=visibility):
                result = self.run_step(json.dumps({"visibility": visibility}))
                self.assertEqual(result.returncode, 1, result.stdout)

    def test_an_unreadable_package_fails_closed(self) -> None:
        """404 (no package, or the token may not read it) and 403 make gh exit 1."""
        result = self.run_step('{"message": "Not Found", "status": "404"}', status=1)
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot read the visibility", result.stderr)

    def test_an_answer_without_a_visibility_fails_closed(self) -> None:
        for answer in ("{}", '{"visibility": null}', '{"visibility": 0}', "not json", ""):
            with self.subTest(answer=answer):
                result = self.run_step(answer)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("carries no visibility", result.stderr)

    def test_wrong_usage_exits_2(self) -> None:
        result = self.run_step("", command="bash scripts/ci/require-private-ghcr-package.sh vidiko")
        self.assertEqual(result.returncode, 2)


class WorkflowWiring(unittest.TestCase):
    def test_the_guard_runs_before_anything_is_built_or_pushed(self) -> None:
        steps = publish_steps()
        names = [step.get("name") for step in steps]
        guard = names.index(GUARD_STEP)
        build = next(
            i for i, step in enumerate(steps) if "build-push-action" in step.get("uses", "")
        )
        login = next(i for i, step in enumerate(steps) if "login-action" in step.get("uses", ""))
        self.assertLess(guard, min(build, login))

    def test_the_guard_cannot_be_skipped_or_ignored(self) -> None:
        step = guard_step()
        self.assertNotIn("if", step)
        self.assertNotIn("continue-on-error", step)
        self.assertEqual(step["env"]["GH_TOKEN"], "${{ github.token }}")
        self.assertEqual(
            step["run"], "bash scripts/ci/require-private-ghcr-package.sh vidiko vmafx-dev-mcp"
        )

    def test_every_pushed_tag_is_in_the_guarded_package(self) -> None:
        build = next(s for s in publish_steps() if "build-push-action" in s.get("uses", ""))
        tags = [tag.strip() for tag in build["with"]["tags"].splitlines() if tag.strip()]
        self.assertTrue(tags)
        for tag in tags:
            self.assertTrue(tag.startswith(f"{PACKAGE}:"), tag)


if __name__ == "__main__":
    unittest.main()
