#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Planted-case tests for check-bot-squash-body.py.


Proves the defence catches the dangerous pattern AND passes safe ones:

- REFUSE: bot PR, auto-merge armed, null body, non-bot commit present
- PASS: bot PR, auto-merge armed, explicit body (even with non-bot commit)
- PASS: bot PR, auto-merge not armed
- PASS: human PR (not a bot)
- PASS: bot PR, null body, but only bot commits on the branch
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from scripts.lib import git_env

SCRIPT = Path(__file__).resolve().parent.parent / "check-bot-squash-body.py"
GIT = "git"


BOT_EMAIL = "29139614+renovate[bot]@users.noreply.github.com"
BOT_NAME = "renovate[bot]"
HUMAN_EMAIL = "t@example.invalid"
HUMAN_NAME = "t"


def _fixture_env(
    author_name: str, author_email: str, committer_name: str, committer_email: str
) -> dict[str, str]:
    return git_env.drop() | {
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_SYSTEM": os.devnull,
        "GIT_AUTHOR_NAME": author_name,
        "GIT_AUTHOR_EMAIL": author_email,
        "GIT_COMMITTER_NAME": committer_name,
        "GIT_COMMITTER_EMAIL": committer_email,
    }


def git(
    repo: Path,
    *args: str,
    check: bool = True,
    author_name: str = BOT_NAME,
    author_email: str = BOT_EMAIL,
    committer_name: str = BOT_NAME,
    committer_email: str = BOT_EMAIL,
) -> str:
    env = _fixture_env(author_name, author_email, committer_name, committer_email)
    result = subprocess.run(  # noqa: S603 - fixed argv, no shell
        [GIT, "-c", "commit.gpgsign=false", *args],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr}")
    return result.stdout.strip()


def make_repo(tmp: Path) -> Path:
    """Create a test repository with a bot commit on master."""
    repo = tmp / "repo"
    repo.mkdir()
    git(repo, "init", "-b", "master")
    (repo / "a.txt").write_text("bot initial\n")
    git(repo, "add", "a.txt")
    git(
        repo,
        "commit",
        "-m",
        f"chore(deps): initial bot commit\n\nSigned-off-by: {BOT_NAME} <{BOT_EMAIL}>",
    )
    return repo


def add_bot_commit(repo: Path) -> None:
    (repo / "b.txt").write_text("bot fix\n")
    git(repo, "add", "b.txt")
    git(repo, "commit", "-m", "chore(deps): bot fix")


def add_human_commit(repo: Path) -> None:
    (repo / "c.txt").write_text("human fix\n")
    git(
        repo,
        "add",
        "c.txt",
        author_name=HUMAN_NAME,
        author_email=HUMAN_EMAIL,
        committer_name=HUMAN_NAME,
        committer_email=HUMAN_EMAIL,
    )
    git(
        repo,
        "commit",
        "-m",
        f"fix(ci): human fix on bot branch\n\nSigned-off-by: {HUMAN_NAME} <{HUMAN_EMAIL}>",
        author_name=HUMAN_NAME,
        author_email=HUMAN_EMAIL,
        committer_name=HUMAN_NAME,
        committer_email=HUMAN_EMAIL,
    )


def run_check(repo: Path, base: str, head: str, **env_overrides: str) -> int:
    env = git_env.drop() | {
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_SYSTEM": os.devnull,
        "PR_AUTHOR": "renovate[bot]",
        "PR_AUTHOR_TYPE": "Bot",
        "BASE_SHA": base,
        "HEAD_SHA": head,
        "AUTO_MERGE_ARMED": "true",
        "AUTO_MERGE_BODY": "",
        **env_overrides,
    }
    result = subprocess.run(  # noqa: S603 - fixed argv, no shell
        [sys.executable, str(SCRIPT)],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )
    return result.returncode


def test_refuse_bot_null_body_human_commit(tmp_path: Path) -> None:
    """PLANTED CASE: the exact recurrence path that landed bot Co-authored-by on master."""
    repo = make_repo(tmp_path)
    base = git(repo, "rev-parse", "HEAD")
    add_human_commit(repo)
    head = git(repo, "rev-parse", "HEAD")
    rc = run_check(repo, base, head)
    assert rc == 1, f"expected refusal (1), got {rc}"
    print("  PASS: refuses bot PR + null body + human commit")


def test_pass_explicit_body(tmp_path: Path) -> None:
    """Explicit commit body → safe, even with a human commit."""
    repo = make_repo(tmp_path)
    base = git(repo, "rev-parse", "HEAD")
    add_human_commit(repo)
    head = git(repo, "rev-parse", "HEAD")
    rc = run_check(
        repo, base, head, AUTO_MERGE_BODY="chore(deps): explicit body\n\nSigned-off-by: ..."
    )
    assert rc == 0, f"expected pass (0), got {rc}"
    print("  PASS: allows explicit commit body")


def test_pass_no_auto_merge(tmp_path: Path) -> None:
    """Auto-merge not armed → safe."""
    repo = make_repo(tmp_path)
    base = git(repo, "rev-parse", "HEAD")
    add_human_commit(repo)
    head = git(repo, "rev-parse", "HEAD")
    rc = run_check(repo, base, head, AUTO_MERGE_ARMED="false")
    assert rc == 0, f"expected pass (0), got {rc}"
    print("  PASS: allows when auto-merge not armed")


def test_pass_human_pr(tmp_path: Path) -> None:
    """Human-opened PR → not a bot PR, safe."""
    repo = make_repo(tmp_path)
    base = git(repo, "rev-parse", "HEAD")
    add_human_commit(repo)
    head = git(repo, "rev-parse", "HEAD")
    rc = run_check(repo, base, head, PR_AUTHOR="lusoris", PR_AUTHOR_TYPE="User")
    assert rc == 0, f"expected pass (0), got {rc}"
    print("  PASS: allows human-opened PR")


def test_pass_only_bot_commits(tmp_path: Path) -> None:
    """Bot PR with only bot commits → safe (default body lists only the bot)."""
    repo = make_repo(tmp_path)
    base = git(repo, "rev-parse", "HEAD")
    add_bot_commit(repo)
    head = git(repo, "rev-parse", "HEAD")
    rc = run_check(repo, base, head)
    assert rc == 0, f"expected pass (0), got {rc}"
    print("  PASS: allows bot-only commits")


def main() -> int:
    failures = 0
    tests = [
        test_refuse_bot_null_body_human_commit,
        test_pass_explicit_body,
        test_pass_no_auto_merge,
        test_pass_human_pr,
        test_pass_only_bot_commits,
    ]
    for test in tests:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                test(Path(tmp))
            except Exception as exc:
                print(f"  FAIL: {test.__name__}: {exc}")
                failures += 1
    print(f"\n{len(tests) - failures}/{len(tests)} passed, {failures} failed.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
