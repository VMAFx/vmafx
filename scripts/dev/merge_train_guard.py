#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Inspect and guard local VMAFx merge-train actions (ADR-1244).

Every remote mutation requires --apply. Local gate receipts are produced only
by executing make lint and make test, never by accepting a pass flag.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator

REPO = "VMAFx/vmafx"
RELEASE_PR = 1213
KEY_BYTES = 32
INVENTORY_LIMIT = 100
PR_FIELDS = (
    "number,state,isDraft,baseRefName,headRefName,headRefOid,"
    "isCrossRepository,autoMergeRequest,mergeStateStatus"
)
GATES = (("make", "lint"), ("make", "test"))


class Refused(RuntimeError):
    """The action has no safe, current authorization."""


def encoded(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def environment() -> dict[str, str]:
    """Do not inherit Git redirection or make dry-run/alternate-file flags."""
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("GIT_")
        and key
        not in {"MAKEFLAGS", "MFLAGS", "MAKELEVEL", "GNUMAKEFLAGS", "MAKEFILES", "MAKEOVERRIDES"}
    }


def execute(argv: list[str], cwd: Path) -> str:
    """Run fixed argument vectors; failures never become success-shaped output."""
    binary = shutil.which(argv[0])
    if binary is None:
        raise Refused(f"required executable is unavailable: {argv[0]}")
    result = subprocess.run(  # noqa: S603 -- argument vector, no shell
        [binary, *argv[1:]], cwd=cwd, env=environment(), capture_output=True, text=True
    )
    if result.returncode:
        raise Refused(f"{' '.join(argv[:2])} failed ({result.returncode}): {result.stderr.strip()}")
    return result.stdout


