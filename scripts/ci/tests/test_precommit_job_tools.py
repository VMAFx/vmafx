#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pre-Commit CI job installs every locked tool a system hook looks up.

A ``language: system`` hook runs with whatever the job put on PATH. When its
script calls ``shutil.which("<tool>")`` for a tool that a requirements lock
pins, the Pre-Commit job in ``lint-and-format.yml`` must install a lock whose
``.in`` pins that tool; otherwise the hook stops with "<tool> is not on PATH"
in CI only (``check-fork-python-subprocess`` and ruff, pull request #3237).

Cases: the real config and workflow (positive), a planted hook whose tool the
job never installs (negative), and a tool the runner image provides that no
lock pins (boundary: not this check's business).
"""

from __future__ import annotations

import re
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import suite_registry  # noqa: E402

CONFIG = ROOT / ".pre-commit-config.yaml"
WORKFLOW = ROOT / ".github" / "workflows" / "lint-and-format.yml"
LOCKS = ROOT / "requirements" / "locks"
# A fail-closed lookup: ``x = shutil.which("tool")`` then ``if x is None``.
WHICH = re.compile(
    r"""(\w+)\s*=\s*shutil\.which\(\s*["']([A-Za-z0-9_.-]+)["']\s*\)\s*\n\s*if\s+\1\s+is\s+None"""
)
SCRIPT = re.compile(r"(scripts/[\w./-]+\.py)")
LOCK_TXT = re.compile(r"requirements/locks/([\w.-]+)\.txt")
PIN = re.compile(r"^([A-Za-z0-9_.-]+)(?:\[[^\]]*\])?==", re.MULTILINE)


def locked_tools(locks: Path) -> dict[str, set[str]]:
    """Map each lock name to the packages its ``.in`` pins."""
    return {
        path.stem: {m.lower() for m in PIN.findall(path.read_text(encoding="utf-8"))}
        for path in sorted(locks.glob("*.in"))
    }


def job_locks(workflow: dict[str, Any]) -> set[str]:
    """Lock names the Pre-Commit job installs."""
    job = workflow["jobs"]["pre-commit"]
    runs = " ".join(str(step.get("run", "")) for step in job["steps"])
    return set(LOCK_TXT.findall(runs))


def ci_skip(workflow: dict[str, Any], registry_skip: str) -> set[str]:
    """Hook ids the Pre-Commit job skips: its static SKIP plus the registry's."""
    steps = workflow["jobs"]["pre-commit"]["steps"]
    raw = " ".join(str(step.get("env", {}).get("SKIP", "")) for step in steps)
    ids = re.sub(r"\$\{\{[^}]*\}\}", "", raw).split(",") + registry_skip.split(",")
    return {i.strip() for i in ids if i.strip()}


def runs_in_ci(
    hook: dict[str, Any], config: dict[str, Any], skip: frozenset[str] | set[str]
) -> bool:
    """A system hook the Pre-Commit job runs (pre-commit stage, not skipped)."""
    stages = hook.get("stages") or config.get("default_stages") or ["pre-commit"]
    return hook.get("language") == "system" and "pre-commit" in stages and hook["id"] not in skip


def system_hook_tools(
    config: dict[str, Any], root: Path, skip: frozenset[str] | set[str] = frozenset()
) -> list[tuple[str, str]]:
    """(hook id, tool) for each tool a CI-run system hook needs on PATH."""
    found: list[tuple[str, str]] = []
    for repo in config.get("repos", []):
        for hook in repo.get("hooks", []):
            if not runs_in_ci(hook, config, skip):
                continue
            for script in SCRIPT.findall(str(hook.get("entry", ""))):
                path = root / script
                if path.is_file():
                    text = path.read_text(encoding="utf-8")
                    found += [(hook["id"], tool) for _, tool in WHICH.findall(text)]
    return found


def findings(
    config: dict[str, Any],
    workflow: dict[str, Any],
    root: Path,
    locks: Path,
    registry_skip: str = "",
) -> list[str]:
    """Hooks whose locked tool the Pre-Commit job does not install."""
    pinned = locked_tools(locks)
    any_lock = set().union(*pinned.values())
    installed = set().union(*(pinned.get(name, set()) for name in job_locks(workflow)))
    return [
        f"{hook}: Pre-Commit job installs no lock that pins {tool}"
        for hook, tool in system_hook_tools(config, root, ci_skip(workflow, registry_skip))
        if tool.lower() in any_lock and tool.lower() not in installed
    ]


def _load(path: Path) -> dict[str, Any]:
    loaded = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(loaded, dict):
        raise TypeError(f"{path}: top level is not a mapping")
    return loaded


class PrecommitJobTools(unittest.TestCase):
    def test_real_config_installs_every_locked_tool(self) -> None:
        registry = ",".join(suite_registry.precommit_test_hooks(ROOT))
        self.assertEqual(findings(_load(CONFIG), _load(WORKFLOW), ROOT, LOCKS, registry), [])

    def _planted(self, tmp: Path, tool: str) -> tuple[dict[str, Any], dict[str, Any]]:
        (tmp / "scripts").mkdir()
        (tmp / "scripts" / "hook.py").write_text(
            f'import shutil\nt = shutil.which("{tool}")\nif t is None:\n    raise SystemExit(2)\n',
            encoding="utf-8",
        )
        (tmp / "locks").mkdir()
        (tmp / "locks" / "linters.in").write_text("ruff==1.0\n", encoding="utf-8")
        (tmp / "locks" / "base.in").write_text("pre-commit==4.0\n", encoding="utf-8")
        config = {
            "repos": [
                {
                    "repo": "local",
                    "hooks": [
                        {
                            "id": "planted",
                            "language": "system",
                            "entry": "python3 -B scripts/hook.py",
                        }
                    ],
                }
            ]
        }
        run = "python -m pip install --require-hashes -r requirements/locks/base.txt"
        workflow = {"jobs": {"pre-commit": {"steps": [{"run": run}]}}}
        return config, workflow

    def test_planted_missing_tool_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            config, workflow = self._planted(tmp, "ruff")
            self.assertEqual(
                findings(config, workflow, tmp, tmp / "locks"),
                ["planted: Pre-Commit job installs no lock that pins ruff"],
            )
            workflow["jobs"]["pre-commit"]["steps"].append(
                {"run": "python -m pip install -r requirements/locks/linters.txt"}
            )
            self.assertEqual(findings(config, workflow, tmp, tmp / "locks"), [])

    def test_runner_tool_without_lock_is_out_of_scope(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            config, workflow = self._planted(tmp, "shellcheck")
            self.assertEqual(findings(config, workflow, tmp, tmp / "locks"), [])

    def test_skipped_or_pre_push_hook_is_out_of_scope(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            config, workflow = self._planted(tmp, "ruff")
            self.assertEqual(findings(config, workflow, tmp, tmp / "locks", "planted"), [])
            config["repos"][0]["hooks"][0]["stages"] = ["pre-push"]
            self.assertEqual(findings(config, workflow, tmp, tmp / "locks"), [])

    def test_fallback_lookup_is_out_of_scope(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            config, workflow = self._planted(tmp, "ruff")
            (tmp / "scripts" / "hook.py").write_text(
                'import shutil\nt = shutil.which("ruff")\nif t:\n    print(t)\n',
                encoding="utf-8",
            )
            self.assertEqual(findings(config, workflow, tmp, tmp / "locks"), [])


if __name__ == "__main__":
    unittest.main()
