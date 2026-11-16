#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""PR bot-opener exemption coverage for the Intel NEO lock refresh commit (ADR-3555)."""

from __future__ import annotations

import datetime as dt
import json
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci.tests import check_dco_fixture as fx  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult, run  # noqa: E402

LOCK_PATH = "dev/scripts/intel-neo-lock.json"
RULE = "dco-bot-opener"
TODAY = "2026-11-16"
EXPIRES = "2027-03-16"
RELEASE_BOT = (
    "vmafx-release-bot[bot]",
    "340976456+vmafx-release-bot[bot]@users.noreply.github.com",
)
ACTIONS_BOT = (
    "github-actions[bot]",
    "41898282+github-actions[bot]@users.noreply.github.com",
)
DEPENDABOT = (
    "dependabot[bot]",
    "49699333+dependabot[bot]@users.noreply.github.com",
)


def write_exception_list(
    exc_root: Path, paths: tuple[str, ...] = (LOCK_PATH,), expires: str = EXPIRES
) -> None:
    """Write a temporary dco-bot-opener exception list under exc_root."""
    target = exc_root / ".config" / "lint-exceptions.d" / f"{RULE}.toml"
    target.parent.mkdir(parents=True, exist_ok=True)
    blocks = [
        f'[[exception]]\npath = "{path}"\nreason = "fixture"\nexpires = {expires}\n'
        for path in paths
    ]
    target.write_text("\n".join(blocks), encoding="utf-8")


def commit_paths(
    repo: fx.Repo,
    paths: tuple[str, ...],
    ident: tuple[str, str],
    *,
    message: str = "chore(deps): refresh Intel NEO and Level Zero asset lock",
    signoff: bool = True,
    committer: tuple[str, str] | None = None,
) -> str:
    """Commit exact repository-relative paths (or --allow-empty when paths is empty)."""
    parent = repo.git(["rev-parse", "--short", "HEAD"], ident).strip()
    for index, rel in enumerate(paths):
        full = repo.path / rel
        full.parent.mkdir(parents=True, exist_ok=True)
        full.write_text(f"{parent}:{index}:{message}\n", encoding="utf-8")
    if paths:
        repo.git(["add", "--", *paths], ident)
    flags = ["-s"] if signoff else []
    empty = ["--allow-empty"] if not paths else []
    repo.git(["commit", "-q", *empty, *flags, "-F", "-"], ident, committer, stdin=message)
    return repo.git(["rev-parse", "HEAD"], ident).strip()


def run_gate(
    repo: fx.Repo,
    *args: str,
    exc_root: Path | None = None,
    today: str = TODAY,
    include_range: bool = True,
) -> TextCommandResult:
    """Invoke check-dco.py with isolated environment and optional temp exception root."""
    env = fx.clean_env()
    env["LINT_EXCEPTIONS_TODAY"] = today
    cmd = [sys.executable, "-I", str(fx.GATE)]
    if include_range:
        head = repo.git(["rev-parse", "HEAD"]).strip()
        cmd += ["--base", repo.base, "--head", head]
    if exc_root is not None:
        cmd += ["--exceptions-root", str(exc_root)]
    cmd.extend(args)
    return run(
        cmd,
        allowed_executables=(sys.executable,),
        cwd=repo.path,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=fx.SCRIPT_TIMEOUT_S,
    )


