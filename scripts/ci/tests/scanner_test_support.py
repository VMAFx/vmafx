#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Environment isolation for scanner checks run inside fixture repositories."""

from __future__ import annotations

import os
import shlex
import shutil
import sys
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import yaml  # type: ignore[import-untyped]  # hook environments omit PyYAML stubs

from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]


def isolate_pre_commit_environment(test_case: unittest.TestCase) -> None:
    """Remove caller hook variables until the test's registered cleanups run."""
    test_case.enterContext(mock.patch.dict(os.environ))
    for key in tuple(os.environ):
        if key.startswith("PRE_COMMIT_"):
            del os.environ[key]


def scanner_hooks(scanner: str) -> tuple[dict[str, Any], dict[str, Any]]:
    """Read the actual commit and push hook definitions used by pre-commit."""
    config = yaml.safe_load((ROOT / ".pre-commit-config.yaml").read_text())
    hooks = {hook["id"]: hook for repo in config["repos"] for hook in repo["hooks"]}
    return hooks[f"{scanner}-new"], hooks[f"{scanner}-new-push"]


def assert_push_contract(test: unittest.TestCase, scanner: str) -> None:
    """Check the split stages and unconditional filename-free outgoing invocation."""
    commit, push = scanner_hooks(scanner)
    test.assertEqual(commit["stages"], ["pre-commit"])
    test.assertEqual(push["stages"], ["pre-push"])
    test.assertIs(push["pass_filenames"], False)
    test.assertIs(push["always_run"], True)
    test.assertEqual(shlex.split(push["entry"]), [*shlex.split(commit["entry"]), "--outgoing"])
    test.assertEqual(push["language"], commit["language"])
    test.assertEqual(push["additional_dependencies"], commit["additional_dependencies"])
    test.assertEqual(push.get("require_serial"), commit.get("require_serial"))
    test.assertNotIn("types_or", push)
    test.assertNotIn("types", push)
    test.assertNotIn("files", push)


def run_push_entry(
    root: Path,
    scanner: str,
    *,
    from_ref: str | None = None,
    to_ref: str | None = None,
    extra: tuple[str, ...] = (),
) -> tuple[int, str, str]:
    """Run the configured entry with no filenames, as an installed push hook does."""
    _, hook = scanner_hooks(scanner)
    paths = (
        "scripts/__init__.py",
        "scripts/lib/safe_subprocess.py",
        "scripts/lib/new_scanner_findings.py",
        f"scripts/ci/check_{scanner}_new.py",
    )
    for relative in paths:
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / relative, target)
    env = dict(os.environ)
    if to_ref is not None:
        env.update(PRE_COMMIT_FROM_REF=from_ref or "master", PRE_COMMIT_TO_REF=to_ref)
        env["PRE_COMMIT_REMOTE_BRANCH"] = "refs/heads/change"
    # Resolve the entry unchanged inside the test interpreter's environment.
    env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env.get("PATH", os.defpath)
    argv = shlex.split(hook["entry"])
    proc = run_command(
        [*argv, *extra],
        allowed_executables=("python3",),
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        timeout_seconds=120,
    )
    return proc.returncode, proc.stdout, proc.stderr
