#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise Git's real hooks with local repos and the installed framework."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from pre_commit.clientlib import load_config

ROOT = Path(__file__).resolve().parents[3]
CONFIG = """repos:
  - repo: local
    hooks:
      - id: commit-probe
        name: commit-probe
        entry: python3 probe.py pre-commit
        language: system
        stages: [pre-commit]
        always_run: true
        pass_filenames: false
      - id: message-probe
        name: message-probe
        entry: python3 probe.py commit-msg
        language: system
        stages: [commit-msg]
      - id: push-probe
        name: push-probe
        entry: python3 probe.py pre-push
        language: system
        stages: [pre-push]
        always_run: true
        pass_filenames: false
      - id: validate-pr-body
        name: validate-pr-body
        entry: scripts/git-hooks/pre-push-pr-body-lint.sh
        language: script
        stages: [pre-push]
        always_run: true
        pass_filenames: false
      - id: mkdocs-strict
        name: mkdocs-strict
        entry: scripts/git-hooks/pre-push-mkdocs-strict.sh
        language: script
        stages: [pre-push]
        files: '^(docs/|mkdocs\\.yml)'
        pass_filenames: false
"""
PROBE = """import json, os, pathlib, sys
with open(os.environ['HOOK_TEST_LOG'], 'a') as log:
    log.write(json.dumps([sys.argv[1], os.getcwd(), os.environ.get('PRE_COMMIT_LOCAL_BRANCH')]) + '\\n')
if sys.argv[1] == 'commit-msg' and pathlib.Path(sys.argv[2]).read_text().startswith('bad'):
    sys.exit(1)
if sys.argv[1] == os.environ.get('HOOK_TEST_FAIL'):
    sys.exit(1)
"""


