#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check the composite actions under .github/actions/.

actionlint reads workflow files only; it rejects an ``action.yml`` as a workflow with
"jobs section is missing". This gate gives the composite actions the two checks a
workflow gets:

1. Structure. The manifest is a mapping with ``name``, ``description`` and a ``runs``
   mapping whose ``using`` is ``composite`` and whose ``steps`` is a non-empty list; every
   step is either a ``run`` step with a ``shell`` or a ``uses`` step, never both and never
   neither; every input has a ``description``. The full GitHub schema is checked by the
   ``check-github-actions`` hook of check-jsonschema.
2. Shell. Every ``run:`` block whose shell is ``bash`` or ``sh`` goes through shellcheck,
   with each ``${{ ... }}`` expression replaced by a placeholder word. A step in another
   shell (``pwsh``, ``cmd``, ``python``) is reported as skipped with the reason: no
   checker for it is part of the toolchain.
3. Expressions. A ``run:`` block holds no ``${{ ... }}`` expression except the runner
   values in ``RUN_EXPRESSION_ALLOW``. GitHub expands an expression into the script text
   before the shell parses it, so an input or a step output there is code injection
   (zizmor ``template-injection``); the step passes it through ``env:`` and quotes the
   variable instead. This holds for every shell, not only the ones shellcheck reads.

Exit 0 when every action passes, 1 on a finding, 2 when shellcheck is missing.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs in the hook env, as in scripts/ci/check-helm-selector-isolation.py

ROOT = Path(__file__).resolve().parents[2]
EXPRESSION = re.compile(r"\$\{\{.*?\}\}", re.DOTALL)
PLACEHOLDER = "__GHA_EXPRESSION__"
SHELLCHECK_SHELLS = {"bash": "bash", "sh": "sh"}
SHELLCHECK_TIMEOUT_S = 60
# The shellcheck arguments actionlint v1.7.12 uses for a workflow `run:` block
# (rule_shellcheck.go: `--norc -x -e SC1091,SC2194,SC2050,SC2153,SC2154,SC2157,SC2043`), so a
# composite action is held to the standard a workflow already is: a block sees the step's
# environment and the runner's files, which shellcheck cannot, and a `${{ }}` placeholder
# makes some words constant.
SHELLCHECK_EXCLUDE = "SC1091,SC2194,SC2050,SC2153,SC2154,SC2157,SC2043"
# Expressions a run block may hold: values the runner sets, which carry no caller text.
# An allow-list (fleet rule for readers that mirror another parser), never a list of
# unsafe contexts.
RUN_EXPRESSION_ALLOW = frozenset(
    {
        "github.action_path",
        "github.run_attempt",
        "github.run_id",
        "github.run_number",
        "github.sha",
        "github.workspace",
        "runner.arch",
        "runner.os",
        "runner.temp",
    }
)
MAX_ACTIONS = 256
MAX_STEPS = 1024


def find_actions(root: Path) -> list[Path]:
    """Return every composite-action manifest below ``root``/.github/actions."""
    base = root / ".github" / "actions"
    found = sorted(p for p in base.glob("*/action.y*ml") if p.is_file())
    return found[:MAX_ACTIONS]


def _check_header(manifest: object) -> list[str]:
    """Findings for the top-level keys of one manifest."""
    if not isinstance(manifest, dict):
        return ["manifest is not a mapping"]
    findings = [
        f"missing top-level key {key!r}" for key in ("name", "description") if not manifest.get(key)
    ]
    inputs = manifest.get("inputs") or {}
    if not isinstance(inputs, dict):
        return [*findings, "`inputs` is not a mapping"]
    for key, spec in inputs.items():
        if not isinstance(spec, dict) or not spec.get("description"):
            findings.append(f"input {key!r} has no description")
    return findings


def _check_step(index: int, step: object) -> list[str]:
    """Findings for one step of a composite action."""
    where = f"step {index}"
    if not isinstance(step, dict):
        return [f"{where} is not a mapping"]
    has_run, has_uses = "run" in step, "uses" in step
    if has_run == has_uses:
        return [f"{where} must have exactly one of `run` and `uses`"]
    if has_run and not step.get("shell"):
        return [f"{where} has `run` without `shell` (a composite action requires it)"]
    return []


