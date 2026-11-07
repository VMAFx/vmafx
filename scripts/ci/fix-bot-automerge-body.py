#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Re-arm a bot pull request's platform auto-merge with an explicit sign-off body.

Renovate's ``commitBody`` option adds a ``Signed-off-by`` trailer to individual
branch commits, but does NOT pass a ``commitBody`` to the GitHub
``enablePullRequestAutoMerge`` mutation.  When the repository's squash-merge
commit-message setting is ``BLANK``, the landed squash commit would have an
empty body — failing the ``check-dco.py --landed`` gate on master.

This script detects a bot pull request whose platform auto-merge has a null
commit body and re-arms it with an explicit body containing the bot's sign-off.
The ``check-bot-squash-body.py`` pre-merge gate then passes because the body is
non-null, and the landed commit carries the sign-off.

Environment variables (from the workflow event payload):

- ``PR_NUMBER`` — pull request number
- ``PR_NODE_ID`` — pull request GraphQL node ID
- ``PR_TITLE`` — pull request title
- ``PR_AUTHOR`` — pull request author login (e.g. ``renovate[bot]``)
- ``PR_AUTHOR_TYPE`` — ``Bot`` or ``User``
- ``PR_AUTHOR_ID`` — pull request author database ID (for the noreply address)
- ``AUTO_MERGE_BODY`` — auto-merge commit body (empty string when null)

Exit: 0 re-armed or skipped (not applicable), 1 re-arm failed, 2 usage error.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys

GH = shutil.which("gh") or "/usr/bin/gh"
GH_TIMEOUT_S = 30

MUTATION = """\
mutation($id: ID!, $headline: String!, $body: String!) {
  enablePullRequestAutoMerge(input: {
    pullRequestId: $id,
    mergeMethod: SQUASH,
    commitHeadline: $headline,
    commitBody: $body
  }) {
    pullRequest {
      autoMergeRequest {
        commitBody
      }
    }
  }
}"""


def _call_mutation(node_id: str, headline: str, signoff: str) -> tuple[int, str | None]:
    """Call the GraphQL mutation. Returns (exit_code, new_body)."""
    try:
        result = subprocess.run(  # noqa: S603 — fixed argv
            [
                GH,
                "api",
                "graphql",
                "-f",
                f"query={MUTATION}",
                "-f",
                f"id={node_id}",
                "-f",
                f"headline={headline}",
                "-f",
                f"body={signoff}",
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=GH_TIMEOUT_S,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"fix-bot-automerge-body: gh api call failed: {exc}", file=sys.stderr)
        return 1, None

    if result.returncode != 0:
        print(f"fix-bot-automerge-body: gh api exited {result.returncode}", file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        return 1, None

    try:
        data = json.loads(result.stdout)
        new_body = (
            data.get("data", {})
            .get("enablePullRequestAutoMerge", {})
            .get("pullRequest", {})
            .get("autoMergeRequest", {})
            .get("commitBody")
        )
    except (json.JSONDecodeError, AttributeError):  # fmt: skip
        new_body = None

    return 0, new_body


def main() -> int:
    author_type = os.environ.get("PR_AUTHOR_TYPE", "")
    if author_type != "Bot":
        print("fix-bot-automerge-body: SKIP — not a bot pull request.")
        return 0

    body = os.environ.get("AUTO_MERGE_BODY", "").strip()
    if body:
        print("fix-bot-automerge-body: SKIP — auto-merge body already set.")
        return 0

    node_id = os.environ.get("PR_NODE_ID", "")
    number = os.environ.get("PR_NUMBER", "")
    title = os.environ.get("PR_TITLE", "")
    author = os.environ.get("PR_AUTHOR", "")
    author_id = os.environ.get("PR_AUTHOR_ID", "")

    if not node_id or not number or not title or not author or not author_id:
        print("fix-bot-automerge-body: missing required env vars", file=sys.stderr)
        return 2

    headline = f"{title} (#{number})"
    signoff = f"Signed-off-by: {author} <{author_id}+{author}@users.noreply.github.com>"

    print(f"fix-bot-automerge-body: re-arming #{number} with sign-off body.")
    print(f"  headline: {headline}")
    print(f"  body: {signoff}")

    rc, new_body = _call_mutation(node_id, headline, signoff)
    if rc != 0:
        return rc

    if new_body and "Signed-off-by" in new_body:
        print(f"fix-bot-automerge-body: OK — auto-merge body set to: {new_body!r}")
        return 0

    print(f"fix-bot-automerge-body: mutation succeeded but body is {new_body!r}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
