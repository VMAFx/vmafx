# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared isolated Git repository fixtures for ``scripts/ci/check-dco.py`` tests."""

from __future__ import annotations

import importlib.util
import json
import multiprocessing
import os
import shutil
import sys
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib import git_env as git_env_lib  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult, run  # noqa: E402

GATE = ROOT / "scripts" / "ci" / "check-dco.py"
GIT = shutil.which("git") or "/usr/bin/git"
GIT_TIMEOUT_S = 60
SCRIPT_TIMEOUT_S = 60
# Repository-path variables the gate subprocess must not inherit (see clean_env).
REPO_ENV = (
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_COMMON_DIR",
    "GIT_INDEX_FILE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_NAMESPACE",
)
HUMAN = ("Ada Example", "ada@example.org")
BOT_LOGIN = "renovate[bot]"
BOT_IDENT = (BOT_LOGIN, "29139614+renovate[bot]@users.noreply.github.com")
LANDING_ID = "Lusoris <lusoris@proton.me>"

LANDED_3042 = """chore(deps): Update Python (patch) (#3042)

* chore(deps): Update Python (patch)

Signed-off-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>

---------

Signed-off-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>
Signed-off-by: Lusoris <lusoris@proton.me>
Co-authored-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>
Co-authored-by: Lusoris <lusoris@proton.me>
"""

ADVERSARIAL_PREFIXES = (
    ("hashes", "#" * 64 + "x"),
    ("whitespace", " " * 5000 + "x"),
    ("bullets", "- " * 5000 + "x"),
    ("comments", "<!-- " * 2000 + "x"),
)


def git_env(ident: tuple[str, str], committer: tuple[str, str] | None = None) -> dict[str, str]:
    """Prepare isolated fixture identities without inherited repository configuration."""
    committer = committer or ident
    env = git_env_lib.hermetic()
    env.update(
        GIT_AUTHOR_NAME=ident[0],
        GIT_AUTHOR_EMAIL=ident[1],
        GIT_COMMITTER_NAME=committer[0],
        GIT_COMMITTER_EMAIL=committer[1],
    )
    return env


def clean_env() -> dict[str, str]:
    """The environment without caller Git state or the variables the gate reads."""
    drop = (
        *REPO_ENV,
        "BASE_SHA",
        "HEAD_SHA",
        "PR_AUTHOR",
        "PR_AUTHOR_TYPE",
        "PR_CREATED_AT",
        "DCO_RELEASE_PR",
        "LANDING_IDENTITY",
        "PR_BODY",
        "PRE_COMMIT_TO_REF",
    )
    env = git_env_lib.hermetic()
    for key in drop:
        env.pop(key, None)
    return env


class Repo:
    """A throwaway repository with an initial commit on ``main``."""

    def __init__(self, path: Path) -> None:
        """Create an isolated repository initialized with one signed base commit."""
        self.path = path
        self.git(["init", "-q", "-b", "main"])
        self.commit("initial", HUMAN, signoff=True)
        self.base = self.git(["rev-parse", "HEAD"]).strip()

    def git(
        self,
        args: list[str],
        ident: tuple[str, str] = HUMAN,
        committer: tuple[str, str] | None = None,
        stdin: str | None = None,
    ) -> str:
        """Run fixture Git commands for scripts/ci/check-dco.py.

        Sets up commits and git state in an isolated test repo.
        """
        done = run(
            [GIT, "-c", "commit.gpgsign=false", *args],
            allowed_executables=(GIT,),
            cwd=self.path,
            env=git_env(ident, committer),
            input_data=stdin,
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=GIT_TIMEOUT_S,
        )
        return done.stdout

    def commit(
        self,
        message: str,
        ident: tuple[str, str],
        signoff: bool = False,
        committer: tuple[str, str] | None = None,
    ) -> str:
        """Create a fixture commit with optional sign-off and return its revision."""
        n = len(list(self.path.glob("f*.txt")))
        (self.path / f"f{n}.txt").write_text(f"{n}\n")
        self.git(["add", "-A"], ident)
        flags = ["-s"] if signoff else []
        self.git(["commit", "-q", *flags, "-F", "-"], ident, committer, stdin=message)
        return self.git(["rev-parse", "HEAD"], ident).strip()

    def gate(
        self, *extra: str, pr_body: str | None = None, event: object | None = None
    ) -> TextCommandResult:
        """Run scripts/ci/check-dco.py against commits in the test repo.

        Runs the DCO gate against commits between base and head in the test repo.
        """
        head = self.git(["rev-parse", "HEAD"]).strip()
        env = clean_env()
        if pr_body is not None:
            env["PR_BODY"] = pr_body
        if event is not None:
            event_file = self.path / "event.json"
            event_file.write_text(json.dumps(event), encoding="utf-8")
            env["GITHUB_EVENT_PATH"] = str(event_file)
        return run(
            [sys.executable, "-I", str(GATE), "--base", self.base, "--head", head, *extra],
            allowed_executables=(sys.executable,),
            cwd=self.path,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=SCRIPT_TIMEOUT_S,
        )


def load_gate(name: str) -> ModuleType:
    """Load the policy module under a registered name for dataclass resolution."""
    spec = importlib.util.spec_from_file_location(name, GATE)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def assert_no_machine_attribution(line: str) -> None:
    """Check a potentially expensive line inside a disposable child process."""
    mod = load_gate("check_dco_child")
    assert mod.check_machine_attribution(line) is False


def run_prefix_scan_child(line: str) -> str | None:
    """Scan one line in a spawned child; return the failure, or None when it passes in time."""
    child = multiprocessing.get_context("spawn").Process(
        target=assert_no_machine_attribution, args=(line,)
    )
    child.start()
    try:
        child.join(timeout=2)
        if child.is_alive():
            return "attribution scan exceeded two seconds"
        if child.exitcode != 0:
            return "non-attribution line was refused or child failed"
        return None
    finally:
        if child.is_alive():
            child.kill()
            child.join(timeout=2)
        child.close()
