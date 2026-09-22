#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Detect merges that silently revert work already on the target branch (ADR-1284).

A pull request can remove work from ``master`` without any hunk in its review
diff looking like a removal of somebody else's commit.  The observed shape in
this fork (``.workingdir/evidence/silent-reverts-2026-09-18.md``) is a branch
whose *own* tree carries an older copy of a file — a rebase conflict resolved by
taking the stale side, or a squash built from a stale worktree.  The merge then
rewinds that file and nothing fails: the PR is green, the diff reads as the
branch's own work, and the reverted commit stays in ``git log`` so every later
audit believes the change is still in tree.

The gate answers one question: **what would this merge remove from the target
that the branch never set out to touch?**  It runs four detectors over the real
merge result (``git merge-tree --write-tree``), never over the branch tree
alone, so a file the branch does not touch can never be reported:

``rewind``
    The merged blob for a path is byte-identical to an *older* blob that path
    had on the target's own history, while the target's current blob differs.
    The merge resets the file to a historical state.  This is the detector that
    catches a rebased branch — for those, intent and effect are the same diff,
    so only history can tell that the content is old.

``reverse-hunk``
    The merge undoes a specific target commit: it removes the lines that commit
    added (the ones still live in the target) and restores the lines it
    deleted.  Catches partial rewinds that leave the rest of the file current.

``dropped``
    The merge loses lines the target had — gone from the merged file — that no
    non-merge commit on the branch ever removed.  Fires when a merge commit
    *inside* the branch resolved a conflict against the target.

``resurrected``
    The mirror image: the merge introduces lines the target does not have and
    no non-merge commit on the branch ever added — text the target had deleted,
    coming back.  This is the principle of
    ``~/.cache/vmafx-tools/check-resurrected.py`` (post-restack audit) lifted to
    a base/head pair.

``dropped`` and ``resurrected`` are quiet for a cleanly rebased branch by
construction; ``rewind`` and ``reverse-hunk`` are the detectors that cover that
case.  All four are reported together.

Deliberate reverts are declared, not silenced: a ``revert:`` Conventional-Commit
title, or ``reverts: #N`` / ``intentional revert: <reason>`` in the PR body,
turns findings into notices.

Fails closed.  Anything that makes the analysis unsound — an unresolvable ref,
no merge base, a conflicting merge, a git too old for ``merge-tree
--write-tree`` — exits non-zero rather than reporting "clean".

Exit codes: 0 clean (or declared), 1 findings, 2 usage / unanalysable.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Resolved once: an absolute path keeps the invocation independent of whatever
# PATH the caller (a CI step, a Makefile recipe, a hook) happens to export.
GIT = shutil.which("git") or "/usr/bin/git"

# Paths whose content is rendered from elsewhere in the tree.  A "rewind" there
# is a regeneration artefact, not a lost commit; the generator's input is what
# the gate protects.
GENERATED_PREFIXES = (
    "docs/adr/by-tag/",
    "docs/adr/README.md",
    "CHANGELOG.md",
    "mkdocs.yml",
    ".standards-baseline.json",
    "scripts/ci/tidy-baseline-",
)

# A line only counts as evidence of a revert when it carries enough text to be
# unique.  `}`, `*/`, `#endif` and friends match everywhere and would make the
# set arithmetic fire on unrelated edits.
MIN_EVIDENCE_LEN = 6
MIN_EVIDENCE_LINES = 2

# Conflict markers are never content worth protecting: `scripts/ci/
# check-conflict-markers.sh` exists because master has carried them before
# (`0c494cca0` left three in core/src/feature/cuda/integer_vif_cuda.c), and a
# PR that deletes them must not read as a revert.
CONFLICT_MARKER = re.compile(r"^(<{7}|={7}|>{7})(\s|$)")

DECLARED_PATTERNS = (
    re.compile(r"\breverts:\s*#\d+", re.IGNORECASE),
    re.compile(r"\bintentional revert:\s*(?!REASON\b)\S+", re.IGNORECASE),
)

Lines = collections.Counter
FileLines = dict[str, Lines]


class GitError(RuntimeError):
    """A git invocation the analysis cannot proceed without has failed."""


