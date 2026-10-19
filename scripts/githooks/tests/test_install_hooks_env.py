#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hook environments install outside the commit's git environment.

In a linked worktree git exports an absolute ``GIT_INDEX_FILE`` to hooks. When
the pre-commit framework (re)installs a ``language: node`` hook it runs
``npm install -g git+file://<hook repo>`` with that variable set, and npm's git
checkout writes the hook repository's tree into the worktree's index. The
``framework-hooks`` entries of ``lefthook.yml`` therefore run
``pre-commit install-hooks`` with the commit's git variables unset before
``run`` / ``hook-impl``.

pre-commit 4.7.0 strips the git variables from its own ``npm install`` (its
pull request 3774), so the block is now the second barrier. A pre-commit that
runs npm with the commit's environment, as every release before 4.7.0 did, is
modelled by a ``pre-commit`` wrapper first on PATH: on its first call it runs
``npm install -g git+file://<hook repo>`` with the environment it was given,
then hands over to the real framework.

These tests take the real ``run`` scripts from ``lefthook.yml`` and execute them
as a genuine ``pre-commit`` hook of a linked worktree, so git itself exports the
variable. pre-commit 4.7.0 runs that ``npm install`` without the git variables
itself (pre-commit/pre-commit#3774), so a framework of that version no longer
shows the defect. The negative case therefore plants the install step of
pre-commit 4.6 directly: run with the commit's git variables it must pollute the
index, and behind the shipped block's ``env -u`` prefix it must not. The
positive cases run the shipped block and must leave the index alone.
"""

from __future__ import annotations

import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import TextCommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
UNSET = "env -u GIT_INDEX_FILE -u GIT_DIR -u GIT_WORK_TREE -u GIT_OBJECT_DIRECTORY"
TOOLS = ("git", "pre-commit", "npm", "node")
EXPECTED = [".pre-commit-config.yaml", "f1", "f2", "f3", "f4", "new.txt"]


def framework_block(stage: str) -> str:
    """Return the shipped ``framework-hooks`` run script of a lefthook stage."""
    config = yaml.safe_load((ROOT / "lefthook.yml").read_text())
    run = str(config[stage]["commands"]["framework-hooks"]["run"]).strip()
    match = re.match(r"^bash\s+([^\s]+)", run)
    if match:
        script = (ROOT / match.group(1)).read_text()
        return f"stage={stage}\n" + script
    return run


class InstallHooksEnvTests(unittest.TestCase):
    def setUp(self) -> None:
        missing = [tool for tool in TOOLS if shutil.which(tool) is None]
        if missing:
            self.skipTest(f"needs {', '.join(missing)} on PATH; the contract is not exercised")
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-install-env-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("GIT_", "PRE_COMMIT_")) and key != "SKIP"
        }
        self.env.update(
            GIT_CONFIG_NOSYSTEM="1",
            GIT_CONFIG_GLOBAL=os.devnull,
            PRE_COMMIT_HOME=str(self.base / "cache"),
            HOOK_TEST_LOG=str(self.base / "ran.log"),
        )
        self.hook_repo = self.base / "hook"
        self.hook_repo.mkdir()
        self.git("init", "-b", "master", cwd=self.hook_repo)
        (self.hook_repo / "package.json").write_text(
            '{"name":"probe-hook","version":"1.0.0","bin":{"probe-hook":"bin.js"}}\n'
        )
        (self.hook_repo / "bin.js").write_text(
            "#!/usr/bin/env node\n"
            "require('fs').appendFileSync(process.env.HOOK_TEST_LOG, 'ran\\n');\n"
        )
        (self.hook_repo / "bin.js").chmod(0o755)
        (self.hook_repo / ".pre-commit-hooks.yaml").write_text(
            "- id: probe-hook\n  name: probe-hook\n  entry: probe-hook\n  language: node\n"
            "  language_version: system\n  always_run: true\n  pass_filenames: false\n"
        )
        self.git("add", ".", cwd=self.hook_repo)
        self.commit("test: hook", cwd=self.hook_repo)
        self.install_leaky_framework()
        revision = self.git("rev-parse", "HEAD", cwd=self.hook_repo).stdout.strip()
        self.main = self.base / "main"
        self.main.mkdir()
        self.git("init", "-b", "master", cwd=self.main)
        for name in ("f1", "f2", "f3", "f4"):
            (self.main / name).write_text(name + "\n")
        (self.main / ".pre-commit-config.yaml").write_text(
            f"repos:\n  - repo: {self.hook_repo.as_uri()}\n    rev: {revision}\n"
            "    hooks:\n      - id: probe-hook\n"
        )
        self.git("add", ".", cwd=self.main)
        self.commit("test: fixture", cwd=self.main)
        self.worktree = self.base / "worktree"
        self.git("worktree", "add", "-b", "feature", str(self.worktree), cwd=self.main)

    def install_leaky_framework(self) -> None:
        """Put a pre-commit first on PATH that installs node hooks with the caller's git env."""
        real = shutil.which("pre-commit", path=self.env.get("PATH"))
        self.assertIsNotNone(real)
        stub_dir = self.base / "stub-bin"
        stub_dir.mkdir()
        stub = stub_dir / "pre-commit"
        stub.write_text(
            "#!/bin/sh\n"
            'marker="$LEAKY_NPM_PREFIX/.installed"\n'
            'if [ ! -e "$marker" ]; then\n'
            '  npm install --allow-git=root --install-links -g --prefix "$LEAKY_NPM_PREFIX" "$LEAKY_HOOK_URL" >/dev/null 2>&1 || exit 1\n'
            '  : > "$marker"\n'
            "fi\n"
            f'exec "{real}" "$@"\n'
        )
        stub.chmod(0o755)
        (self.base / "npm-prefix").mkdir()
        self.env.update(
            PATH=f"{stub_dir}{os.pathsep}{self.env.get('PATH', '')}",
            LEAKY_NPM_PREFIX=str(self.base / "npm-prefix"),
            LEAKY_HOOK_URL="git+" + self.hook_repo.as_uri(),
        )

    def git(self, *args: str, cwd: Path, check: bool = True) -> TextCommandResult:
        return self.sh("git", "-C", str(cwd), *args, check=check)

    def commit(self, message: str, *, cwd: Path) -> TextCommandResult:
        return self.git(
            "-c", "user.name=Hook Test", "-c", "user.email=hook-test@example.invalid",
            "commit", "-m", message, cwd=cwd,
        )  # fmt: skip

    def sh(self, *args: str, check: bool = True) -> TextCommandResult:
        result = run_command(
            args,
            allowed_executables=(args[0],),
            env=self.env,
            text=True,
            capture_output=True,
            timeout_seconds=300,
        )
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def tracked(self, cwd: Path) -> list[str]:
        return self.git("ls-files", cwd=cwd).stdout.split()

    def commit_with_block(self, block: str, checkout: Path) -> tuple[int, list[str]]:
        """Commit one new file through ``block`` installed as the pre-commit hook."""
        hooks = Path(self.git("rev-parse", "--git-path", "hooks", cwd=checkout).stdout.strip())
        hooks = hooks if hooks.is_absolute() else checkout / hooks
        hooks.mkdir(parents=True, exist_ok=True)
        hook = hooks / "pre-commit"
        hook.write_text("#!/bin/sh\n" + block + "\n")
        hook.chmod(0o755)
        (checkout / "new.txt").write_text("new\n")
        self.git("add", "new.txt", cwd=checkout)
        result = self.git(
            "-c", "user.name=Hook Test", "-c", "user.email=hook-test@example.invalid",
            "commit", "-m", "test: change", cwd=checkout, check=False,
        )  # fmt: skip
        return result.returncode, self.tracked(checkout)

    def ran(self) -> bool:
        log = Path(self.env["HOOK_TEST_LOG"])
        return log.is_file() and "ran" in log.read_text()

    def npm_install_block(self, prefix: str) -> str:
        """Return the ``npm install`` of a node hook as pre-commit 4.6 ran it."""
        log = self.base / "npm.log"
        return (
            f'cd "{self.hook_repo}" && {prefix}env NPM_CONFIG_PREFIX="{self.base / "npm-env"}" '
            f'npm install --allow-git=root --install-links -g --cache "{self.base / "npm-cache"}" '
            f'"git+{self.hook_repo.as_uri()}" > "{log}" 2>&1 || {{ cat "{log}" >&2; exit 1; }}'
        )

    def test_negative_npm_install_with_the_commits_git_env_pollutes_the_index(self) -> None:
        returncode, tracked = self.commit_with_block(self.npm_install_block(""), self.worktree)
        self.assertNotEqual(returncode, 0)
        self.assertNotEqual(
            sorted(tracked), EXPECTED,
            "the planted install must reproduce the defect, else this test proves nothing",
        )  # fmt: skip

    def test_positive_npm_install_behind_the_shipped_prefix_keeps_the_index(self) -> None:
        self.assertIn(f'{UNSET} "$fw" install-hooks', framework_block("pre-commit"))
        returncode, tracked = self.commit_with_block(
            self.npm_install_block(UNSET + " "), self.worktree
        )
        self.assertEqual(returncode, 0)
        self.assertEqual(sorted(tracked), EXPECTED)

    def test_positive_shipped_block_leaves_the_index_alone(self) -> None:
        returncode, tracked = self.commit_with_block((framework_block("pre-commit")), self.worktree)
        self.assertEqual(returncode, 0)
        self.assertEqual(sorted(tracked), EXPECTED)
        self.assertTrue(self.ran(), "the node hook must still run")

    def test_boundary_main_checkout_with_relative_index(self) -> None:
        returncode, tracked = self.commit_with_block((framework_block("pre-commit")), self.main)
        self.assertEqual(returncode, 0)
        self.assertEqual(sorted(tracked), EXPECTED)
        self.assertTrue(self.ran())

    def test_both_stages_install_before_they_run_and_fail_closed(self) -> None:
        for stage, runner in (("pre-commit", '"$fw" run'), ("pre-push", '"$fw" hook-impl')):
            block = framework_block(stage)
            install = f'{UNSET} "$fw" install-hooks || exit 1'
            self.assertIn(install, block, stage)
            self.assertLess(block.index(install), block.index(runner), stage)
            self.assertIn("pre-commit is missing", block, stage)


if __name__ == "__main__":
    unittest.main()
