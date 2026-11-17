#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail an edited Rules run when its pull request text has been superseded."""

from __future__ import annotations

import json
import os
import sys
import time
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Callable, Mapping

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import ci_tier

SETTLE_S = 5
MAX_WAIT_S = 30


def settle_wait_s(stamp: str, now: datetime) -> float:
    """Return the bounded delay before reading the pull request."""
    parsed = datetime.fromisoformat(stamp.replace("Z", "+00:00"))
    if parsed.tzinfo is None or now.tzinfo is None:
        raise ValueError("timestamp must include a timezone")
    return max(
        0.0, min(float(MAX_WAIT_S), (parsed + timedelta(seconds=SETTLE_S) - now).total_seconds())
    )


def _fields(pr: object) -> tuple[str, str, str]:
    if not isinstance(pr, dict) or "body" not in pr or not isinstance(pr.get("base"), dict):
        raise ValueError("pull request or base is not an object")
    title, body, base = pr.get("title"), pr.get("body"), pr["base"].get("ref")
    if (
        not isinstance(title, str)
        or not isinstance(base, str)
        or not (body is None or isinstance(body, str))
    ):
        raise ValueError("invalid pull request title, body or base.ref")
    return title, body or "", base


def decide(payload_pr: object, live_pr: object) -> list[str]:
    """Name the event fields that differ from the current pull request."""
    before, after = _fields(payload_pr), _fields(live_pr)
    return [
        name
        for name, old, new in zip(("title", "body", "base.ref"), before, after, strict=True)
        if old != new
    ]


def _event_data(payload: dict, env: Mapping[str, str]) -> tuple[dict, int, str, str, str]:
    pr = payload.get("pull_request")
    if not isinstance(pr, dict):
        raise ValueError("missing pull request")
    number, stamp = payload.get("number"), pr.get("updated_at")
    token, repo = env.get("GH_TOKEN"), env.get("GITHUB_REPOSITORY")
    if not isinstance(number, int) or isinstance(number, bool) or number <= 0:
        raise ValueError("missing pull request number")
    if not isinstance(stamp, str) or not stamp or not token or not repo:
        raise ValueError("missing stamp, token or repository")
    _fields(pr)
    return pr, number, stamp, token, repo


def run(
    env: Mapping[str, str],
    fetch: Callable[..., object],
    sleep: Callable[[float], None],
    now: Callable[[], datetime],
) -> int:
    """Check the event using injectable API, sleep and clock functions."""
    try:
        payload = json.loads(Path(env["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        if not isinstance(payload, dict) or not isinstance(payload.get("action"), str):
            raise ValueError("invalid event payload")
        if payload["action"] != "edited":
            print("::notice::not an edited event; supersede guard skipped")
            return 0
        pr, number, stamp, token, repo = _event_data(payload, env)
        sleep(settle_wait_s(stamp, now()))
        api_url = env.get("GITHUB_API_URL", "https://api.github.com").rstrip("/")
        live = fetch("GET", f"{api_url}/repos/{repo}/pulls/{number}", token)
        changed = decide(pr, live)
        if changed:
            print(
                f"::error::superseded by a later edit: {', '.join(changed)} changed since the event at {stamp}; the Rules run of the later edit judges the current text"
            )
            return 1
        return 0
    except (KeyError, TypeError, ci_tier.TierError, OSError, ValueError) as exc:
        print(f"::error::supersede guard could not check the event: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(run(os.environ, ci_tier.api_request, time.sleep, lambda: datetime.now(UTC)))