class Repo:
    """Read-only git accessor rooted at one working tree."""

    def __init__(self, root: Path) -> None:
        self.root = root

    def run(self, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        """Invoke git with a fixed argv (never a shell) and return the result."""
        proc = subprocess.run(  # noqa: S603 -- fixed argv list, no shell, no user string splicing
            [GIT, "-C", str(self.root), *args],
            capture_output=True,
            text=True,
            check=False,
        )
        if check and proc.returncode != 0:
            raise GitError(
                f"git {' '.join(args)} failed ({proc.returncode}): {proc.stderr.strip()}"
            )
        return proc

    def out(self, *args: str) -> str:
        return self.run(*args).stdout

    def resolve(self, ref: str) -> str:
        proc = self.run("rev-parse", "--verify", f"{ref}^{{commit}}", check=False)
        if proc.returncode != 0:
            raise GitError(f"cannot resolve {ref!r} to a commit in {self.root}")
        return proc.stdout.strip()

    def blob(self, tree_ish: str, path: str) -> str | None:
        """Return the blob SHA for *path* in *tree_ish*, or None when absent."""
        proc = self.run("rev-parse", "--verify", f"{tree_ish}:{path}", check=False)
        return proc.stdout.strip() if proc.returncode == 0 else None

    def blob_lines(self, tree_ish: str, path: str) -> set[str]:
        proc = self.run("show", f"{tree_ish}:{path}", check=False)
        if proc.returncode != 0:
            return set()
        return {line.rstrip() for line in proc.stdout.split("\n")}


def is_generated(path: str) -> bool:
    return path.startswith(GENERATED_PREFIXES)


def is_evidence(line: str) -> bool:
    """True when a line is distinctive enough to key set arithmetic on."""
    stripped = line.strip()
    if CONFLICT_MARKER.match(stripped):
        return False
    return len(stripped) >= MIN_EVIDENCE_LEN and any(ch.isalnum() for ch in stripped)


def parse_diff(diff: str) -> tuple[FileLines, FileLines]:
    """Split a ``-U0`` unified diff into per-file added and removed line bags."""
    added: FileLines = collections.defaultdict(Lines)
    removed: FileLines = collections.defaultdict(Lines)
    path = None
    for line in diff.split("\n"):
        # `+++ b/<path>` names every file except a deletion, where it reads
        # `+++ /dev/null` and only the `--- a/<path>` header carries the name.
        if line.startswith("+++ b/") or (line.startswith("--- a/") and path is None):
            path = line[6:]
        elif line.startswith("diff --git "):
            path = None
        elif path is None or is_generated(path):
            continue
        elif line.startswith("+") and not line.startswith("+++"):
            added[path][line[1:].rstrip()] += 1
        elif line.startswith("-") and not line.startswith("---"):
            removed[path][line[1:].rstrip()] += 1
    return dict(added), dict(removed)


DIFF_FLAGS = ("--no-color", "--no-renames", "--no-ext-diff", "-U0", "-M0")


def diff_pair(repo: Repo, before: str, after: str) -> tuple[FileLines, FileLines]:
    return parse_diff(repo.out("diff", *DIFF_FLAGS, before, after))


def merge_result_tree(repo: Repo, base: str, head: str) -> str:
    """Return the tree git would produce for a merge of *head* into *base*.

    This is the tree GitHub's squash-merge commits, so the gate measures the
    content that would actually land rather than the branch tree in isolation.
    """
    proc = repo.run("merge-tree", "--write-tree", base, head, check=False)
    if proc.returncode == 0:
        return proc.stdout.split("\n", 1)[0].strip()
    if proc.returncode == 1:
        conflicts = proc.stdout.strip().split("\n")
        raise GitError(
            "the merge does not resolve cleanly, so its result cannot be analysed.\n"
            "  Rebase the branch onto the target and re-run the gate.\n"
            "  git reported:\n    " + "\n    ".join(conflicts[:12])
        )
    raise GitError(
        "`git merge-tree --write-tree` is unavailable (git >= 2.38 required): "
        + (proc.stderr.strip() or f"exit {proc.returncode}")
    )


def branch_intent(repo: Repo, merge_base: str, head: str) -> tuple[FileLines, FileLines]:
    """Union the per-commit diffs of the branch's non-merge commits.

    Merge commits are excluded on purpose: a conflict resolution is not work the
    branch set out to do, and resolutions taken against the target are exactly
    what the ``dropped`` / ``resurrected`` detectors look for.
    """
    added: FileLines = collections.defaultdict(Lines)
    removed: FileLines = collections.defaultdict(Lines)
    revs = repo.out("rev-list", "--no-merges", f"{merge_base}..{head}").split()
    for rev in revs:
        commit_added, commit_removed = diff_pair(repo, f"{rev}^", rev)
        for path, lines in commit_added.items():
            added[path] += lines
        for path, lines in commit_removed.items():
            removed[path] += lines
    return dict(added), dict(removed)


def history_blobs(repo: Repo, ref: str, path: str, window: int) -> dict[str, str]:
    """Map every blob *path* held over the last *window* commits to its commit."""
    revs = repo.out("log", f"--max-count={window}", "--format=%H", ref, "--", path).split()
    if not revs:
        return {}
    query = "\n".join(f"{rev}:{path}" for rev in revs) + "\n"
    proc = subprocess.run(  # noqa: S603 -- fixed argv list, no shell; refs come from git itself
        [GIT, "-C", str(repo.root), "cat-file", "--batch-check=%(objectname)"],
        input=query,
        capture_output=True,
        text=True,
        check=False,
    )
    seen: dict[str, str] = {}
    for rev, line in zip(revs, proc.stdout.split("\n"), strict=False):
        blob = line.strip()
        if blob and " " not in blob:
            seen.setdefault(blob, rev)
    return seen


def commit_diffs(repo: Repo, ref: str, path: str, window: int) -> list[tuple[str, Lines, Lines]]:
    """Per-commit (sha, added, removed) bags for *path* over the last *window* commits."""
    text = repo.out(
        "log",
        f"--max-count={window}",
        "--format=__commit__ %H",
        "-p",
        *DIFF_FLAGS,
        ref,
        "--",
        path,
    )
    out: list[tuple[str, Lines, Lines]] = []
    sha = ""
    added, removed = Lines(), Lines()
    for line in text.split("\n"):
        if line.startswith("__commit__ "):
            if sha:
                out.append((sha, added, removed))
            sha, added, removed = line.split(" ", 1)[1].strip(), Lines(), Lines()
        elif line.startswith("+") and not line.startswith("+++"):
            added[line[1:].rstrip()] += 1
        elif line.startswith("-") and not line.startswith("---"):
            removed[line[1:].rstrip()] += 1
    if sha:
        out.append((sha, added, removed))
    return out


def subject(repo: Repo, sha: str) -> str:
    return repo.out("log", "-1", "--format=%s", sha).strip()


class Finding(dict):
    """One reported problem, JSON-serialisable for the ``--json`` report."""

    def __init__(self, kind: str, path: str, detail: str, lines: list[str]) -> None:
        super().__init__(kind=kind, path=path, detail=detail, lines=lines[:6])


def detect_rewind(
    repo: Repo, base: str, merged: str, effect: FileLines, paths: list[str], window: int
) -> list[Finding]:
    """Paths the merge resets to a blob they held earlier on the target's history."""
    findings = []
    for path in paths:
        base_blob = repo.blob(base, path)
        merged_blob = repo.blob(merged, path)
        if base_blob is None or merged_blob is None or base_blob == merged_blob:
            continue
        # A rewind that costs the target no substantive line is not a loss:
        # dropping stray conflict markers resets a file to an older blob too.
        if not any(is_evidence(line) for line in effect.get(path, ())):
            continue
        history = history_blobs(repo, base, path, window)
        origin = history.get(merged_blob)
        if origin is None or origin == base:
            continue
        findings.append(
            Finding(
                "rewind",
                path,
                f"the merged file is byte-identical to its blob at {origin[:9]} "
                f"({subject(repo, origin)[:70]}), not to the target's current content",
                [f"merged blob {merged_blob[:9]} == blob at {origin[:9]}"],
            )
        )
    return findings


def _undone(
    live: set[str], removed_by: set[str], eff_removed: set[str], eff_added: set[str]
) -> bool:
    """True when the merge both deletes a commit's additions and restores its deletions.

    Both sides need ``MIN_EVIDENCE_LINES`` of their own.  A single restored line
    is what a *rewrite* of freshly added code looks like — ``04c6610ae``
    rewrote the file-header comment ``b14a718ed`` had written and happened to
    restore one line of the text before it — and calling that a revert costs
    more in noise than the one-line reversals it would buy.
    """
    if len(live) < MIN_EVIDENCE_LINES or not live <= eff_removed:
        return False
    if removed_by:
        return len(removed_by) >= MIN_EVIDENCE_LINES and removed_by <= eff_added
    # A pure-addition commit counts as undone only when the merge puts nothing
    # in its place; otherwise the same rewrite ambiguity applies.
    return not eff_added


def detect_reverse_hunk(
    repo: Repo, base: str, effect: tuple[FileLines, FileLines], paths: list[str], window: int
) -> list[Finding]:
    """Target commits whose hunks the merge undoes without saying so."""
    eff_added, eff_removed = effect
    findings = []
    for path in paths:
        removed = {line for line in eff_removed.get(path, ()) if is_evidence(line)}
        if not removed:
            continue
        added = {line for line in eff_added.get(path, ()) if is_evidence(line)}
        base_lines = repo.blob_lines(base, path)
        for sha, commit_added, commit_removed in commit_diffs(repo, base, path, window):
            live = {line for line in commit_added if is_evidence(line)} & base_lines
            deleted = {line for line in commit_removed if is_evidence(line)}
            if not _undone(live, deleted, removed, added):
                continue
            findings.append(
                Finding(
                    "reverse-hunk",
                    path,
                    f"the merge undoes {sha[:9]} ({subject(repo, sha)[:70]}): "
                    f"{len(live)} line(s) that commit added are removed"
                    + (f" and {len(deleted)} it deleted come back" if deleted else ""),
                    sorted(live),
                )
            )
    return findings


def detect_unintended(
    repo: Repo, survivor: str, effect: FileLines, intent: FileLines, kind: str, detail: str
) -> list[Finding]:
    """Lines the merge moves that no non-merge branch commit moved.

    A line is only reported when it is genuinely absent from *survivor* — the
    merged tree for ``dropped``, the target for ``resurrected``.  Without that
    check the detectors fire on a diff-alignment artefact: inserting text above
    a line makes the cumulative ``base..head`` diff re-pair that line as a
    delete plus an add, while no individual commit's diff shows the pair, so a
    line still present in both trees looks unintentionally moved.  Measured on
    a 48-commit stack, that artefact was the only thing these two detectors
    reported (``python/test/executor_test.py``, one class statement present in
    both trees).
    """
    findings = []
    for path, lines in sorted(effect.items()):
        extra = lines - intent.get(path, Lines())
        candidates = sorted(line for line in extra if is_evidence(line))
        if not candidates:
            continue
        surviving = repo.blob_lines(survivor, path)
        evidence = [line for line in candidates if line not in surviving]
        if not evidence:
            continue
        findings.append(Finding(kind, path, f"{len(evidence)} line(s) {detail}", evidence))
    return findings


def declared(title: str, body: str) -> str:
    """Return the declaration that exempts this change, or "" when undeclared."""
    if re.match(r"^revert[:(]", title.strip(), re.IGNORECASE):
        return "Conventional-Commit `revert:` title"
    stripped = re.sub(r"<!--.*?-->", "", body, flags=re.DOTALL)
    for pattern in DECLARED_PATTERNS:
        match = pattern.search(stripped)
        if match:
            return match.group(0).strip()
    return ""


def analyse(repo: Repo, base: str, head: str, window: int) -> list[Finding]:
    """Run every detector against the merge of *head* into *base*."""
    merge_base = repo.run("merge-base", base, head, check=False)
    if merge_base.returncode != 0 or not merge_base.stdout.strip():
        raise GitError(
            f"no merge base between {base[:9]} and {head[:9]} — the histories are unrelated, "
            "so what the merge would remove cannot be established"
        )
    merged = merge_result_tree(repo, base, head)
    eff_added, eff_removed = diff_pair(repo, base, merged)
    int_added, int_removed = branch_intent(repo, merge_base.stdout.strip(), head)
    paths = sorted(set(eff_added) | set(eff_removed))
    return [
        *detect_rewind(repo, base, merged, eff_removed, paths, window),
        *detect_reverse_hunk(repo, base, (eff_added, eff_removed), paths, window),
        *detect_unintended(
            repo,
            merged,
            eff_removed,
            int_removed,
            "dropped",
            "gone from the merge that the branch never removed",
        ),
        *detect_unintended(
            repo,
            base,
            eff_added,
            int_added,
            "resurrected",
            "new to the target that the branch never added",
        ),
    ]


def report(findings: list[Finding], note: str) -> None:
    """Print findings in GitHub-Actions annotation form."""
    by_kind: dict[str, list[Finding]] = collections.defaultdict(list)
    for finding in findings:
        by_kind[finding["kind"]].append(finding)
    for kind in ("rewind", "reverse-hunk", "dropped", "resurrected"):
        for finding in by_kind.get(kind, []):
            print(f"{note}title=Silent revert ({kind})::{finding['path']}: {finding['detail']}")
            for line in finding["lines"]:
                print(f"    {line[:160]}")


EXPLAIN = """
A silent revert is not visible in the PR diff: the reverted commit stays in
`git log`, so every later audit believes the change is still in tree. See
ADR-1284 and docs/development/silent-revert-gate.md.

Fix the branch, do not annotate it:

  1. `git fetch origin && git rebase origin/master` — then re-inspect every
     hunk the rebase resolved. A file that came back whole is the tell.
  2. Re-run this gate: `make silent-revert-check`.

If the revert is intended, say so where a reviewer sees it: a `revert:`
Conventional-Commit title, or `reverts: #N` / `intentional revert: <reason>`
in the PR description.
""".strip()


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="check-silent-revert.py",
        description="Report work a merge would remove from its target without saying so.",
    )
    parser.add_argument("--base", default=os.environ.get("BASE_SHA", ""), help="target ref")
    parser.add_argument("--head", default=os.environ.get("HEAD_SHA", ""), help="branch ref")
    parser.add_argument("--repo", default=".", help="repository root (default: cwd)")
    parser.add_argument(
        "--history-window",
        type=int,
        default=40,
        help="commits of target history to inspect per path (default: 40)",
    )
    parser.add_argument("--body-file", help="file holding the PR description")
    parser.add_argument("--title", default=os.environ.get("PR_TITLE", ""), help="PR title")
    parser.add_argument("--json", dest="json_out", help="write the findings to this path as JSON")
    return parser.parse_args(argv)


