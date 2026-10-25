#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the scheduled render pull request opener (ADR-3373).

Verifies author and cross-repository filtering for open PRs, auto-merge arming
on parent==base early exit, detection of untracked files in rendered paths,
and required environment validation.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "ci" / "open-render-pr.sh"
GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"

MIN_CALL_LEN = 2
TEST_TOKEN = "test-token"  # noqa: S105 -- dummy token for fixture

GH_STUB = r"""#!/usr/bin/env python3
import json
import os
import subprocess
import sys
from pathlib import Path

state = Path(os.environ["STUB_STATE_DIR"])
log = state / "gh.log"
with log.open("a", encoding="utf-8") as f:
    f.write(json.dumps(sys.argv[1:]) + "\n")

argv = sys.argv[1:]
if not argv:
    sys.exit(0)

def eval_jq(data, expr):
    res = subprocess.run(
        ["jq", "-r", expr],
        input=json.dumps(data),
        text=True,
        capture_output=True,
        check=True,
    )
    sys.stdout.write(res.stdout)

cmd = argv[0]
if cmd == "pr":
    sub = argv[1] if len(argv) > 1 else ""
    if sub == "list":
        prs_path = state / "prs.json"
        data = json.loads(prs_path.read_text(encoding="utf-8")) if prs_path.exists() else []
        if "--jq" in argv:
            eval_jq(data, argv[argv.index("--jq") + 1])
        else:
            print(json.dumps(data))
    elif sub == "create":
        after_path = state / "prs_after_create.json"
        if after_path.exists():
            (state / "prs.json").write_text(after_path.read_text(encoding="utf-8"), encoding="utf-8")
    sys.exit(0)

if cmd == "api":
    endpoint = argv[1] if len(argv) > 1 else ""
    if "branches" in endpoint:
        branch_path = state / "branch.json"
        data = (
            json.loads(branch_path.read_text(encoding="utf-8"))
            if branch_path.exists()
            else {"commit": {"parents": [{"sha": "none"}]}}
        )
        if "--jq" in argv:
            eval_jq(data, argv[argv.index("--jq") + 1])
        else:
            print(json.dumps(data))
    elif "users" in endpoint:
        data = {"id": 12345678}
        if "--jq" in argv:
            eval_jq(data, argv[argv.index("--jq") + 1])
        else:
            print(json.dumps(data))
    sys.exit(0)

sys.exit(0)
"""

MAKE_STUB = r"""#!/usr/bin/env python3
import json
import os
import runpy
import sys
from pathlib import Path

state = Path(os.environ["STUB_STATE_DIR"])
log = state / "make.log"
with log.open("a", encoding="utf-8") as f:
    f.write(json.dumps(sys.argv[1:]) + "\n")

action = state / "make_action.py"
if action.exists() and "docs-render" in sys.argv:
    runpy.run_path(str(action))

sys.exit(0)
"""


RENDERED_FILES = (
    "CHANGELOG.md",
    "docs/adr/README.md",
    "docs/adr/titles.md",
    "docs/research/titles.md",
    "docs/rebase-notes.md",
)


