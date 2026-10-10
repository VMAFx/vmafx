#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run Git fixture helpers under caller redirection without touching a real repo.

Also guards the fixture identity: no test script may write ``user.email`` or
``user.name`` into a git config, and every script that builds a scratch
repository must leave a hook-exported ``GIT_DIR`` repository untouched.

Three nets, because each one misses what another catches:

* the identity rule: no tracked script persists ``user.*``;
* the isolation rule: a tracked script that creates a repository (``git init``,
  ``git clone``) or persists any git config key (``core.bare``,
  ``uploadpack.*``, ...) must drop the caller's ``GIT_*`` variables in the
  same file, or be a declared exception. This is the net for a pytest-only
  test and for a script that is not a test, which the third net cannot run;
* the sentinel run: every scratch-repository test script that runs on its
  own is executed the way a hook runs it, and must leave the hook's
  repository byte-identical.
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


# The isolation rule. A script that runs one of these acts on the CALLER's repository when a
# hook's GIT_DIR / GIT_INDEX_FILE are still set, whatever its working directory is.
SHELL_SUFFIXES = (".sh", ".mk", "Makefile")
_GIT_OPTIONS = r"(?:\s+-[cC]\s*\S+|\s+--[\w-]+(?:=\S+)?)*"
_READ_ONLY = r"(?:get|get-all|get-regexp|get-urlmatch|list|show-origin|show-scope|name-only)"
SHELL_GIT_WRITE = re.compile(
    r"(?m)^(?!\s*#)[^\n#]*?\bgit\b" + _GIT_OPTIONS + r"\s+(?:(?:init|clone)\b"
    r"|config\s+(?!--" + _READ_ONLY + r"\b)"
    r"(?:--(?:local|global|worktree|system|add|replace-all|file\s+\S+)\s+)*[\w.-]+\.[\w.-]+[ \t]+\S)"
)
CODE_GIT_WRITE = re.compile(
    r"""(?im)^(?!\s*(?:#|//))[^\n]*\bgit\w*[^\n]{0,80}?["'](?:init|clone)["']\s*[,)\]]"""
    r"""|["']config["']\s*,\s*(?:["']--(?:local|global|worktree|system|add|replace-all)["']\s*,\s*)*"""
    r"""["'][\w.-]+\.[\w.-]+["']\s*,\s*\S"""
)
# What counts as dropping the caller's variables: the two shell helpers (or their loop), and a
# filter on the GIT_ prefix in Python, Node and Go.
ISOLATION_MARKER = re.compile(
    r"""(?:clean|drop)-git-env\.sh|compgen -A variable GIT_"""
    r"""|startswith\(\s*\(?\s*["']GIT_|startsWith\(\s*["']GIT_|HasPrefix\([^)\n]*"GIT_"""
    r"""|clean_git_environment\(|\bgit_environment\("""
)
# One file, one reason, one expiry: a file whose git runs isolated in a way the marker cannot
# see, with the place that does it. The sentinel run covers every test script among them.
ISOLATION_EXCEPTION_EXPIRY = "2026-12-31"
ISOLATION_EXCEPTIONS = {
    ".config/lefthook/scripts/common.py": "clean_env() removes the repository variables by name",
    "scripts/ci/tests/test_research_digest_ids.py": (
        "git runs through check-research-digest-ids._git, which drops GIT_*"
    ),
    "scripts/dev/tests/test_install_merge_train_guard.py": (
        "git runs through install_merge_train_guard.git, which drops GIT_*"
    ),
    "scripts/dev/tests/test_merge_train_guard.py": (
        "git runs through merge_train_guard.environment(), which drops GIT_*"
    ),
    "scripts/docs/tests/test_render_at_landing.py": (
        "every git call gets a fixed environment (ENV) that holds no caller variable"
    ),
    "tools/apicompat/gate/main.go": "main() unsets gitRepositoryVariables before any git call",
    # NOT isolated: the fixtures of its --self-test mode inherit the environment. The file is the
    # governance engine's template and no hook or workflow of this repository runs that mode.
    "tools/markdownlint/verify.mjs": (
        "engine template; --self-test fixtures inherit GIT_*; that mode is not run here"
    ),
}


