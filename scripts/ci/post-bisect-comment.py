#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Post or update the sticky bisect-tracker comment on a tracking issue.

Used by `.github/workflows/nightly-bisect.yml`. The first time the
workflow runs against a new tracking issue, this writes a fresh comment.
On every subsequent run, it edits the *same* comment in place so the
issue history stays a flat audit log instead of a wall of duplicates.

Identification: the most recent comment authored by the workflow's
GITHUB_ACTOR whose body starts with the magic header `<!-- bisect-tracker -->`.

Usage:

    GH_TOKEN=...  python scripts/ci/post-bisect-comment.py \\
        --issue 42 --report bisect-result.json [--repo owner/name]
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

STICKY_HEADER = "<!-- bisect-tracker -->"

# Cap the cache-check stderr we inline into the wiring-broke comment so a
# pathological run (e.g. unbounded log) doesn't blow past GitHub's 65 536-char
# issue-comment limit. Keeps the visible diagnostic compact for issue #40.
WIRING_BROKE_LOG_MAX_CHARS = 4000


def _gh(*args: str) -> str:
    """Run `gh` and return stdout; raise on non-zero exit."""
    # S603/S607: trusted args (caller-controlled, not user input);
    # `gh` resolved from $PATH is correct here — GitHub Actions
    # provides it pre-installed and at varying absolute locations.
    cmd = ["gh", *args]
    result = subprocess.run(cmd, check=True, capture_output=True, text=True)  # noqa: S603
    return result.stdout


def _format_wiring_broke_body(error_log: str, run_url: str) -> str:
    """Render the sticky body when the cache `--check` step itself failed.

    Distinct from a quality-regression verdict: the bisect never ran, so
    there are no per-step PLCC numbers. We surface the cache-check stderr
    so a maintainer scanning issue #40 sees the failure mode at a glance
    (toolchain drift, missing file, schema mismatch). See ADR-0262.
    """
    truncated = error_log.strip()
    if len(truncated) > WIRING_BROKE_LOG_MAX_CHARS:
        truncated = truncated[:WIRING_BROKE_LOG_MAX_CHARS] + "\n…(truncated)"
    return "\n".join(
        [
            STICKY_HEADER,
            "## Latest nightly bisect-model-quality run",
            "",
            "**WIRING BROKE** — fixture cache check failed; the bisect did "
            "not execute. See workflow logs for the full traceback; the "
            "cache-check stderr is captured below.",
            "",
            f"- workflow run: {run_url}",
            "- typical fix: regenerate the committed cache",
            "  (`python ai/scripts/build_bisect_cache.py`) and commit, *or* "
            "  diagnose the toolchain drift if regeneration alone does not "
            "  restore parity.",
            "",
            "<details><summary>cache-check stderr</summary>",
            "",
            "```",
            truncated or "(empty)",
            "```",
            "",
            "</details>",
        ]
    )


def _format_body(report: dict, run_url: str) -> str:
    threshold = f"{report['threshold_kind']} = {report['threshold_value']:g}"
    n_models = report["n_models"]
    n_visited = len(report["steps"])
    first_bad = report.get("first_bad_index")
    last_good = report.get("last_good_index")
    if first_bad is None:
        verdict_line = f"**Verdict**: {report['verdict']}"
    else:
        verdict_line = (
            f"**REGRESSION** — first bad at index {first_bad} "
            f"(`{Path(report['first_bad_model']).name}`); "
            f"last good at index {last_good}"
            + (f" (`{Path(report['last_good_model']).name}`)" if last_good is not None else "")
        )

    rows = [
        "| idx | status | PLCC | SROCC | RMSE | model |",
        "| --: | :-: | --: | --: | --: | :-- |",
    ]
    for s in sorted(report["steps"], key=lambda r: r["index"]):
        status = "GOOD" if s["passed"] else "BAD"
        r = s["report"]
        rows.append(
            f"| {s['index']} | {status} | {r['plcc']:+.4f} | "
            f"{r['srocc']:+.4f} | {r['rmse']:.4f} | `{Path(s['model']).name}` |"
        )

    return "\n".join(
        [
            STICKY_HEADER,
            "## Latest nightly bisect-model-quality run",
            "",
            verdict_line,
            "",
            f"- threshold: `{threshold}`",
            f"- models in timeline: `{n_models}`",
            f"- models visited: `{n_visited}`",
            f"- workflow run: {run_url}",
            "",
            "<details><summary>per-step report</summary>",
            "",
            *rows,
            "",
            "</details>",
        ]
    )