class Train:
    def __init__(self, root: Path, state: Path) -> None:
        self.root = root.resolve()
        self.state = state.resolve()

    def git(self, *args: str, cwd: Path | None = None) -> str:
        return execute(["git", "-C", str(cwd or self.root), *args], self.root).strip()

    def gh(self, *args: str) -> str:
        return execute(["gh", *args, "--repo", f"github.com/{REPO}"], self.root)

    def pr(self, number: int) -> dict[str, Any]:
        value = json.loads(self.gh("pr", "view", str(number), "--json", PR_FIELDS))
        if not isinstance(value, dict) or value.get("number") != number:
            raise Refused("GitHub returned incomplete or mismatched PR metadata")
        return value

    def policy(self) -> dict[str, Any]:
        value = json.loads((self.state / "policy.json").read_text())
        if not isinstance(value, dict) or value.get("schema") != 1:
            raise Refused("missing supported train policy")
        for key in ("protected_branches", "protected_worktrees"):
            if not isinstance(value.get(key), list) or not all(
                isinstance(item, str) for item in value[key]
            ):
                raise Refused(f"invalid train policy: {key}")
        return value

    def held(self) -> set[int]:
        result = {RELEASE_PR}
        for raw in (self.state / "hold.txt").read_text().splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            token = line.split()[0]
            if not token.isdecimal() or int(token) <= 0:
                raise Refused("invalid hold entry; refusing mutations")
            result.add(int(token))
        return result

    def worktrees(self) -> list[dict[str, str]]:
        records: list[dict[str, str]] = []
        record: dict[str, str] = {}
        for field in self.git("worktree", "list", "--porcelain", "-z").split("\0"):
            if not field:
                if record:
                    records.append(record)
                    record = {}
                continue
            key, _, value = field.partition(" ")
            record[key] = value
        if record:
            records.append(record)
        return records

    def guard(self, number: int, expected: str | None = None) -> dict[str, Any]:
        policy = self.policy()
        if (self.state / "PAUSED").exists():
            raise Refused("train is paused")
        if number in self.held():
            raise Refused(f"PR #{number} is held")
        pr = self.pr(number)
        if pr.get("state") != "OPEN" or pr.get("baseRefName") != "master":
            raise Refused("only open PRs targeting master are eligible")
        if pr.get("isCrossRepository") is not False:
            raise Refused("cross-repository PRs require an explicit owner")
        if not isinstance(pr.get("isDraft"), bool):
            raise Refused("PR draft state is unavailable")
        head = pr.get("headRefOid", "")
        if not isinstance(head, str) or re.fullmatch(r"[0-9a-f]{40}", head) is None:
            raise Refused("PR head is unavailable")
        if expected is not None and head != expected:
            raise Refused("PR head changed; repeat inspection and validation")
        branch = pr.get("headRefName", "")
        if not isinstance(branch, str) or not branch or branch == "master":
            raise Refused("invalid PR branch")
        self.git("check-ref-format", "--branch", branch)
        if branch in policy["protected_branches"]:
            raise Refused("PR branch belongs to a protected owner")
        protected = {Path(path).resolve() for path in policy["protected_worktrees"]}
        for worktree in self.worktrees():
            path = Path(worktree["worktree"]).resolve()
            owns_branch = worktree.get("branch") == f"refs/heads/{branch}"
            owns_head = worktree.get("HEAD") == head and path.name.startswith("agent-")
            if owns_branch or owns_head:
                raise Refused(f"PR source is checked out by another owner: {path}")
            if path in protected and worktree.get("HEAD") == head:
                raise Refused(f"PR head belongs to protected worktree: {path}")
        return pr

    @contextmanager
    def mutation_lock(self) -> Iterator[None]:
        """Serialize cooperating actors; root must retire old unrestricted actors."""
        self.state.mkdir(parents=True, exist_ok=True)
        with (self.state / "mutation.lock").open("a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise Refused("another train action owns the mutation lock") from exc
            try:
                yield
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def signing_key(self, create: bool = False) -> bytes:
        path = self.state / "validation-key"
        if create and not path.exists():
            try:
                descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            except FileExistsError:
                pass
            else:
                with os.fdopen(descriptor, "wb") as stream:
                    stream.write(secrets.token_bytes(KEY_BYTES))
        mode = path.lstat().st_mode
        if not stat.S_ISREG(mode) or mode & 0o077:
            raise Refused("validation key must be a private regular file")
        key = path.read_bytes()
        if len(key) != KEY_BYTES:
            raise Refused("invalid validation key")
        return key

    def clean_head(self, checkout: Path, head: str) -> None:
        if self.git("rev-parse", "HEAD", cwd=checkout) != head:
            raise Refused("validation checkout does not match the PR head")
        if self.git("status", "--porcelain=v1", "--untracked-files=all", cwd=checkout):
            raise Refused("validation source has tracked or untracked changes")
        for name in ("GNUmakefile", "makefile", "Makefile"):
            if (checkout / name).exists():
                # Git status excludes ignored files; an ignored GNUmakefile can
                # shadow a tracked Makefile and turn both gates into no-ops.
                self.git("ls-files", "--error-unmatch", name, cwd=checkout)

    def validate(self, number: int, checkout: Path) -> None:
        """Explicit local validation may proceed while the PR is held/paused."""
        pr = self.pr(number)
        if number == RELEASE_PR or pr.get("state") != "OPEN" or pr.get("baseRefName") != "master":
            raise Refused("validation requires an open master-target PR other than #1213")
        checkout = checkout.resolve(strict=True)
        head = pr["headRefOid"]
        self.clean_head(checkout, head)
        self.state.mkdir(parents=True, exist_ok=True)
        logs = Path(tempfile.mkdtemp(prefix="validation-", dir=self.state))
        target = self.state / f"validated-{head}.json"
        if target.exists():
            # A new failed run must not leave an earlier passing receipt usable.
            target.rename(logs / "previous-receipt.json")
        results = []
        make = shutil.which("make")
        if make is None:
            raise Refused("make is unavailable")
        for command in GATES:
            self.clean_head(checkout, head)
            log = logs / f"{command[1]}.log"
            with log.open("wb") as output:
                result = subprocess.run(  # noqa: S603 -- fixed full-gate command
                    [make, command[1]],
                    cwd=checkout,
                    env=environment(),
                    stdout=output,
                    stderr=subprocess.STDOUT,
                )
            if result.returncode:
                raise Refused(f"{' '.join(command)} failed ({result.returncode}); evidence: {log}")
            results.append({"command": list(command), "log": str(log), "sha256": digest(log)})
        self.clean_head(checkout, head)
        if self.pr(number)["headRefOid"] != head:
            raise Refused("PR head changed during validation")
        payload = {
            "schema": 1,
            "repository": REPO,
            "head": head,
            "tree": self.git("rev-parse", "HEAD^{tree}", cwd=checkout),
            "generator_sha256": digest(Path(__file__)),
            "results": results,
            "completed_at": datetime.now(timezone.utc).isoformat(),
        }
        signature = hmac.new(
            self.signing_key(create=True), encoded(payload), hashlib.sha256
        ).hexdigest()
        target.write_text(json.dumps({"payload": payload, "signature": signature}, indent=2) + "\n")
        print(f"Full make lint/test validation recorded for {head}: {target}")

    def require_validation(self, head: str) -> None:
        receipt = json.loads((self.state / f"validated-{head}.json").read_text())
        payload = receipt["payload"]
        signature = hmac.new(self.signing_key(), encoded(payload), hashlib.sha256).hexdigest()
        if not hmac.compare_digest(signature, receipt.get("signature", "")):
            raise Refused("validation receipt was not issued by the local gate runner")
        if (
            payload.get("schema") != 1
            or payload.get("repository") != REPO
            or payload.get("head") != head
            or payload.get("generator_sha256") != digest(Path(__file__))
        ):
            raise Refused("validation receipt does not match this head and gate runner")
        results = payload.get("results", [])
        if [item.get("command") for item in results] != [list(command) for command in GATES]:
            raise Refused("full make lint and make test evidence is missing")
        for item in results:
            log = Path(item["log"]).resolve(strict=True)
            if not log.is_relative_to(self.state) or digest(log) != item["sha256"]:
                raise Refused("validation log is missing, outside state, or changed")

    def require_checks(self, number: int) -> None:
        checks = json.loads(
            self.gh("pr", "checks", str(number), "--required", "--json", "name,bucket,state")
        )
        if not checks or not any(
            item.get("name") == "Required Checks Aggregator" for item in checks
        ):
            raise Refused("required checks are absent; zero checks is not green")
        if any(item.get("bucket") != "pass" for item in checks):
            raise Refused("required checks are not eligible for this action")

    def merge(self, number: int, auto: bool) -> None:
        pr = self.guard(number)
        if pr["isDraft"]:
            raise Refused("draft PRs cannot be armed or merged")
        self.require_validation(pr["headRefOid"])
        self.require_checks(number)
        self.guard(number, pr["headRefOid"])
        args = ["pr", "merge", str(number), "--squash", "--match-head-commit", pr["headRefOid"]]
        if auto:
            args.append("--auto")
        self.gh(*args)

    def rebase(self, number: int) -> str:
        pr = self.guard(number)
        head, branch = pr["headRefOid"], pr["headRefName"]
        runs = self.state / "worktrees"
        if any(runs.glob(f"pr-{number}-*/checkout")):
            raise Refused("a retained train checkout needs an explicit owner handoff before retry")
        canonical = {"https://github.com/VMAFx/vmafx.git", "git@github.com:VMAFx/vmafx.git"}
        for direction in ((), ("--push",)):
            urls = self.git("remote", "get-url", "--all", *direction, "origin").splitlines()
            if len(urls) != 1 or urls[0] not in canonical:
                raise Refused("origin URLs must point only to the canonical VMAFx repository")
        remote_head = self.git("ls-remote", "--exit-code", "origin", f"refs/heads/{branch}")
        if remote_head != f"{head}\trefs/heads/{branch}":
            raise Refused("remote branch changed before rebase")
        base, _, ref = self.git(
            "ls-remote", "--exit-code", "origin", "refs/heads/master"
        ).partition("\t")
        if ref != "refs/heads/master" or re.fullmatch(r"[0-9a-f]{40}", base) is None:
            raise Refused("remote master head is unavailable")
        # FETCH_HEAD is shared by worktrees and can be replaced by an unrelated
        # fetch. Fetch the observed objects without reading or changing it.
        self.git("fetch", "--no-tags", "--no-write-fetch-head", "origin", head, base)
        runs.mkdir(parents=True, exist_ok=True)
        run = Path(tempfile.mkdtemp(prefix=f"pr-{number}-", dir=runs))
        checkout = run / "checkout"
        self.git("worktree", "add", "--detach", str(checkout), head)
        (run / "owner.json").write_text(
            json.dumps({"pr": number, "head": head, "checkout": str(checkout)}) + "\n"
        )
        try:
            self.git("rebase", base, cwd=checkout)
            self.guard(number, head)
            updated = self.git("rev-parse", "HEAD", cwd=checkout)
            self.clean_head(checkout, updated)
            if updated != head:
                self.git(
                    "push",
                    f"--force-with-lease=refs/heads/{branch}:{head}",
                    "origin",
                    f"HEAD:refs/heads/{branch}",
                    cwd=checkout,
                )
            self.guard(number, updated)
        except (Refused, OSError):
            print(f"Retained failed rebase checkout and ownership receipt: {run}", file=sys.stderr)
            raise
        if self.git(
            "status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching", cwd=checkout
        ):
            print(f"Retained rebase evidence/artifacts: {run}")
        else:
            self.git("worktree", "remove", str(checkout))
        return updated

    def promote(self, number: int) -> None:
        pr = self.guard(number)
        if not pr["isDraft"]:
            raise Refused("promotion requires a draft PR")
        head = self.rebase(number)
        self.guard(number, head)
        self.gh("pr", "ready", str(number))
        # Arming is deliberately separate: the new head requires full local gates.

    def cycle(self, apply: bool, window: int) -> None:
        """Bound one train pass; every action repeats the shared fresh guard."""
        prs = json.loads(
            self.gh(
                "pr",
                "list",
                "--state",
                "open",
                "--limit",
                str(INVENTORY_LIMIT),
                "--json",
                PR_FIELDS,
            )
        )
        if not isinstance(prs, list) or len(prs) >= INVENTORY_LIMIT:
            raise Refused("PR inventory is incomplete; inspect the queue manually")
        eligible = []
        for pr in prs:
            number = pr["number"]
            try:
                current = self.guard(number)
            except Refused as exc:
                print(f"PR #{number}: refused: {exc}")
                if pr.get("autoMergeRequest") is not None:
                    raise Refused(
                        f"ineligible PR #{number} already has server auto-merge; owner must disarm it"
                    ) from exc
                continue
            eligible.append(current)
            print(f"PR #{number}: eligible {'draft' if current['isDraft'] else 'ready'}")
        if not apply:
            return
        ready = [pr for pr in eligible if not pr["isDraft"]]
        for pr in ready:
            try:
                if pr.get("mergeStateStatus") in {"BEHIND", "DIRTY"}:
                    self.rebase(pr["number"])
                else:
                    self.merge(pr["number"], auto=False)
            except (Refused, OSError, ValueError, KeyError, TypeError) as exc:
                print(f"PR #{pr['number']}: waiting for owner/validation: {exc}")
        if len(ready) < window:
            drafts = sorted((pr for pr in eligible if pr["isDraft"]), key=lambda pr: pr["number"])
            if drafts:
                self.promote(drafts[0]["number"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "action", choices=("inspect", "cycle", "promote", "rebase", "arm", "merge", "validate")
    )
    parser.add_argument("number", type=int, nargs="?")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument(
        "--checkout", type=Path, help="explicit checkout for executing full local gates"
    )
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--window", type=int, default=3, help="maximum ready queue size for cycle")
    args = parser.parse_args(argv)
    train = Train(args.repo_root, args.state_dir)
    try:
        if args.action != "cycle" and (args.number is None or args.number <= 0):
            raise Refused("an explicit positive PR number is required")
        if args.action == "cycle" and not args.apply:
            train.cycle(False, args.window)
            return 0
        if args.action == "inspect":
            print(json.dumps(train.guard(args.number), indent=2))
            return 0
        if not args.apply:
            raise Refused(
                "mutation/validation requires --apply; use inspect for read-only eligibility"
            )
        with train.mutation_lock():
            if args.action == "cycle":
                if args.window <= 0:
                    raise Refused("cycle window must be positive")
                train.cycle(True, args.window)
            elif args.action == "validate":
                if args.checkout is None:
                    raise Refused("validate requires an explicit --checkout")
                train.validate(args.number, args.checkout)
            elif args.action == "promote":
                train.promote(args.number)
            elif args.action == "rebase":
                train.rebase(args.number)
            else:
                train.merge(args.number, auto=args.action == "arm")
        return 0
    except (Refused, OSError, ValueError, KeyError, TypeError) as exc:
        print(f"merge-train: refused: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
