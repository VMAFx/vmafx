#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Community metrics for a date window (issue #2554, maintainer decision Q-182 d).

Reads the GitHub API through the ``gh`` CLI (no token handling of its own) and
prints a Markdown or JSON report for the window: issues and pull requests
opened and closed, the median and 90th-percentile time to a maintainer's first
response, the open ``good first issue`` / ``help wanted`` counts, open pull
requests older than 30 days, and the contributors whose first merged pull
request falls inside the window. Output only — nothing is posted; the
maintainer decides what to publish.

Derivation notes.

- First response = the earliest comment or review authored by a maintainer
  (the ``@logins`` of ``MAINTAINERS.md``) that is not authored by a bot. An
  item with none reads ``no response yet`` — never zero, never dropped. A
  response that arrives after the item was closed still counts and its detail
  row is marked ``post-close``.
- First merged pull request is derived from the whole merged history, never
  from ``author_association``: GitHub computes that field at query time, so a
  merged pull request's author always reads ``CONTRIBUTOR`` (measured on
  #1082, the first merge of an outside contributor, 2026-10-21). The earliest
  ``mergedAt`` per author decides; bots and maintainers are excluded; the
  fetch fails when it reaches the page limit instead of truncating.
- Bots are the logins ending in ``[bot]``; they are excluded from first
  responses and first merges, and the report lists every login treated as a
  bot so the rule is auditable.
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import statistics
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
MAINTAINERS_MD = ROOT / "MAINTAINERS.md"
GH = "gh"
FETCH_LIMIT = 5000
LIST_LIMIT = 2000
# The search-backed listings (`gh <kind> list --search ...`) stop at the
# search API's 1000-result ceiling no matter what --limit asks for; a busy
# window is undercounted silently unless the cap fails loudly.
SEARCH_RESULT_CAP = 1000
AGING_DAYS = 30
BOT_SUFFIX = "[bot]"
_LOGIN = re.compile(r"@([A-Za-z0-9][A-Za-z0-9-]{0,38})")


def is_bot(login: str) -> bool:
    """The bot rule the report states: any login ending in ``[bot]``."""
    return login.endswith(BOT_SUFFIX)


def maintainer_logins() -> list[str]:
    """The ``@logins`` of MAINTAINERS.md — the repo's own single source."""
    text = MAINTAINERS_MD.read_text(encoding="utf-8")
    return sorted({m.group(1) for m in _LOGIN.finditer(text)})


def run_gh(args: list[str]) -> Any:
    """Run one gh query and return the parsed JSON.

    ``gh`` talks to the API and never runs git, so the caller's environment
    passes through untouched; the executable is allowlisted and the call is
    bounded per the process-execution invariant (ADR-1270).
    """
    done = run_command(
        [GH, *args],
        allowed_executables=(GH,),
        text=True,
        capture_output=True,
        check=True,
        timeout_seconds=120,
    )
    return json.loads(str(done.stdout))


def fetch_merged_pull_requests(repo: str, limit: int = FETCH_LIMIT) -> list[dict[str, Any]]:
    """The whole merged history: author and mergedAt per pull request.

    Fails when the fetched count reaches ``limit`` — a silently truncated
    history would misreport first-time contributors (issue #2554).
    """
    done = run_gh(
        [
            "pr",
            "list",
            "--repo",
            repo,
            "--state",
            "merged",
            "--json",
            "author,mergedAt",
            "--limit",
            str(limit),
        ]
    )
    if len(done) >= limit:
        raise RuntimeError(
            f"merged history reached the fetch limit of {limit}; raise the limit "
            "instead of trusting a truncated first-merge derivation"
        )
    return done


def _list(repo: str, kind: str, search: str) -> list[dict[str, Any]]:
    """One search-backed window listing; fails at the cap instead of undercounting."""
    rows = run_gh(
        [
            kind,
            "list",
            "--repo",
            repo,
            "--state",
            "all",
            "--search",
            search,
            "--json",
            "number,title,author,createdAt,closedAt,labels",
            "--limit",
            str(LIST_LIMIT),
        ]
    )
    if len(rows) >= min(LIST_LIMIT, SEARCH_RESULT_CAP):
        raise RuntimeError(
            f"{kind} list for {search!r} reached its result cap of "
            f"{min(LIST_LIMIT, SEARCH_RESULT_CAP)}; narrow the window instead "
            "of trusting an undercounted opened/closed total"
        )
    return rows