def _find_existing_comment(repo: str, issue: int) -> int | None:
    """Return the comment ID of the existing sticky, or None."""
    raw = _gh(
        "api",
        f"repos/{repo}/issues/{issue}/comments",
        "--paginate",
    )
    comments = json.loads(raw)
    for c in reversed(comments):
        if c.get("body", "").startswith(STICKY_HEADER):
            return int(c["id"])
    return None


def _write_comment(repo: str, issue: int, body: str) -> None:
    comment_id = _find_existing_comment(repo, issue)
    # `gh api -f` sends raw strings (no @file substitution); `-F` types
    # values and resolves @file. Pass the body as a raw string from stdin
    # via `--input -` so we don't have to escape multi-line markdown on
    # the command line and don't smuggle a literal "@path" into the JSON.
    if comment_id is None:
        endpoint = f"repos/{repo}/issues/{issue}/comments"
        _gh_with_stdin(["api", endpoint, "--input", "-"], body)
        print(f"posted new sticky comment on issue #{issue}")
    else:
        endpoint = f"repos/{repo}/issues/comments/{comment_id}"
        _gh_with_stdin(
            ["api", "--method", "PATCH", endpoint, "--input", "-"],
            body,
        )
        print(f"edited sticky comment {comment_id} on issue #{issue}")


def _gh_with_stdin(args: list[str], body: str) -> None:
    """Run `gh` and feed a JSON body on stdin; raise on non-zero exit."""
    payload = json.dumps({"body": body})
    cmd = ["gh", *args]
    # S603/S607: trusted args (caller-controlled, not user input);
    # `gh` resolved from $PATH is the GitHub-Actions convention.
    result = subprocess.run(  # noqa: S603
        cmd,
        check=False,
        input=payload,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"gh stderr: {result.stderr.strip()}", file=sys.stderr)
        print(f"gh stdout: {result.stdout.strip()}", file=sys.stderr)
        raise subprocess.CalledProcessError(
            result.returncode, cmd, output=result.stdout, stderr=result.stderr
        )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--issue", type=int, required=True, help="Tracking issue number")
    p.add_argument(
        "--report",
        type=Path,
        default=None,
        help="Path to bisect JSON report (omit when --wiring-broke is used)",
    )
    p.add_argument(
        "--wiring-broke",
        action="store_true",
        help="Post a 'cache-check failed; bisect did not run' sticky update.",
    )
    p.add_argument(
        "--error-log",
        type=Path,
        default=None,
        help="Path to a stderr log captured during the failing step " "(used with --wiring-broke).",
    )
    p.add_argument(
        "--repo",
        default=os.environ.get("GITHUB_REPOSITORY", "VMAFx/vmafx"),
        help="owner/name (defaults to $GITHUB_REPOSITORY or VMAFx/vmafx)",
    )
    args = p.parse_args()

    server_url = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    run_id = os.environ.get("GITHUB_RUN_ID", "?")
    run_url = f"{server_url}/{args.repo}/actions/runs/{run_id}"

    if args.wiring_broke:
        log = ""
        if args.error_log is not None and args.error_log.is_file():
            log = args.error_log.read_text(encoding="utf-8", errors="replace")
        body = _format_wiring_broke_body(log, run_url)
    else:
        if args.report is None or not args.report.is_file():
            print(
                f"report not found: {args.report} (pass --wiring-broke for the "
                "cache-check-failed path)",
                file=sys.stderr,
            )
            return 1
        report = json.loads(args.report.read_text(encoding="utf-8"))
        body = _format_body(report, run_url)

    _write_comment(args.repo, args.issue, body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