def fixture_env() -> dict[str, str]:
    """Environment for fixture git calls, without the caller's GIT_* variables."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(
        GIT_CONFIG_GLOBAL="/dev/null",
        GIT_CONFIG_SYSTEM="/dev/null",
        GIT_AUTHOR_NAME="Test",
        GIT_AUTHOR_EMAIL="t@example.com",
        GIT_COMMITTER_NAME="Test",
        GIT_COMMITTER_EMAIL="t@example.com",
    )
    return env


def git(*args: str) -> str:
    return subprocess.run(  # noqa: S603 -- fixed argv, fixture repo
        [GIT, *args], check=True, env=fixture_env(), capture_output=True, text=True
    ).stdout


class OpenRenderPrTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.state_dir = self.root / "state"
        self.state_dir.mkdir()
        self.bin_dir = self.root / "bin"
        self.bin_dir.mkdir()
        self.runner_temp = self.root / "runner_temp"
        self.runner_temp.mkdir()
        self.app_slug = "vmafx-release-bot"
        self.token = TEST_TOKEN
        self.github_repository = "VMAFx/vmafx"
        self.render_branch = "render-docs"
        self.push_url = (
            f"https://x-access-token:{self.token}@github.com/{self.github_repository}.git"
        )
        self._write_stubs()
        self._init_repos()
        self._write_git_config()

    def _write_stubs(self) -> None:
        for name, text in (("gh", GH_STUB), ("make", MAKE_STUB)):
            stub = self.bin_dir / name
            stub.write_text(text, encoding="utf-8")
            stub.chmod(0o755)

    def _init_repos(self) -> None:
        self.bare_repo = self.root / "bare.git"
        git("init", "--bare", "-q", str(self.bare_repo))
        self.repo = self.root / "repo"
        self.repo.mkdir()
        git("init", "-q", "-b", "master", str(self.repo))
        for rel in RENDERED_FILES:
            file_path = self.repo / rel
            file_path.parent.mkdir(parents=True, exist_ok=True)
            file_path.write_text("initial\n", encoding="utf-8")
        (self.repo / "docs/adr/by-tag").mkdir(parents=True, exist_ok=True)
        git("-C", str(self.repo), "add", "-A")
        git("-C", str(self.repo), "commit", "-q", "-m", "initial")
        git("-C", str(self.repo), "config", f"url.{self.bare_repo}.insteadOf", self.push_url)

    def _write_git_config(self) -> None:
        self.git_config = self.root / "gitconfig"
        self.git_config.write_text(
            f'[url "{self.bare_repo}"]\n'
            f"    insteadOf = {self.push_url}\n"
            f"[user]\n"
            f"    name = Test User\n"
            f"    email = test@example.com\n",
            encoding="utf-8",
        )

    def gh_calls(self) -> list[list[str]]:
        log = self.state_dir / "gh.log"
        if not log.exists():
            return []
        return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines() if line]

    def set_prs(self, prs: list[dict]) -> None:
        (self.state_dir / "prs.json").write_text(json.dumps(prs), encoding="utf-8")

    def run_script(
        self,
        render_due: bool = False,
        extra_env: dict[str, str] | None = None,
        remove_env: tuple[str, ...] = (),
    ) -> subprocess.CompletedProcess[str]:
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env.update(
            PATH=f"{self.bin_dir}:{env.get('PATH', '')}",
            STUB_STATE_DIR=str(self.state_dir),
            GH_TOKEN=self.token,
            APP_SLUG=self.app_slug,
            GITHUB_REPOSITORY=self.github_repository,
            RENDER_BRANCH=self.render_branch,
            RENDER_DUE="true" if render_due else "false",
            RUNNER_TEMP=str(self.runner_temp),
            OWED_SINCE="2026-10-25T00:00:00Z",
            GIT_CONFIG=str(self.git_config),
            GIT_CONFIG_GLOBAL=str(self.git_config),
            GIT_AUTHOR_NAME="Test User",
            GIT_AUTHOR_EMAIL="test@example.com",
            GIT_COMMITTER_NAME="Test User",
            GIT_COMMITTER_EMAIL="test@example.com",
        )
        if extra_env:
            env.update(extra_env)
        for key in remove_env:
            env.pop(key, None)

        return subprocess.run(  # noqa: S603 -- fixed argv, fixture repo
            [BASH, str(SCRIPT)],
            cwd=self.repo,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_case_a_foreign_pr_on_render_branch_not_closed_when_render_not_due(self) -> None:
        # Case a: foreign PR (author other login) on RENDER_BRANCH with RENDER_DUE=false -> no gh pr close call.
        # Unfixed failure: gh pr list has no author filter, so open_pr takes the foreign PR number (99)
        # and executes 'gh pr close --delete-branch 99', closing an unrelated pull request.
        self.set_prs([{"number": 99, "author": {"login": "attacker"}, "isCrossRepository": False}])
        res = self.run_script(render_due=False)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        calls = self.gh_calls()
        close_calls = [
            c for c in calls if len(c) >= MIN_CALL_LEN and c[0] == "pr" and c[1] == "close"
        ]
        self.assertEqual(close_calls, [], f"foreign PR was closed: {close_calls}")

    def test_case_b_cross_repo_pr_by_bot_not_closed_nor_merged(self) -> None:
        # Case b: cross-repo PR by the bot -> not closed, not merged.
        # Unfixed failure: gh pr list does not filter by isCrossRepository == false.
        # With RENDER_DUE=false, the unfixed script calls 'gh pr close 99' on the fork PR.
        # With RENDER_DUE=true and parent != base, the unfixed script executes 'gh pr merge 99' on the fork PR.
        self.set_prs(
            [
                {
                    "number": 99,
                    "author": {"login": f"{self.app_slug}[bot]"},
                    "isCrossRepository": True,
                }
            ]
        )

        # Sub-check 1: RENDER_DUE=false -> not closed
        res_close = self.run_script(render_due=False)
        self.assertEqual(res_close.returncode, 0, res_close.stdout + res_close.stderr)
        close_calls = [
            c
            for c in self.gh_calls()
            if len(c) >= MIN_CALL_LEN and c[0] == "pr" and c[1] == "close"
        ]
        self.assertEqual(close_calls, [], f"cross-repo PR was closed: {close_calls}")

        # Sub-check 2: RENDER_DUE=true -> not merged
        (self.state_dir / "gh.log").unlink(missing_ok=True)
        # parent commit differs from base
        (self.state_dir / "branch.json").write_text(
            json.dumps({"commit": {"parents": [{"sha": "different-sha"}]}}),
            encoding="utf-8",
        )
        res_merge = self.run_script(render_due=True)
        self.assertEqual(res_merge.returncode, 0, res_merge.stdout + res_merge.stderr)
        merge_calls = [
            c
            for c in self.gh_calls()
            if len(c) >= MIN_CALL_LEN and c[0] == "pr" and c[1] == "merge" and "99" in c
        ]
        self.assertEqual(merge_calls, [], f"cross-repo PR was merged: {merge_calls}")

    def test_case_c_own_pr_parent_equals_base_arms_automerge_without_push(self) -> None:
        # Case c: own PR, parent==base -> gh pr merge --auto called, no push.
        # Unfixed failure: early exit when parent == base skips 'gh pr merge --auto', exiting 0
        # without arming auto-merge.
        head_sha = git("-C", str(self.repo), "rev-parse", "HEAD").strip()
        (self.state_dir / "branch.json").write_text(
            json.dumps({"commit": {"parents": [{"sha": head_sha}]}}),
            encoding="utf-8",
        )
        self.set_prs(
            [
                {
                    "number": 42,
                    "author": {"login": f"{self.app_slug}[bot]"},
                    "isCrossRepository": False,
                }
            ]
        )

        res = self.run_script(render_due=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("already rendered", res.stdout)

        calls = self.gh_calls()
        merge_auto_calls = [
            c
            for c in calls
            if len(c) >= MIN_CALL_LEN
            and c[0] == "pr"
            and c[1] == "merge"
            and "--auto" in c
            and "42" in c
        ]
        self.assertTrue(merge_auto_calls, f"gh pr merge --auto was not called: {calls}")

        bare_branches = git("-C", str(self.bare_repo), "branch")
        self.assertNotIn(self.render_branch, bare_branches)

    def test_case_d_untracked_adr_tag_file_triggers_commit_and_push(self) -> None:
        # Case d: make docs-render creates only an untracked docs/adr/by-tag/x.md -> commit + push path taken.
        # Unfixed failure: 'git diff --quiet -- ...' ignores untracked files, so the unfixed script
        # prints 'make docs-render changed nothing' and exits 0 without committing or pushing.
        self.set_prs([])
        (self.state_dir / "prs_after_create.json").write_text(
            json.dumps(
                [
                    {
                        "number": 42,
                        "author": {"login": f"{self.app_slug}[bot]"},
                        "isCrossRepository": False,
                    }
                ]
            ),
            encoding="utf-8",
        )
        (self.state_dir / "make_action.py").write_text(
            "from pathlib import Path\n"
            "p = Path('docs/adr/by-tag/x.md')\n"
            "p.parent.mkdir(parents=True, exist_ok=True)\n"
            "p.write_text('# Tag x\\n', encoding='utf-8')\n",
            encoding="utf-8",
        )

        res = self.run_script(render_due=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("auto-merge armed", res.stdout)

        bare_branches = git("-C", str(self.bare_repo), "branch")
        self.assertIn(self.render_branch, bare_branches)

    def test_case_e_runner_temp_unset_exits_nonzero_before_gh_calls(self) -> None:
        # Case e: RUNNER_TEMP unset -> script exits non-zero before any gh call.
        # Unfixed failure: RUNNER_TEMP is omitted from the initial ': "${...:?}"' check,
        # so the unfixed script executes 'gh pr list' before failing later.
        self.set_prs([])
        res = self.run_script(render_due=False, remove_env=("RUNNER_TEMP",))
        self.assertNotEqual(res.returncode, 0, "script should fail when RUNNER_TEMP is unset")
        calls = self.gh_calls()
        self.assertEqual(calls, [], f"gh was called despite missing RUNNER_TEMP: {calls}")


if __name__ == "__main__":
    unittest.main()