def check_structure(manifest: object) -> list[str]:
    """Return the structural findings of one parsed manifest."""
    findings = _check_header(manifest)
    if not isinstance(manifest, dict):
        return findings
    runs = manifest.get("runs")
    if not isinstance(runs, dict) or runs.get("using") != "composite":
        return [*findings, "`runs.using` is not `composite`"]
    steps = runs.get("steps")
    if not isinstance(steps, list) or not steps:
        return [*findings, "`runs.steps` is empty or not a list"]
    for index, step in enumerate(steps[:MAX_STEPS], start=1):
        findings.extend(_check_step(index, step))
    return findings


def shellcheck_block(shell: str, script: str) -> str:
    """Run shellcheck over one block; return its report, empty when clean."""
    executable = shutil.which("shellcheck")
    if executable is None:
        return "shellcheck not found on PATH"
    cleaned = EXPRESSION.sub(PLACEHOLDER, script)
    result = subprocess.run(  # noqa: S603 -- fixed argv, script on stdin
        [
            executable,
            "--norc",
            "-x",
            f"--shell={SHELLCHECK_SHELLS[shell]}",
            f"--exclude={SHELLCHECK_EXCLUDE}",
            "-",
        ],
        input=cleaned,
        capture_output=True,
        text=True,
        check=False,
        timeout=SHELLCHECK_TIMEOUT_S,
    )
    return result.stdout.strip() if result.returncode else ""


def check_shell(manifest: dict[str, Any], label: str, notes: list[str]) -> list[str]:
    """shellcheck every bash and sh ``run`` block; record skips in ``notes``."""
    findings: list[str] = []
    steps = (manifest.get("runs") or {}).get("steps") or []
    for index, step in enumerate(steps[:MAX_STEPS], start=1):
        if not isinstance(step, dict) or "run" not in step:
            continue
        shell = str(step.get("shell", "")).split()[0] if step.get("shell") else ""
        if shell not in SHELLCHECK_SHELLS:
            notes.append(
                f"{label}: step {index} shell={shell or '?'} skipped: no checker for this shell"
            )
            continue
        report = shellcheck_block(shell, str(step["run"]))
        if report:
            findings.append(f"{label}: step {index} ({step.get('name', 'unnamed')}):\n{report}")
    return findings


def check_run_expressions(manifest: dict[str, Any], label: str) -> list[str]:
    """Findings for every expression outside the allow-list inside a ``run`` block."""
    findings: list[str] = []
    steps = (manifest.get("runs") or {}).get("steps") or []
    for index, step in enumerate(steps[:MAX_STEPS], start=1):
        if not isinstance(step, dict) or "run" not in step:
            continue
        for match in EXPRESSION.finditer(str(step["run"])):
            inner = match.group(0)[3:-2].strip()
            if inner not in RUN_EXPRESSION_ALLOW:
                findings.append(
                    f"{label}: step {index} ({step.get('name', 'unnamed')}): "
                    f"{match.group(0)} in `run`; pass it through `env:` and quote the variable"
                )
    return findings


def check_action(path: Path, root: Path, notes: list[str]) -> list[str]:
    """All findings for one action manifest."""
    label = str(path.relative_to(root))
    try:
        manifest = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        return [f"{label}: invalid YAML: {exc}"]
    findings = [f"{label}: {item}" for item in check_structure(manifest)]
    if not findings:
        findings = check_run_expressions(manifest, label) + check_shell(manifest, label, notes)
    return findings


def main(argv: list[str]) -> int:
    """Check every composite action under the repository root (argv[1], default: this checkout)."""
    root = Path(argv[1]).resolve() if len(argv) > 1 else ROOT
    if shutil.which("shellcheck") is None:
        print("check_composite_actions: shellcheck not found on PATH", file=sys.stderr)
        return 2
    actions = find_actions(root)
    if not actions:
        print(
            "check_composite_actions: no composite action found under .github/actions",
            file=sys.stderr,
        )
        return 1
    notes: list[str] = []
    findings: list[str] = []
    for path in actions:
        findings.extend(check_action(path, root, notes))
    for note in notes:
        print(f"note: {note}")
    for finding in findings:
        print(f"error: {finding}", file=sys.stderr)
    print(f"check_composite_actions: {len(actions)} action(s), {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
