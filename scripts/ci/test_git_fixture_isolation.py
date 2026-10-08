#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run Git fixture helpers under caller redirection without touching a real repo.

Also guards the fixture identity: no test script may write ``user.email`` or
``user.name`` into a git config, and every script that builds a scratch
repository must leave a hook-exported ``GIT_DIR`` repository untouched.
"""

from __future__ import annotations

import concurrent.futures
import datetime
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from functools import partial
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"
VARIABLES = (
    "GIT_DIR",
    "GIT_COMMON_DIR",
    "GIT_WORK_TREE",
    "GIT_INDEX_FILE",
    "GIT_CONFIG_PARAMETERS",
)


# `git config user.email|user.name` written to a config file. Per-command `-c user.*=`
# and GIT_AUTHOR_* / GIT_COMMITTER_* variables are the allowed ways to give a fixture an
# identity: they never persist, so they cannot leak into a caller's shared .git/config.
IDENTITY_WRITE = re.compile(
    r"""config\s+(?:--(?:local|global|worktree|system)\s+)?user\.(?:email|name)\b"""
    r"""|["']config["']\s*,\s*(?:["']--(?:local|global|worktree|system)["']\s*,\s*)?["']user\.(?:email|name)["']"""
)
# Scripts that create a scratch repository: a `git ... init` command or an "init" argument.
SCRATCH_REPOSITORY = re.compile(r"""git[^\n]*\binit\b|["']init["']""")
TEST_SCRIPT = re.compile(r"(^|/)test[-_][^/]*\.(sh|py)$")
# Workflow steps and Containerfiles are out of scope: a CI runner or an image build has no hook
# environment, and the clone they configure is ephemeral.
SUFFIXES = (".sh", ".py", ".mjs", ".go", ".rs", ".mk", "Makefile")

# An exception names one file, one rule, a reason and an expiry (ISO date) and fails like a
# missing exception once it expires. Every entry builds its git environment through a helper
# that drops GIT_*, and the runtime sentinel check below covers it as well.
IDENTITY_EXCEPTION_REASON = (
    "git helper scrubs GIT_* and ignores global config; sentinel check covers it"
)
IDENTITY_EXCEPTION_EXPIRY = "2026-12-31"
IDENTITY_EXCEPTION_FILES = (
    "scripts/ci/test_git_fixture_isolation.py",
    "scripts/ci/tests/test_check_source_adr_citations.py",
    "scripts/ci/tests/test_research_digest_ids.py",
    "scripts/ci/tests/test_scorecard_gate.py",
    "scripts/dev/tests/test_install_merge_train_guard.py",
    "scripts/dev/tests/test_merge_train_guard.py",
    "scripts/git-hooks/test-pre-push-mypy.py",
    "scripts/git-hooks/test-pre-push-pr-body-lint.py",
    "scripts/githooks/tests/test_install.py",
)


def tracked_files() -> list[str]:
    """List tracked files without inheriting a caller's repository."""
    done = run_git(ROOT, "ls-files", "-z", environment=clean_git_environment())
    listing = done.stdout
    assert isinstance(listing, str)
    return [name for name in listing.split("\0") if name]


def identity_writes(text: str) -> bool:
    """True when the text writes a fixture identity into a git config."""
    return IDENTITY_WRITE.search(text) is not None


def scratch_scripts() -> list[str]:
    """Tracked test scripts that build a scratch repository and can run on their own."""
    found = []
    for name in tracked_files():
        if not TEST_SCRIPT.search(name) or name == "scripts/ci/test_git_fixture_isolation.py":
            continue
        text = (ROOT / name).read_text(encoding="utf-8", errors="replace")
        runnable = name.endswith(".sh") or "__main__" in text
        if runnable and SCRATCH_REPOSITORY.search(text):
            found.append(name)
    return found


def sentinel_environment(sentinel: Path) -> dict[str, str]:
    """The environment a git hook gives its children: GIT_DIR and GIT_INDEX_FILE."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    environment.update(GIT_DIR=str(sentinel / ".git"), GIT_INDEX_FILE=str(sentinel / ".git/index"))
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    # Auto-maintenance would race the snapshot with a transient objects/maintenance.lock.
    environment.update(
        GIT_CONFIG_COUNT="2",
        GIT_CONFIG_KEY_0="maintenance.auto",
        GIT_CONFIG_VALUE_0="false",
        GIT_CONFIG_KEY_1="gc.auto",
        GIT_CONFIG_VALUE_1="0",
    )
    return environment


def make_sentinel(root: Path) -> Path:
    """A committed repository whose files must be byte-identical after a hook-run script."""
    sentinel = root / "sentinel"
    sentinel.mkdir()
    clean = clean_git_environment()
    run_git(sentinel, "init", "-q", "-b", "master", environment=clean)
    (sentinel / "tracked").write_text("caller work\n")
    run_git(sentinel, "add", "tracked", environment=clean)
    run_git(
        sentinel,
        "-c", "user.name=Sentinel", "-c", "user.email=sentinel@example.invalid",
        "commit", "-qm", "sentinel baseline",
        environment=clean,
    )  # fmt: skip
    return sentinel


def sentinel_damage(script: str, directory: Path) -> list[str]:
    """Run `script` from the checkout root under a sentinel GIT_DIR; list changed sentinel files."""
    sentinel = make_sentinel(directory)
    before = snapshot(sentinel)
    path = Path(script) if Path(script).is_absolute() else ROOT / script
    executable = sys.executable if path.suffix == ".py" else BASH
    run_command(
        [executable, str(path)],
        allowed_executables=(executable,),
        cwd=ROOT,
        env=sentinel_environment(sentinel),
        text=True,
        capture_output=True,
        timeout_seconds=600,
        check=False,
    )
    after = snapshot(sentinel)
    return sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))


def snapshot(directory: Path) -> dict[str, bytes]:
    """Capture config, refs, object store, index and work without refreshing Git."""
    return {
        path.relative_to(directory).as_posix(): path.read_bytes()
        for path in directory.rglob("*")
        if path.is_file()
    }


def clean_git_environment() -> dict[str, str]:
    """Return a deterministic environment with all caller Git state removed."""
    clean = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    clean.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, LC_ALL="C")
    return clean


def run_git(path: Path, *args: str, environment: dict[str, str]) -> CommandResult:
    """Run Git only inside a disposable fixture repository."""
    return run_command(
        [GIT, "-C", str(path), *args],
        allowed_executables=(GIT,),
        env=environment,
        text=True,
        capture_output=True,
        check=True,
        timeout_seconds=60,
    )


def linked_hook_command(root: Path, old_command: bool) -> list[str]:
    """Select the destructive control or the isolated fixed helper."""
    if old_command:
        return [GIT, "init", "-q", str(root / "old-fixture")]
    return [
        sys.executable,
        str(ROOT / "scripts/ci/tests/test_level_zero_single_source.py"),
        "LevelZeroSingleSource.test_workflow_checker_entrypoint_retains_container_validation",
    ]


class GitFixtureIsolation(unittest.TestCase):
    def exercise(self, helper: str, variables: tuple[str, ...]) -> None:
        with tempfile.TemporaryDirectory(prefix="git-fixture-isolation-") as directory:
            root = Path(directory)
            caller = root / "caller"
            caller.mkdir()
            temporary = root / "scratch"
            temporary.mkdir()
            # Even the adversarial test's setup must never inherit a real
            # caller's repository, index, object store or config overrides.
            clean = clean_git_environment()

            def git(*args: str) -> None:
                run_git(caller, *args, environment=clean)

            git("init", "-q", "-b", "master")
            git("config", "user.name", "Isolation Test")
            git("config", "user.email", "fixture@example.invalid")
            (caller / "tracked").write_text("committed caller work\n")
            git("add", "tracked")
            git("commit", "-qm", "test: caller baseline")
            (caller / "staged").write_text("unique staged caller work\n")
            git("add", "staged")
            (caller / "tracked").write_text("unique unstaged caller work\n")
            poison = {
                "GIT_DIR": str(caller / ".git"),
                "GIT_COMMON_DIR": str(caller / ".git"),
                "GIT_WORK_TREE": str(caller),
                "GIT_INDEX_FILE": str(caller / ".git/index"),
                "GIT_CONFIG_PARAMETERS": "'core.bare=true' 'fixture.marker=inherited'",
            }
            environment = {
                **clean,
                **{key: poison[key] for key in variables},
                "TMPDIR": str(temporary),
            }
            before = snapshot(caller)
            executable = sys.executable if helper.endswith(".py") else BASH
            result = run_command(
                [executable, str(ROOT / helper)],
                allowed_executables=(executable,),
                cwd=root,
                env=environment,
                text=True,
                capture_output=True,
                timeout_seconds=90,
                check=False,
            )
            after = snapshot(caller)
            changed = sorted(
                key for key in before.keys() | after.keys() if before.get(key) != after.get(key)
            )
            self.assertEqual(
                changed, [], f"{helper} changed caller files under {variables}: {changed}"
            )
            self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-2000:])

    def check_helper(self, helper: str) -> None:
        for variables in (*((variable,) for variable in VARIABLES), VARIABLES):
            with self.subTest(helper=helper, variables=variables):
                self.exercise(helper, variables)

    def test_replay_fixture(self) -> None:
        self.check_helper("scripts/ci/test_ffmpeg_patch_stack.py")

    def test_smoke_fixture(self) -> None:
        self.check_helper("scripts/ci/test_ffmpeg_patch_smoke_safety.py")

    def test_cleanup_fixture(self) -> None:
        self.check_helper("scripts/dev/test-cleanup-agent-state.sh")

    def test_state_md_resolver_fixture(self) -> None:
        self.check_helper("scripts/dev/test-resolve-state-md-conflict.py")

    def test_dependency_classifier_fixture(self) -> None:
        self.check_helper("scripts/ci/test-classify-dependency-pr.sh")

    def test_level_zero_fixture(self) -> None:
        self.check_helper("scripts/ci/tests/test_level_zero_single_source.py")

    def test_real_linked_worktree_hook_preserves_shared_repository(self) -> None:
        # A real Git hook supplies GIT_DIR even when its caller has no Git
        # variables. Keep both the old-command control and fixed helper inside
        # fresh disposable linked repositories, never the developer's checkout.
        for old_command in (True, False):
            with (
                self.subTest(old_command=old_command),
                tempfile.TemporaryDirectory(prefix="git-linked-hook-") as directory,
            ):
                root = Path(directory)
                caller = root / "caller"
                linked = root / "linked"
                caller.mkdir()
                git = partial(run_git, environment=clean_git_environment())

                git(caller, "init", "-q", "-b", "master")
                git(caller, "config", "user.name", "Linked Hook Test")
                git(caller, "config", "user.email", "fixture@example.invalid")
                (caller / "tracked").write_text("committed caller work\n")
                git(caller, "add", "tracked")
                git(caller, "commit", "-qm", "test: linked hook baseline")
                git(caller, "worktree", "add", "-q", "-b", "linked", str(linked))
                for path in (caller, linked):
                    (path / "staged").write_text("unique staged work\n")
                    git(path, "add", "staged")
                    (path / "tracked").write_text("unique unstaged work\n")
                self.assertEqual(
                    git(caller, "config", "--local", "--get", "core.bare").stdout, "false\n"
                )
                marker = root / "hook-environment.json"
                command = linked_hook_command(root, old_command)
                hook = caller / ".git/hooks/pre-commit"
                hook.write_text(
                    f"#!{sys.executable}\n"
                    "import json, os, subprocess\nfrom pathlib import Path\n"
                    f"Path({str(marker)!r}).write_text(json.dumps({{'GIT_DIR': os.environ.get('GIT_DIR')}}))\n"
                    f"subprocess.run({command!r}, check=True)\n"
                )
                hook.chmod(0o700)
                before_caller, before_linked = snapshot(caller), snapshot(linked)
                git(linked, "hook", "run", "pre-commit")
                inherited = json.loads(marker.read_text())["GIT_DIR"]
                self.assertEqual((linked / inherited).resolve(), caller / ".git/worktrees/linked")
                after_caller, after_linked = snapshot(caller), snapshot(linked)
                if old_command:
                    self.assertNotEqual(after_caller[".git/config"], before_caller[".git/config"])
                    self.assertIn(b"bare = true", after_caller[".git/config"])
                else:
                    self.assertEqual(after_caller, before_caller)
                    self.assertEqual(after_linked, before_linked)

    def test_no_script_writes_a_fixture_identity_into_a_git_config(self) -> None:
        today = datetime.date.today().isoformat()
        offenders = []
        for name in tracked_files():
            if not name.endswith(SUFFIXES) and Path(name).name != "Makefile":
                continue
            path = ROOT / name
            if not path.is_file():
                continue
            if not identity_writes(path.read_text(encoding="utf-8", errors="replace")):
                continue
            if name not in IDENTITY_EXCEPTION_FILES or today > IDENTITY_EXCEPTION_EXPIRY:
                offenders.append(name)
        self.assertEqual(
            offenders,
            [],
            "fixture identity written to a git config; use GIT_AUTHOR_*/GIT_COMMITTER_* "
            "(scripts/lib/clean-git-env.sh) or `git -c user.*=`: " + ", ".join(offenders),
        )

    def test_identity_exceptions_are_live_and_needed(self) -> None:
        self.assertGreaterEqual(
            IDENTITY_EXCEPTION_EXPIRY, datetime.date.today().isoformat(), "exceptions expired"
        )
        for name in IDENTITY_EXCEPTION_FILES:
            with self.subTest(name=name):
                text = (ROOT / name).read_text(encoding="utf-8", errors="replace")
                self.assertTrue(identity_writes(text), "no longer needed: remove the exception")

    def test_identity_rule_refuses_the_planted_old_scripts(self) -> None:
        # Negative controls: the exact lines of the scripts that leaked `t@t` on 2026-10-08.
        for planted in (
            'cd "$tmp"\ngit init -q\ngit config user.email t@t\ngit config user.name t\n',
            'git -C "$dst" config user.email t@t\n',
            '        self.git("config", "user.email", "test@example.invalid")\n',
        ):
            with self.subTest(planted=planted):
                self.assertTrue(identity_writes(planted))
        for allowed in (
            "git -c user.name=t -c user.email=t@example.invalid commit -qm x\n",
            "export GIT_AUTHOR_EMAIL=t@example.invalid\n",
            "git config --get user.email\n",
        ):
            with self.subTest(allowed=allowed):
                self.assertFalse(identity_writes(allowed))

    def test_planted_leaking_script_damages_the_sentinel(self) -> None:
        # Runtime negative control: the old test-preflight-msvcism.sh pattern, run the way a
        # hook runs it, must change the sentinel; the gate below must be able to see that.
        with tempfile.TemporaryDirectory(prefix="git-identity-planted-") as directory:
            root = Path(directory)
            leak = root / "test-planted-leak.sh"
            leak.write_text(
                '#!/usr/bin/env bash\nset -euo pipefail\ntmp=$(mktemp -d)\ncd "$tmp"\n'
                "git init -q\ngit config user.email t@t\ngit config user.name t\n"
            )
            changed = sentinel_damage(str(leak), root)
            self.assertIn(".git/config", changed)

    def test_scratch_repository_scripts_leave_a_hook_git_dir_alone(self) -> None:
        scripts = scratch_scripts()
        self.assertGreater(len(scripts), 30, "discovery found too few scratch-repository scripts")

        def damage(script: str) -> list[str]:
            with tempfile.TemporaryDirectory(prefix="git-identity-sentinel-") as directory:
                return sentinel_damage(script, Path(directory))

        # Four workers: the scripts are independent and each owns its sentinel.
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = dict(zip(scripts, pool.map(damage, scripts), strict=True))
        for script, changed in results.items():
            with self.subTest(script=script):
                self.assertEqual(changed, [], f"{script} changed the hook's GIT_DIR repository")

    def test_local_and_required_ci_hook_registration(self) -> None:
        config = (ROOT / ".pre-commit-config.yaml").read_text()
        hook = config.split("      - id: test-git-fixture-isolation\n", 1)[1].split(
            "      - id:", 1
        )[0]
        self.assertIn("entry: python3 scripts/ci/test_git_fixture_isolation.py", hook)
        self.assertIn("stages: [pre-commit, pre-push]", hook)
        self.assertIn("pass_filenames: false", hook)
        pattern = hook.split("files: '", 1)[1].split("'", 1)[0]
        for path in (
            "scripts/ci/test_git_fixture_isolation.py",
            "scripts/ci/test_ffmpeg_patch_stack.py",
            "scripts/ci/test_ffmpeg_patch_smoke_safety.py",
            "scripts/dev/test-cleanup-agent-state.sh",
            "scripts/dev/test-resolve-state-md-conflict.py",
            "scripts/dev/resolve-state-md-conflict.py",
            "scripts/ci/test-classify-dependency-pr.sh",
            "scripts/ci/tests/test_level_zero_single_source.py",
            ".pre-commit-config.yaml",
        ):
            self.assertIsNotNone(re.search(pattern, path), path)
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text()
        self.assertIn("name: Pre-Commit", workflow)
        self.assertIn("pre-commit run --show-diff-on-failure --color=always --all-files", workflow)


if __name__ == "__main__":
    unittest.main()
