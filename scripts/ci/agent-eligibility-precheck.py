#!/usr/bin/env python3
"""Eligibility precheck for agent dispatch.

Symphony §3.1 has a *Reconciliation* step that stops in-flight runs
when the underlying issue's state changes. The fork ports the same
idea as a **pre-dispatch** gate (cheaper than mid-flight stop):
before invoking ``Agent(...)`` for a given backlog item, run this
script. It exits 0 if dispatching is still useful, 1 otherwise.

Three checks, in order:

    1. **Backlog row not closed.**
       Parse ``.workingdir2/BACKLOG.md``. If the row for the given
       ID has status DONE / CLOSED / REMOVED, exit 1 with the
       closing PR's number (when known).

    2. **No merged PR for this scope.**
       ``gh pr list --state merged --search "<id> in:title,body"``.
       Any hit means the work has likely already shipped. Exit 1
       and list the matching PRs so the operator can read them.

    3. **No in-flight agent on the same scope.**
       Scan ``/tmp/claude-1000/*/tasks/*.output`` (the CLI agent
       harness's per-task metadata; configurable via
       ``--harness-tasks-glob``) and the open-PR head-branch list
       for any active task that mentions the same item ID. If found,
       exit 1 with the existing task ID.

Exit 0 if all three checks pass; exit 1 otherwise. Verdicts go to
**stderr** in GitHub Actions ``::error`` format so a wrapping CI
script can parse them.

Usage::

    scripts/ci/agent-eligibility-precheck.py --backlog-id T3-9
    scripts/ci/agent-eligibility-precheck.py --task-tag codeql-cpp-overflow
    scripts/ci/agent-eligibility-precheck.py --backlog-id T7-5 --skip-gh-search

The ``--task-tag`` form is for runs without a backlog row (e.g.
CodeQL sweeps); checks 1 and 2 are skipped, only check 3 runs.

The script is intentionally **read-only**: it never modifies
BACKLOG.md, never writes to ``/tmp``, and never calls ``gh`` write
commands. Safe to run unconditionally in any wrapper.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable

# The script lives at scripts/ci/, the lib package at scripts/lib/.
_SCRIPTS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_SCRIPTS))
from lib.backlog_tracker import BacklogTracker, GitHubTracker  # noqa: E402  (sys.path bootstrap)

# ---------------------------------------------------------------------------
# stderr helpers — keep verdicts machine-parseable.
# ---------------------------------------------------------------------------


def _emit_error(title: str, body: str) -> None:
    """Print a GitHub Actions ``::error`` line + a human echo."""
    sys.stderr.write(f"::error title={title}::{body}\n")


def _emit_notice(message: str) -> None:
    sys.stderr.write(f"  {message}\n")


# ---------------------------------------------------------------------------
# Check 1 — Backlog row not closed.
# ---------------------------------------------------------------------------


def check_backlog_row_open(backlog_id: str, tracker: BacklogTracker) -> bool:
    """Return True if the backlog row is OPEN-class, False otherwise."""
    item = tracker.get(backlog_id)
    if item is None:
        # An ID the operator passed in but doesn't exist in
        # BACKLOG.md is suspicious but not necessarily blocking —
        # the row may live in OPEN.md or PLAN.md instead. Warn but
        # let the dispatcher continue.
        _emit_notice(f"backlog: {backlog_id} not found in BACKLOG.md (skipping check 1)")
        return True
    if item.is_closed():
        prs = ", ".join(f"#{n}" for n in item.pr_refs) or "none recorded"
        _emit_error(
            f"agent-eligibility: {backlog_id} is {item.status}",
            f"BACKLOG.md row already closed (PRs: {prs}). Title: {item.title}",
        )
        return False
    if item.status in {"BLOCKED", "DEFERRED"}:
        _emit_error(
            f"agent-eligibility: {backlog_id} is {item.status}",
            f"BACKLOG.md row is {item.status} — clear the blocker before dispatching. "
            f"Title: {item.title}",
        )
        return False
    _emit_notice(f"backlog: {backlog_id} status={item.status} priority={item.priority} — OK")
    return True


# ---------------------------------------------------------------------------
# Check 2 — No merged PR mentions this scope.
# ---------------------------------------------------------------------------


def check_no_merged_pr(backlog_id: str, tracker: GitHubTracker) -> bool:
    """Return True if no merged PR mentions the backlog ID, False otherwise.

    Uses ``gh pr list --search "<id> in:title,body"``. If ``gh`` is
    unavailable (offline / not authenticated), the check is skipped
    with a notice — the operator can re-run with ``--skip-gh-search``
    to silence the notice.
    """
    if shutil.which("gh") is None:
        _emit_notice("gh CLI not in PATH — skipping merged-PR check")
        return True
    try:
        prs = tracker.search_prs(
            f"{backlog_id} in:title,body",
            state="merged",
            limit=10,
        )
    except subprocess.CalledProcessError as exc:
        _emit_notice(f"gh search failed (rc={exc.returncode}); skipping merged-PR check")
        return True
    except FileNotFoundError:
        _emit_notice("gh CLI missing — skipping merged-PR check")
        return True

    # Filter for PRs that name the ID as a token (not e.g.
    # `T3-90` matching a search for `T3-9`). The regex requires a
    # word boundary or punctuation around the ID.
    token = re.compile(rf"(?<![A-Za-z0-9-]){re.escape(backlog_id)}(?![A-Za-z0-9])")
    hits = [
        pr
        for pr in prs
        if token.search(pr.get("title", "") or "") or token.search(pr.get("body", "") or "")
    ]
    if hits:
        listing = ", ".join(f"#{pr['number']} ({pr['title']!r})" for pr in hits[:5])
        _emit_error(
            f"agent-eligibility: {backlog_id} already has merged PR(s)",
            f"Search hit {len(hits)} merged PR(s): {listing}. Re-investigate before dispatching.",
        )
        return False
    _emit_notice(f"merged-PR search: no hits for {backlog_id} — OK")
    return True


# ---------------------------------------------------------------------------
# Check 3 — No in-flight agent on the same scope.
# ---------------------------------------------------------------------------


def _read_text_safely(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def check_no_active_agent(
    scope_token: str,
    *,
    tasks_glob: str,
    open_branches: Iterable[str],
) -> bool:
    """Return True if no active agent is on the same scope.

    Two surfaces are scanned:

    * ``tasks_glob`` — files matching this glob (default
      ``/tmp/claude-1000/*/tasks/*.output``) are read as plain text;
      any file whose contents mention ``scope_token`` is treated as
      an in-flight agent run.

    * ``open_branches`` — head-branch names of currently-open PRs
      (provided by the caller so this function stays unit-testable).
      Branches whose name contains the token are flagged.
    """
    token = re.compile(rf"(?<![A-Za-z0-9-]){re.escape(scope_token)}(?![A-Za-z0-9])")

    # Scan harness task files. Glob may match nothing if the harness
    # is configured differently or no agents are active — that's a
    # pass, not an error.
    matching_tasks: list[str] = []
    # PTH207 noqa: tasks_glob is a user-overridable, fully-arbitrary glob
    # (default is an absolute multi-wildcard pattern like
    # ``/tmp/claude-*/tasks/*.output``). ``Path.glob`` would require splitting
    # into a base directory + relative pattern, which we cannot do without
    # parsing the user-supplied glob — ``glob.glob`` is the correct primitive.
    for path_str in glob.glob(tasks_glob):  # noqa: PTH207
        text = _read_text_safely(Path(path_str))
        if token.search(text):
            matching_tasks.append(path_str)

    matching_branches = [b for b in open_branches if scope_token.lower() in b.lower()]

    if not matching_tasks and not matching_branches:
        _emit_notice(f"in-flight scan: no active agent on {scope_token} — OK")
        return True

    if matching_tasks:
        _emit_error(
            f"agent-eligibility: {scope_token} already in flight (harness)",
            f"Found {len(matching_tasks)} active task file(s): " + ", ".join(matching_tasks[:3]),
        )
    if matching_branches:
        _emit_error(
            f"agent-eligibility: {scope_token} already in flight (PR branch)",
            f"Open PR branch(es): {', '.join(matching_branches[:3])}",
        )
    return False


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Pre-dispatch eligibility gate for Claude Code agent runs.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="See ADR-0355 (Symphony-inspired agent-dispatch infrastructure).",
    )
    parser.add_argument(
        "--backlog-id",
        help="BACKLOG.md row identifier (e.g. T3-9, T7-10b, TA-VOCAB).",
    )
    parser.add_argument(
        "--task-tag",
        help=(
            "Free-form scope tag for runs without a BACKLOG.md row "
            "(e.g. codeql-cpp-overflow). Skips checks 1 and 2."
        ),
    )
    parser.add_argument(
        "--repo",
        default=os.environ.get("GH_REPO", "VMAFx/vmafx"),
        help="GitHub repository (default: VMAFx/vmafx).",
    )
    # The Claude Code harness writes per-task metadata under
    # `/tmp/claude-<uid>/...` — that path is the harness's contract,
    # not a tempfile we own (we never write to it). The S108 lint
    # rule fires on the literal `/tmp/` prefix; this default is
    # **read-only** and overridable via `--harness-tasks-glob`, so
    # the rule's threat model (an attacker pre-creating a predictable
    # tempfile) doesn't apply. Cite ADR-0355 for the contract.
    default_glob = (
        f"/tmp/claude-{os.getuid()}/*/tasks/*.output"  # noqa: S108  ADR-0355: harness path, read-only
        if hasattr(os, "getuid")
        else "/tmp/claude-*/tasks/*.output"  # noqa: S108  ADR-0355: harness path, read-only
    )
    parser.add_argument(
        "--harness-tasks-glob",
        default=default_glob,
        help="Glob for the agent harness's per-task metadata files.",
    )
    parser.add_argument(
        "--skip-gh-search",
        action="store_true",
        help="Skip the merged-PR search (offline mode).",
    )
    parser.add_argument(
        "--skip-active-scan",
        action="store_true",
        help="Skip the in-flight-agent scan.",
    )
    parser.add_argument(
        "--backlog-path",
        default=None,
        help="Override path to BACKLOG.md (default: autodetect via scripts/lib).",
    )
    args = parser.parse_args(argv)

    if not args.backlog_id and not args.task_tag:
        parser.error("either --backlog-id or --task-tag is required")

    scope_token = args.backlog_id or args.task_tag
    sys.stderr.write(f"agent-eligibility-precheck: scope={scope_token}\n")

    # Wire dependencies.
    backlog_path = Path(args.backlog_path) if args.backlog_path else None
    backlog = BacklogTracker(backlog_path)
    github = GitHubTracker(repo=args.repo)

    failed = False

    # -- Check 1 ---------------------------------------------------
    if args.backlog_id and not check_backlog_row_open(args.backlog_id, backlog):
        failed = True

    # -- Check 2 ---------------------------------------------------
    if (
        args.backlog_id
        and not args.skip_gh_search
        and not check_no_merged_pr(args.backlog_id, github)
    ):
        failed = True

    # -- Check 3 ---------------------------------------------------
    if not args.skip_active_scan:
        # Only consult `gh` for open branches if we have it
        # available; otherwise pass an empty list.
        open_branches: list[str] = []
        if shutil.which("gh") is not None:
            try:
                open_branches = github.open_agent_branches()
            except (subprocess.CalledProcessError, FileNotFoundError):
                _emit_notice("gh open-branch listing failed; skipping branch portion of check 3")
        if not check_no_active_agent(
            scope_token,
            tasks_glob=args.harness_tasks_glob,
            open_branches=open_branches,
        ):
            failed = True

    if failed:
        sys.stderr.write("agent-eligibility-precheck: VERDICT=FAIL — do not dispatch.\n")
        return 1

    sys.stderr.write("agent-eligibility-precheck: VERDICT=PASS — dispatch eligible.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