def resolve_endpoints(repo: Repo, args: argparse.Namespace) -> tuple[str, str]:
    """Resolve the base/head pair, defaulting to origin/master..HEAD."""
    base = args.base or "origin/master"
    head = args.head or "HEAD"
    return repo.resolve(base), repo.resolve(head)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo = Repo(Path(args.repo).resolve())
    body = (
        Path(args.body_file).read_text(encoding="utf-8")
        if args.body_file
        else os.environ.get("PR_BODY", "")
    )

    try:
        base, head = resolve_endpoints(repo, args)
        findings = analyse(repo, base, head, max(1, args.history_window))
    except GitError as err:
        print(f"::error title=Silent-revert gate could not run::{err}")
        return 2

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(findings, indent=2) + "\n", encoding="utf-8")

    if not findings:
        print(
            f"check-silent-revert: clean — merging {head[:9]} into {base[:9]} removes no target work."
        )
        return 0

    exemption = declared(args.title, body)
    report(findings, "::notice " if exemption else "::error ")
    if exemption:
        print(f"check-silent-revert: {len(findings)} finding(s), declared by {exemption} — PASS.")
        return 0
    print(f"\ncheck-silent-revert: {len(findings)} finding(s) merging {head[:9]} into {base[:9]}.")
    print(EXPLAIN)
    return 1


if __name__ == "__main__":
    sys.exit(main())