def fetch_window_items(repo: str, since: str, until: str) -> list[dict[str, Any]]:
    """Issues and pull requests opened or closed inside ``[since, until]``.

    One record per item (a pair opened and closed inside the window appears
    once), carrying ``opened_in_window`` and ``closed_in_window`` flags.
    """
    items: dict[int, dict[str, Any]] = {}
    for kind in ("issue", "pr"):
        for field, search in (
            ("opened_in_window", f"created:{since}..{until}"),
            ("closed_in_window", f"closed:{since}..{until}"),
        ):
            for row in _list(repo, kind, search):
                record = items.setdefault(
                    int(row["number"]),
                    {
                        **row,
                        "is_pr": kind == "pr",
                        "opened_in_window": False,
                        "closed_in_window": False,
                    },
                )
                record[field] = True
    return sorted(items.values(), key=lambda row: int(row["number"]))


def fetch_first_response_inputs(
    repo: str, item: dict[str, Any]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Conversation comments and, for a pull request, reviews of one item.

    Conversation comments live on the ``issues`` endpoint for both kinds: the
    ``pulls/comments`` endpoint only returns inline review comments, so a
    maintainer's plain reply would be invisible. Both queries paginate —
    ``--paginate --slurp`` yields the pages as JSON and the rows are mapped
    here, because ``--jq`` runs once per page and cannot be combined with
    ``--slurp``. A ``PENDING`` review carries no ``submitted_at`` and is
    skipped: it is not a response.
    """
    pages = run_gh(
        ["api", "--paginate", "--slurp", f"repos/{repo}/issues/{item['number']}/comments"]
    )
    comments = [
        {
            "author": str((row.get("user") or {}).get("login", "")),
            "createdAt": str(row["created_at"]),
        }
        for page in pages
        for row in page or []
        if row.get("created_at")
    ]
    reviews: list[dict[str, Any]] = []
    if item["is_pr"]:
        pages = run_gh(
            ["api", "--paginate", "--slurp", f"repos/{repo}/pulls/{item['number']}/reviews"]
        )
        reviews = [
            {
                "author": str((row.get("user") or {}).get("login", "")),
                "createdAt": str(row["submitted_at"]),
            }
            for page in pages
            for row in page or []
            if row.get("submitted_at")
        ]
    return comments, reviews


def fetch_open_pull_requests(repo: str) -> list[dict[str, Any]]:
    """Every open pull request: the aging metric is window-independent.

    An open pull request created long before the window is exactly the one
    the aging section exists to surface, so the list comes from its own query
    instead of the window listings.
    """
    rows = run_gh(
        [
            "pr",
            "list",
            "--repo",
            repo,
            "--state",
            "open",
            "--json",
            "number,title,createdAt",
            "--limit",
            str(LIST_LIMIT),
        ]
    )
    if len(rows) >= LIST_LIMIT:
        raise RuntimeError(
            f"open pull request list reached the fetch limit of {LIST_LIMIT}; "
            "raise the limit instead of trusting a truncated aging section"
        )
    return rows


def first_response(
    item: dict[str, Any],
    comments: list[dict[str, Any]],
    reviews: list[dict[str, Any]],
    maintainers: set[str],
) -> dict[str, Any] | None:
    """The earliest maintainer response, or None when the item has none."""
    candidates = []
    for event in (*comments, *reviews):
        login = str(event["author"])
        if is_bot(login) or login not in maintainers:
            continue
        candidates.append(datetime.datetime.fromisoformat(str(event["createdAt"])))
    if not candidates:
        return None
    created = datetime.datetime.fromisoformat(str(item["createdAt"]))
    closed = item.get("closedAt")
    first = min(candidates)
    return {
        "responder_seconds": (first - created).total_seconds(),
        "post_close": bool(closed) and first > datetime.datetime.fromisoformat(str(closed)),
    }


def percentile(values: list[float], pct: float) -> float | None:
    """Linear-interpolated percentile; None for an empty list, exact for n=1."""
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * pct / 100.0
    low, high = int(rank), min(int(rank) + 1, len(ordered) - 1)
    share = rank - low
    return ordered[low] * (1.0 - share) + ordered[high] * share


def days_between(iso_then: str, now: datetime.datetime) -> float:
    return (now - datetime.datetime.fromisoformat(iso_then)).total_seconds() / 86400.0


def first_time_contributors(
    merged: list[dict[str, Any]],
    window_start: datetime.datetime,
    window_end: datetime.datetime,
    maintainers: set[str],
) -> list[dict[str, Any]]:
    """Authors whose earliest merge falls inside the window (bots/maintainers out)."""
    earliest: dict[str, datetime.datetime] = {}
    for row in merged:
        login = str(row["author"]["login"])
        if is_bot(login) or login in maintainers or not row.get("mergedAt"):
            continue
        moment = datetime.datetime.fromisoformat(str(row["mergedAt"]))
        if login not in earliest or moment < earliest[login]:
            earliest[login] = moment
    return sorted(
        (
            {"login": login, "first_merged_at": moment.date().isoformat()}
            for login, moment in earliest.items()
            if window_start <= moment <= window_end
        ),
        key=lambda row: row["login"],
    )


def compute_report(
    *,
    repo: str,
    since: str,
    until: str,
    now: datetime.datetime,
    window_items: list[dict[str, Any]],
    merged: list[dict[str, Any]],
    open_pull_requests: list[dict[str, Any]],
    responses: dict[int, dict[str, Any] | None],
    maintainers: list[str],
    open_labels: dict[str, int],
) -> dict[str, Any]:
    """The pure core: every number of the report from the fetched shapes."""
    bots_seen = sorted(
        {str(row["author"]["login"]) for row in window_items if is_bot(str(row["author"]["login"]))}
        | {str(row["author"]["login"]) for row in merged if is_bot(str(row["author"]["login"]))}
    )
    maintainers_set = set(maintainers)
    response_rows = []
    for item in window_items:
        if not item["opened_in_window"]:
            continue
        answer = responses.get(int(item["number"]))
        response_rows.append(
            {
                "number": int(item["number"]),
                "title": str(item["title"]),
                "kind": "pull request" if item["is_pr"] else "issue",
                "state": "closed" if item.get("closedAt") else "open",
                "response": answer,
            }
        )
    answered = [
        row["response"]["responder_seconds"] / 86400.0 for row in response_rows if row["response"]
    ]
    aging = [
        {
            "number": int(item["number"]),
            "title": str(item["title"]),
            "age_days": round(days_between(str(item["createdAt"]), now), 1),
        }
        for item in open_pull_requests
        if days_between(str(item["createdAt"]), now) > AGING_DAYS
    ]
    window_start = datetime.datetime.fromisoformat(f"{since}T00:00:00+00:00")
    window_end = datetime.datetime.fromisoformat(f"{until}T23:59:59+00:00")
    return {
        "repo": repo,
        "window": {"since": since, "until": until},
        "generated_at": now.date().isoformat(),
        "maintainers": maintainers,
        "bots_seen": bots_seen,
        "opened": {
            "issues": sum(1 for i in window_items if i["opened_in_window"] and not i["is_pr"]),
            "pull_requests": sum(1 for i in window_items if i["opened_in_window"] and i["is_pr"]),
        },
        "closed": {
            "issues": sum(1 for i in window_items if i["closed_in_window"] and not i["is_pr"]),
            "pull_requests": sum(1 for i in window_items if i["closed_in_window"] and i["is_pr"]),
        },
        "first_response": {
            "population": len(response_rows),
            "answered": len(answered),
            "no_response_yet": sum(1 for row in response_rows if row["response"] is None),
            "median_days": round(statistics.median(answered), 2) if answered else None,
            "p90_days": round(percentile(answered, 90), 2) if answered else None,
            "post_close": sum(
                1 for row in response_rows if row["response"] and row["response"]["post_close"]
            ),
            "items": response_rows,
        },
        "open_by_label": open_labels,
        "aging_pull_requests": aging,
        "first_time_contributors": first_time_contributors(
            merged, window_start, window_end, maintainers_set
        ),
    }


def render_markdown(report: dict[str, Any]) -> str:
    """The human report; ``no response yet`` is a stated row, never a zero."""
    lines = [
        f"# Community metrics — {report['repo']}",
        f"Window {report['window']['since']} .. {report['window']['until']}"
        f" (generated {report['generated_at']})",
        "",
        f"- Opened: {report['opened']['issues']} issues, "
        f"{report['opened']['pull_requests']} pull requests",
        f"- Closed: {report['closed']['issues']} issues, "
        f"{report['closed']['pull_requests']} pull requests",
        f"- Maintainer first response (population {report['first_response']['population']}): "
        f"{report['first_response']['answered']} answered, "
        f"{report['first_response']['no_response_yet']} no response yet",
        f"- Median {report['first_response']['median_days']} d, "
        f"p90 {report['first_response']['p90_days']} d"
        f" ({report['first_response']['post_close']} post-close)",
        f"- Open `good first issue`: {report['open_by_label'].get('good first issue', 0)},"
        f" open `help wanted`: {report['open_by_label'].get('help wanted', 0)}",
        f"- Open pull requests older than {AGING_DAYS} d: {len(report['aging_pull_requests'])}",
        "- First merged pull request inside the window: "
        + (
            ", ".join(
                f"{c['login']} ({c['first_merged_at']})" for c in report["first_time_contributors"]
            )
            or "none"
        ),
        f"- Bots treated as bots: {', '.join(report['bots_seen']) or 'none'}",
        "",
        "<!-- item detail: no response yet is a row of its own, never a zero -->",
    ]
    for row in report["first_response"]["items"]:
        answer = row["response"]
        detail = (
            (
                f"answered in {answer['responder_seconds'] / 86400.0:.2f} d"
                + (" (post-close)" if answer["post_close"] else "")
            )
            if answer
            else "no response yet"
        )
        lines.append(
            f"- #{row['number']} {row['kind']} [{row['state']}]: {detail} — {row['title']}"
        )
    return "\n".join(lines) + "\n"


def fetch_open_label_counts(repo: str) -> dict[str, int]:
    """Current open counts for the two labelled queues; fails at the fetch limit."""
    counts: dict[str, int] = {}
    for label in ("good first issue", "help wanted"):
        rows = run_gh(
            [
                "issue",
                "list",
                "--repo",
                repo,
                "--state",
                "open",
                "--label",
                label,
                "--json",
                "number",
                "--limit",
                str(LIST_LIMIT),
            ]
        )
        if len(rows) >= LIST_LIMIT:
            raise RuntimeError(
                f"label {label!r} list reached the fetch limit of {LIST_LIMIT}; "
                "raise the limit instead of reporting a capped count as exact"
            )
        counts[label] = len(rows)
    return counts


def main(argv: list[str] | None = None) -> int:
    today = datetime.datetime.now(datetime.timezone.utc)
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default="", help="OWNER/REPO; default: gh's current repository")
    parser.add_argument("--since", required=True, help="window start, YYYY-MM-DD (inclusive)")
    parser.add_argument(
        "--until", default="", help="window end, YYYY-MM-DD (inclusive; default today)"
    )
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    args = parser.parse_args(argv)
    until = args.until or today.date().isoformat()
    repo = args.repo or run_gh(["repo", "view", "--json", "nameWithOwner"])["nameWithOwner"]

    maintainers = maintainer_logins()
    window_items = fetch_window_items(repo, args.since, until)
    merged = fetch_merged_pull_requests(repo)
    open_pull_requests = fetch_open_pull_requests(repo)
    responses: dict[int, dict[str, Any] | None] = {}
    for item in window_items:
        if not item["opened_in_window"]:
            continue
        comments, reviews = fetch_first_response_inputs(repo, item)
        responses[int(item["number"])] = first_response(item, comments, reviews, set(maintainers))
    open_labels = fetch_open_label_counts(repo)
    report = compute_report(
        repo=repo,
        since=args.since,
        until=until,
        now=today,
        window_items=window_items,
        merged=merged,
        open_pull_requests=open_pull_requests,
        responses=responses,
        maintainers=maintainers,
        open_labels=open_labels,
    )
    print(render_markdown(report) if args.format == "markdown" else json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