class HookInstallTests(unittest.TestCase):
    def test_repository_config_and_required_ci_wiring(self) -> None:
        config = load_config(str(ROOT / ".pre-commit-config.yaml"))
        self.assertEqual(
            config["default_install_hook_types"], ["pre-commit", "commit-msg", "pre-push"]
        )
        push_hooks = {
            hook["id"]: hook
            for repo in config["repos"]
            for hook in repo["hooks"]
            if hook.get("stages") == ["pre-push"]
        }
        self.assertTrue(
            {
                "assertion-density",
                "twin-drift-check",
                "mypy-local",
                "ffmpeg-patches-apply-check",
                "validate-pr-body",
                "mkdocs-strict",
            }
            <= push_hooks.keys()
        )
        self.assertEqual(
            push_hooks["mkdocs-strict"]["entry"],
            "scripts/git-hooks/pre-push-mkdocs-strict.sh",
        )
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text()
        precommit_job = workflow.split("  pre-commit:\n", 1)[1].split("  clang-tidy:\n", 1)[0]
        self.assertIn("# required-aggregator", precommit_job)
        self.assertIn("run: python3 scripts/githooks/tests/test_install.py", precommit_job)

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-hook-test-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.repo = self.base / "repository with spaces"
        self.repo.mkdir()
        self.bin = self.base / "bin"
        self.bin.mkdir()
        self.log = self.base / "events.jsonl"
        # Isolate Git/pre-commit config and caches from the user's checkout.
        self.env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("GIT_", "PRE_COMMIT_"))
        }
        self.env.update(
            GIT_CONFIG_NOSYSTEM="1",
            GIT_CONFIG_GLOBAL=os.devnull,
            PRE_COMMIT_HOME=str(self.base / "cache"),
            HOOK_TEST_LOG=str(self.log),
            PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}",
        )
        self.env.pop("SKIP", None)
        self.env.pop("VMAFX_NATIVE_HOOKS", None)
        self.run_git("init", "-b", "master")
        self.run_git("config", "user.name", "Hook Test")
        self.run_git("config", "user.email", "hook-test@example.invalid")
        for directory in ("githooks", "git-hooks"):
            shutil.copytree(ROOT / "scripts" / directory, self.repo / "scripts" / directory)
        self.write(".pre-commit-config.yaml", CONFIG)
        self.write("probe.py", PROBE)
        self.write("docs/index.md", "# Docs\n")
        self.write("mkdocs.yml", "site_name: fixture\n")
        self.write("scripts/ci/validate-pr-body.sh", "#!/bin/sh\nexit 0\n", executable=True)
        self.write(".gitignore", ".claude/\n")
        self.run_git("add", ".")
        self.run_git("commit", "-m", "test: fixture")
        self.remote = self.base / "remote.git"
        self.run_git("init", "--bare", str(self.remote))
        self.run_git("remote", "add", "origin", str(self.remote))
        self.write_bin(
            "gh",
            '#!/bin/sh\nif [ "${HOOK_TEST_DRAFT:-0}" = 1 ]; then\n  echo \'{"state":"OPEN","isDraft":true,"body":"draft"}\'\nelse\n  exit 1\nfi\n',
        )
        self.write_bin(
            "mkdocs",
            '#!/bin/sh\nprintf \'["mkdocs"]\\n\' >> "$HOOK_TEST_LOG"\nexit "${HOOK_TEST_MKDOCS_EXIT:-0}"\n',
        )

    def write(self, path: str, text: str, *, executable: bool = False) -> None:
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        if executable:
            target.chmod(0o755)

    def write_bin(self, name: str, text: str) -> None:
        target = self.bin / name
        target.write_text(text)
        target.chmod(0o755)

    def run_command(
        self, *args: str, cwd: Path | None = None, check: bool = True
    ) -> subprocess.CompletedProcess[str]:
        result = (
            subprocess.run(  # noqa: S603 -- ADR-1241: fixed fixture commands, isolated Git config.
                args, cwd=cwd or self.repo, env=self.env, text=True, capture_output=True
            )
        )
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def run_git(
        self, *args: str, cwd: Path | None = None, check: bool = True
    ) -> subprocess.CompletedProcess[str]:
        return self.run_command("git", "-C", str(cwd or self.repo), *args, check=check)

    def install(
        self, *, cwd: Path | None = None, check: bool = True
    ) -> subprocess.CompletedProcess[str]:
        return self.run_command("bash", "scripts/githooks/install.sh", cwd=cwd, check=check)

    def events(self) -> list[list[str]]:
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_installer_worktree_removal_and_stage_failures(self) -> None:
        worktree = self.repo / ".claude/worktrees/agent-installer"
        self.run_git("worktree", "add", "-b", "installer", str(worktree))
        hooks = self.repo / ".git/hooks"
        # Match the live clone: explicit core.hooksPath and a stale source link.
        self.run_git("config", "core.hooksPath", str(hooks))
        (hooks / "pre-push").symlink_to(worktree / "scripts/git-hooks/pre-push")
        self.install(cwd=worktree)
        backups = list(hooks.glob("pre-push.vmafx-backup-*"))
        self.assertEqual(len(backups), 1)
        self.assertTrue(backups[0].is_symlink())
        self.run_git("worktree", "remove", str(worktree))
        self.install()  # Idempotent: no extra backups and no vanished target.
        self.assertEqual(len(list(hooks.glob("pre-push.vmafx-backup-*"))), 1)
        self.run_git("switch", "-c", "feature")
        self.env["HOOK_TEST_FAIL"] = "pre-commit"
        self.assertNotEqual(
            self.run_git("commit", "--allow-empty", "-m", "test: blocked", check=False).returncode,
            0,
        )
        self.env.pop("HOOK_TEST_FAIL")
        self.assertNotEqual(
            self.run_git("commit", "--allow-empty", "-m", "bad message", check=False).returncode, 0
        )
        self.run_git("commit", "--allow-empty", "-m", "test: accepted")
        self.env["HOOK_TEST_FAIL"] = "pre-push"
        self.assertNotEqual(self.run_git("push", "origin", "feature", check=False).returncode, 0)
        self.env.pop("HOOK_TEST_FAIL")
        self.env["HOOK_TEST_MKDOCS_EXIT"] = "1"
        self.assertNotEqual(self.run_git("push", "origin", "feature", check=False).returncode, 0)
        self.env["HOOK_TEST_DRAFT"] = "1"
        self.assertNotEqual(self.run_git("push", "origin", "feature", check=False).returncode, 0)
        self.env["HOOK_TEST_MKDOCS_EXIT"] = "0"
        self.run_git("push", "origin", "feature")
        events = self.events()
        self.assertTrue(any(event[0] == "mkdocs" for event in events))
        self.assertTrue(
            any(event == ["pre-push", str(self.repo), "refs/heads/feature"] for event in events)
        )
        self.assertFalse((hooks / "pre-push").is_symlink())
        # The rebase guard must run before mutation when a sibling agent exists.
        self.run_git("worktree", "add", "-b", "sibling", str(worktree))
        result = self.run_command(str(hooks / "pre-rebase"), "master", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("agent-worktree-drift", result.stderr)
        self.run_command(str(hooks / "pre-rebase"), "master", cwd=worktree)

    def test_custom_regular_and_symlink_hooks_are_untouched(self) -> None:
        hooks = self.repo / ".git/hooks"
        path = hooks / "pre-push"
        for symlink in (False, True):
            with self.subTest(symlink=symlink):
                if symlink:
                    path.symlink_to(self.base / "user-hook-not-present")
                else:
                    path.write_text("#!/bin/sh\nexit 17\n")
                before = path.readlink() if symlink else path.read_text()
                result = self.install(check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("custom hooks left untouched", result.stderr)
                self.assertEqual(path.readlink() if symlink else path.read_text(), before)
                self.assertFalse((hooks / "pre-commit").exists())
                path.unlink()

    def test_framework_migration_preserves_legacy_and_native_stages(self) -> None:
        self.run_command(
            "pre-commit", "install", "--hook-type", "pre-commit", "--hook-type", "commit-msg"
        )
        hooks = self.repo / ".git/hooks"
        self.write(
            ".git/hooks/pre-push.legacy",
            '#!/bin/sh\nprintf \'["legacy"]\\n\' >> "$HOOK_TEST_LOG"\ncat >/dev/null\n',
            executable=True,
        )
        self.env["VMAFX_NATIVE_HOOKS"] = "1"
        self.install()
        self.assertEqual(len(list(hooks.glob("commit-msg.vmafx-backup-*"))), 1)
        self.assertNotEqual(
            self.run_git("commit", "--allow-empty", "-m", "bad native", check=False).returncode, 0
        )
        self.run_git("push", "origin", "master")
        events = self.events()
        self.assertIn(["legacy"], events)
        self.assertTrue(any(event[0] == "pre-push" for event in events))
        self.assertIn(["mkdocs"], events)


if __name__ == "__main__":
    unittest.main()