class DcoLockRefreshExemptionTest(unittest.TestCase):
    """Positive, planted negative, and boundary cases for dco-bot-opener in check-dco.py."""

    def setUp(self) -> None:
        """Create an isolated repo and a temporary lint-exceptions root."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        base = Path(tmp.name)
        (base / "repo").mkdir()
        self.repo = fx.Repo(base / "repo")
        self.exc_root = base / "exc"
        write_exception_list(self.exc_root)

    def test_positive_renovate_pr_release_bot_lock_only_passes(self) -> None:
        """Renovate Bot PR + signed release-bot lock-only commit + live exception passes."""
        commit_paths(self.repo, ("build-config.env",), fx.BOT_IDENT)
        commit_paths(self.repo, (LOCK_PATH,), RELEASE_BOT)
        for day in (TODAY, EXPIRES):
            with self.subTest(today=day):
                res = run_gate(
                    self.repo,
                    "--author",
                    fx.BOT_LOGIN,
                    "--author-type",
                    "Bot",
                    exc_root=self.exc_root,
                    today=day,
                )
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
                self.assertIn("PASS - 2 commit(s)", res.stdout)

    def test_positive_normalized_release_bot_and_real_repo_exception_pass(self) -> None:
        """Normalized release-bot identity and the tracked exception list both pass."""
        ident = ("VMAFx-Release-Bot[BOT]\u200b", RELEASE_BOT[1].upper())
        commit_paths(self.repo, (LOCK_PATH,), ident, committer=RELEASE_BOT)
        res = run_gate(self.repo, "--author", fx.BOT_LOGIN, "--author-type", "Bot")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_planted_refusal_another_file(self) -> None:
        """Adding a second path or touching only an unexcepted path is refused."""
        for paths in ((LOCK_PATH, "other.txt"), ("other.txt",)):
            with self.subTest(paths=paths), tempfile.TemporaryDirectory() as directory:
                repo = fx.Repo(Path(directory))
                sha = commit_paths(repo, paths, RELEASE_BOT)
                res = run_gate(
                    repo,
                    "--author",
                    fx.BOT_LOGIN,
                    "--author-type",
                    "Bot",
                    exc_root=self.exc_root,
                )
                self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
                self.assertIn(sha[:10], res.stdout)
                self.assertIn("is not the branch opener", res.stdout)

    def test_planted_refusal_another_bot(self) -> None:
        """Another bot login, another email, or another bot name on the lock path fails."""
        cases = (
            ACTIONS_BOT,
            (RELEASE_BOT[0], "1+vmafx-release-bot[bot]@users.noreply.github.com"),
            ("other[bot]", RELEASE_BOT[1]),
        )
        for ident in cases:
            with self.subTest(ident=ident), tempfile.TemporaryDirectory() as directory:
                repo = fx.Repo(Path(directory))
                sha = commit_paths(repo, (LOCK_PATH,), ident)
                res = run_gate(
                    repo,
                    "--author",
                    fx.BOT_LOGIN,
                    "--author-type",
                    "Bot",
                    exc_root=self.exc_root,
                )
                self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
                self.assertIn(sha[:10], res.stdout)
                self.assertIn("is not the branch opener", res.stdout)

    def test_planted_refusal_another_opener(self) -> None:
        """Dependabot and human openers refuse the identical release-bot lock commit."""
        sha = commit_paths(self.repo, (LOCK_PATH,), RELEASE_BOT)
        openers = (
            (DEPENDABOT[0], "Bot"),
            ("lusoris", "User"),
            (fx.BOT_LOGIN, "User"),
        )
        for author, author_type in openers:
            with self.subTest(author=author, author_type=author_type):
                res = run_gate(
                    self.repo,
                    "--author",
                    author,
                    "--author-type",
                    author_type,
                    exc_root=self.exc_root,
                )
                self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
                self.assertIn(sha[:10], res.stdout)
                self.assertIn("is not the branch opener", res.stdout)

    def test_boundary_expired_exception_refused(self) -> None:
        """An exception whose expires date is strictly before today is refused."""
        write_exception_list(self.exc_root, expires="2026-11-15")
        sha = commit_paths(self.repo, (LOCK_PATH,), RELEASE_BOT)
        res = run_gate(
            self.repo,
            "--author",
            fx.BOT_LOGIN,
            "--author-type",
            "Bot",
            exc_root=self.exc_root,
            today=TODAY,
        )
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn(sha[:10], res.stdout)
        self.assertIn("is not the branch opener", res.stdout)

    def test_boundary_empty_commit_refused(self) -> None:
        """An empty commit touches zero paths and is refused despite a live exception."""
        sha = commit_paths(self.repo, (), RELEASE_BOT)
        res = run_gate(
            self.repo,
            "--author",
            fx.BOT_LOGIN,
            "--author-type",
            "Bot",
            exc_root=self.exc_root,
        )
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn(sha[:10], res.stdout)
        self.assertIn("is not the branch opener", res.stdout)

    def test_boundary_missing_signoff_still_refused(self) -> None:
        """Lifting the opener refusal leaves the Signed-off-by requirement intact."""
        sha = commit_paths(self.repo, (LOCK_PATH,), RELEASE_BOT, signoff=False)
        res = run_gate(
            self.repo,
            "--author",
            fx.BOT_LOGIN,
            "--author-type",
            "Bot",
            exc_root=self.exc_root,
        )
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn(sha[:10], res.stdout)
        self.assertIn("no 'Signed-off-by: Name <email>' trailer", res.stdout)
        self.assertNotIn("is not the branch opener", res.stdout)

    def test_boundary_commit_attribution_still_refused(self) -> None:
        """Commit attribution checks still reject tool trailers and generated lines."""
        for extra, expected in (
            ("Co-authored-by: Codex <x@y>", "disallowed Co-authored-by"),
            ("Generated by assistant", "machine-generated attribution"),
        ):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as directory:
                repo = fx.Repo(Path(directory))
                commit_paths(repo, (LOCK_PATH,), RELEASE_BOT, message=f"chore: lock\n\n{extra}")
                res = run_gate(
                    repo,
                    "--author",
                    fx.BOT_LOGIN,
                    "--author-type",
                    "Bot",
                    exc_root=self.exc_root,
                )
                self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
                self.assertIn(expected, res.stdout)

    def test_boundary_paths_output_bound_refused(self) -> None:
        """Oversized diff-tree output fails closed with MAX_COMMIT_PATHS_BYTES."""
        mod = fx.load_gate("check_dco_lock_bounds")
        self.addCleanup(sys.modules.pop, "check_dco_lock_bounds", None)
        chunk = "p" * 120 + ".txt"
        count = mod.MAX_COMMIT_PATHS_BYTES // (len(chunk) + 1) + 2
        paths = tuple(f"d{idx}/{chunk}" for idx in range(count))
        commit_paths(self.repo, paths, RELEASE_BOT)
        res = run_gate(
            self.repo,
            "--author",
            fx.BOT_LOGIN,
            "--author-type",
            "Bot",
            exc_root=self.exc_root,
        )
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("MAX_COMMIT_PATHS_BYTES", res.stderr)

    def test_landed_and_pre_push_modes_unchanged(self) -> None:
        """Landed and pre-push modes keep their exact prior behavior on the lock commit."""
        self.repo.git(["checkout", "-q", "-b", "renovate/intel-neo"])
        commit_paths(self.repo, ("build-config.env",), fx.BOT_IDENT)
        sha = commit_paths(self.repo, (LOCK_PATH,), RELEASE_BOT)
        landed = run_gate(
            self.repo,
            "--landed",
            "--landing-identity",
            fx.LANDING_ID,
            exc_root=self.exc_root,
        )
        self.assertEqual(landed.returncode, 0, landed.stdout + landed.stderr)
        pre_push = run_gate(
            self.repo,
            "--merge-base",
            "main",
            "--author",
            fx.BOT_LOGIN,
            "--author-type",
            "Bot",
            exc_root=self.exc_root,
            include_range=False,
        )
        self.assertEqual(pre_push.returncode, 1, pre_push.stdout + pre_push.stderr)
        self.assertIn(sha[:10], pre_push.stdout)
        self.assertIn("is not the branch opener", pre_push.stdout)

        commit_paths(self.repo, (LOCK_PATH,), RELEASE_BOT, signoff=False)
        unsigned_landed = run_gate(
            self.repo, "--landed", "--landing-identity", fx.LANDING_ID, exc_root=self.exc_root
        )
        self.assertEqual(unsigned_landed.returncode, 1)
        self.assertIn("Signed-off-by", unsigned_landed.stdout)

    def test_repository_wiring_matches_exemption_contract(self) -> None:
        """Checked-in exception list, renovate.json, and workflow agree on identity and path."""
        toml_path = fx.ROOT / ".config" / "lint-exceptions.d" / f"{RULE}.toml"
        entries = tomllib.loads(toml_path.read_text(encoding="utf-8"))["exception"]
        self.assertEqual(len(entries), 1)
        entry = entries[0]
        self.assertEqual(entry["path"], LOCK_PATH)
        self.assertEqual(entry["category"], "D")
        self.assertEqual(entry["entered"], dt.date.fromisoformat(TODAY))
        self.assertEqual(entry["expires"], dt.date.fromisoformat(EXPIRES))
        self.assertTrue(entry["reason"].startswith("Category D, 120-day interval."))
        for token in (fx.BOT_LOGIN, RELEASE_BOT[0], RELEASE_BOT[1], "intel-neo-lock-refresh.yml"):
            self.assertIn(token, entry["reason"])

        renovate = json.loads((fx.ROOT / "renovate.json").read_text(encoding="utf-8"))
        self.assertEqual(renovate.get("gitIgnoredAuthors"), [RELEASE_BOT[1]])

        workflow = (fx.ROOT / ".github/workflows/intel-neo-lock-refresh.yml").read_text(
            encoding="utf-8"
        )
        for snippet in (
            "permissions:\n  contents: read",
            f'bot="{RELEASE_BOT[0]}"',
            f'email="{RELEASE_BOT[1]}"',
            "Signed-off-by: %s <%s>",
            'git add -- "$lock"',
            '--author="$bot <$email>" -F "$msg" -- "$lock"',
        ):
            self.assertIn(snippet, workflow)


if __name__ == "__main__":
    unittest.main()