def unisolated_git(name: str, text: str) -> str:
    """The first repository-creating or config-writing git call of an unisolated script, or ""."""
    shell = name.endswith(SHELL_SUFFIXES)
    found = (SHELL_GIT_WRITE if shell else CODE_GIT_WRITE).search(text)
    if found is None or ISOLATION_MARKER.search(text):
        return ""
    return found.group(0).strip()[:100]


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
        # Runtime negative controls, each run the way a hook runs it: the old
        # test-preflight-msvcism.sh pattern (identity), the two other keys that reached the
        # shared config (core.bare, uploadpack.*), and a clone. Each must change the sentinel;
        # the gate below must be able to see that.
        head = '#!/usr/bin/env bash\nset -euo pipefail\ntmp=$(mktemp -d)\ncd "$tmp"\ngit init -q\n'
        planted = {
            "identity": (
                head + "git config user.email t@t\ngit config user.name t\n",
                ".git/config",
            ),
            "core.bare": (head + "git config core.bare true\n", ".git/config"),
            "uploadpack": (
                head + "git config uploadpack.allowAnySHA1InWant true\n",
                ".git/config",
            ),
            "clone": (
                head + "git -c user.name=t -c user.email=t@example.invalid commit -q --allow-empty"
                ' -m x\ngit clone -q "$tmp" "$tmp/copy"\n',
                "",
            ),
        }
        for label, (body, expected) in planted.items():
            with (
                self.subTest(leak=label),
                tempfile.TemporaryDirectory(prefix="git-identity-planted-") as directory,
            ):
                root = Path(directory)
                leak = root / "test-planted-leak.sh"
                leak.write_text(body)
                changed = sentinel_damage(str(leak), root)
                self.assertNotEqual(changed, [], f"{label}: the sentinel saw nothing")
                if expected:
                    self.assertIn(expected, changed)

    def test_the_isolated_form_of_each_planted_script_leaves_the_sentinel_alone(self) -> None:
        # The same commands after scripts/lib/clean-git-env.sh: the controls above fail for
        # the missing isolation, not for the commands.
        helper = ROOT / "scripts/lib/clean-git-env.sh"
        body = (
            f'#!/usr/bin/env bash\nset -euo pipefail\n. "{helper}"\ntmp=$(mktemp -d)\ncd "$tmp"\n'
            "git init -q\ngit config core.bare false\n"
            "git config uploadpack.allowAnySHA1InWant true\n"
            'git commit -q --allow-empty -m x\ngit clone -q "$tmp" "$tmp/copy"\n'
        )
        with tempfile.TemporaryDirectory(prefix="git-identity-isolated-") as directory:
            root = Path(directory)
            script = root / "test-planted-isolated.sh"
            script.write_text(body)
            self.assertEqual(sentinel_damage(str(script), root), [])

    def test_no_script_creates_or_configures_a_repository_unisolated(self) -> None:
        today = datetime.date.today().isoformat()
        offenders = []
        for name in tracked_files():
            if not name.endswith(SUFFIXES) or name == "scripts/ci/test_git_fixture_isolation.py":
                continue
            path = ROOT / name
            if not path.is_file():
                continue
            sample = unisolated_git(name, path.read_text(encoding="utf-8", errors="replace"))
            if sample and (name not in ISOLATION_EXCEPTIONS or today > ISOLATION_EXCEPTION_EXPIRY):
                offenders.append(f"{name}: {sample}")
        self.assertEqual(
            offenders,
            [],
            "git creates or configures a repository with the caller's GIT_* still set; source "
            "scripts/lib/clean-git-env.sh (or drop-git-env.sh), or build the environment "
            'without keys that start with "GIT_":\n' + "\n".join(offenders),
        )

    def test_isolation_exceptions_are_live_and_needed(self) -> None:
        self.assertGreaterEqual(
            ISOLATION_EXCEPTION_EXPIRY, datetime.date.today().isoformat(), "exceptions expired"
        )
        for name, reason in ISOLATION_EXCEPTIONS.items():
            with self.subTest(name=name):
                self.assertTrue(reason)
                text = (ROOT / name).read_text(encoding="utf-8", errors="replace")
                self.assertTrue(unisolated_git(name, text), "no longer needed: remove it")

    def test_isolation_rule_refuses_planted_unisolated_fixtures(self) -> None:
        planted = {
            "t.sh": (
                'cd "$tmp"\ngit init -q\n',
                'git -C "$dst" config core.bare true\n',
                "git config --local uploadpack.allowAnySHA1InWant true\n",
                'timeout 600 git -c advice.detachedHead=false clone --quiet "$url" "$dir"\n',
            ),
            "t.py": (
                'subprocess.run(["git", "init", "-q", str(repo)], check=True)\n',
                '        self.git("init", "-q", "-b", "master")\n',
                '    git("config", "uploadpack.allowAnySHA1InWant", "true")\n',
                '    run_git(root, "config", "core.bare", "true")\n',
                '    run((git, "clone", "--bare", str(src), str(dst)))\n',
            ),
            "t.mjs": ('  command("git", ["init", "--quiet"], { cwd: fixture });\n',),
            "t.go": ('\trunTool(ctx, repo, "git", "init", "--quiet")\n',),
        }
        for name, texts in planted.items():
            for text in texts:
                with self.subTest(name=name, text=text):
                    self.assertTrue(unisolated_git(name, text))
        isolated = {
            "t.sh": '. "$ROOT/scripts/lib/clean-git-env.sh"\ngit init -q\ngit config core.bare true\n',
            "u.sh": '. "$ROOT/scripts/lib/drop-git-env.sh"\ngit clone -q "$url" "$dir"\n',
            "t.py": (
                'env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}\n'
                'subprocess.run(["git", "init"], env=env)\n'
            ),
            "u.py": (
                'env = {k: v for k, v in os.environ.items() if not k.startswith(("GIT_", "GH_"))}\n'
                'self.git("config", "core.bare", "true")\n'
            ),
        }
        for name, text in isolated.items():
            with self.subTest(name=name):
                self.assertEqual(unisolated_git(name, text), "")
        harmless = {
            "t.sh": (
                "# git init would be wrong here\n",
                "git config --get user.email\n",
                "git config --list\n",
                'git -C "$repo" log --oneline\n',
            ),
            "t.py": (
                'value = git("config", "--get", "user.email")\n',
                'state = registration.get("init", "")\n',
                '# self.git("init") is what the fixture used to do\n',
            ),
        }
        for name, texts in harmless.items():
            for text in texts:
                with self.subTest(name=name, text=text):
                    self.assertEqual(unisolated_git(name, text), "")

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
