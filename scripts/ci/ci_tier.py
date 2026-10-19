#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Decide which tier of the CI suite an event owes (ADR-2169).

The definition lives in ``.github/ci-tier.json``; this script is the one
reader of it that decides a tier, and ``.github/workflows/ci-tier.yml`` hands
the answer to every workflow. Three tiers:

``full``
    Every event that is not a pull request from this repository (a master
    push, a dispatch, a schedule), every fork pull request, an own pull
    request carrying the ``full`` label, and a release pull request carrying
    the ``cut`` label.
``light``
    A pull request whose head repository is this repository, Renovate
    included.
``release-light``
    The machine-generated release pull request without the cut label. The
    release head ref alone is not trusted: ``release-pr-exempt.sh`` (ADR-1151,
    ADR-1388) verifies the author and, for the maintainer's token, the diff.

Outputs (``--github-output``, one ``name=value`` per line)::

    tier   full | light | release-light
    light  true when light-tier jobs run (tier light or full)
    full   true when full-tier jobs run (tier full)
    reason why

The labels of a pull request are read live from the API when a token and a
pull-request number are present: a re-run of a workflow keeps the labels of
the original event, and the escalation workflow re-runs the suite exactly
after a label changed.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO_ROOT / ".github" / "ci-tier.json"
EXEMPT_SCRIPT = REPO_ROOT / "scripts" / "ci" / "release-pr-exempt.sh"
API_TIMEOUT_S = 30
PAGE_SIZE = 100
MAX_PAGES = 30
PULL_REQUEST_EVENTS = frozenset({"pull_request", "pull_request_target"})
_BASH_PATH = shutil.which("bash")
BASH = str(Path(_BASH_PATH).resolve(strict=True)) if _BASH_PATH is not None else None

FULL = "full"
LIGHT = "light"
RELEASE_LIGHT = "release-light"

Fetcher = Callable[[str], object]


class TierError(RuntimeError):
    """The tier cannot be decided; the caller must not guess."""


@dataclass(frozen=True)
class Event:
    """The facts of one workflow event that the tier depends on."""

    name: str
    repository: str
    head_repository: str
    head_ref: str
    author: str
    author_type: str
    labels: tuple[str, ...]


@dataclass(frozen=True)
class Decision:
    tier: str
    reason: str

    def outputs(self) -> dict[str, str]:
        return {
            "tier": self.tier,
            "light": "true" if self.tier in (LIGHT, FULL) else "false",
            "full": "true" if self.tier == FULL else "false",
            "reason": self.reason,
        }


def load_config(path: Path = DEFAULT_CONFIG) -> dict[str, Any]:
    """Read the tier definition and refuse a malformed one."""
    config: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    for key in ("labels", "release_ref_prefix", "always", "full_only"):
        if key not in config:
            raise TierError(f"{path}: missing key {key!r}")
    for label in ("full", "cut"):
        if not isinstance(config["labels"].get(label), str):
            raise TierError(f"{path}: labels.{label} must be a string")
    overlap = set(config["always"]) & set(config["full_only"])
    if overlap:
        raise TierError(f"{path}: names in both always and full_only: {sorted(overlap)}")
    check_own_input_lanes(path, config)
    return config


def check_own_input_lanes(path: Path, config: Mapping[str, Any]) -> None:
    """Refuse an own-input lane that names a context the tier would not skip (ADR-2198)."""
    lanes = config.get("own_input_lanes", [])
    if not isinstance(lanes, list):
        raise TierError(f"{path}: own_input_lanes must be a list")
    for lane in lanes:
        if not isinstance(lane, dict):
            raise TierError(f"{path}: own_input_lanes entries must be objects")
        selectors = lane.get("selectors")
        if not (
            isinstance(lane.get("workflow"), str)
            and isinstance(lane.get("context"), str)
            and isinstance(lane.get("reason"), str)
            and lane["reason"].strip()
            and isinstance(selectors, list)
            and selectors
            and all(isinstance(name, str) for name in selectors)
        ):
            raise TierError(
                f"{path}: own_input_lanes needs workflow, context, selectors and a reason: {lane}"
            )
        if lane["context"] not in config["full_only"]:
            raise TierError(f"{path}: own_input_lanes context {lane['context']!r} is not full_only")


def decide(
    event: Event,
    config: Mapping[str, Any],
    *,
    release_exempt: Callable[[], bool],
) -> Decision:
    """Return the tier an event owes. ``release_exempt`` is called only for a release head ref."""
    if event.name not in PULL_REQUEST_EVENTS:
        return Decision(FULL, f"event {event.name} is not a pull request")
    if not event.head_repository or event.head_repository != event.repository:
        return Decision(FULL, f"fork pull request (head repository {event.head_repository!r})")
    labels = config["labels"]
    if labels["full"] in event.labels:
        return Decision(FULL, f"label {labels['full']!r}")
    if event.head_ref.startswith(str(config["release_ref_prefix"])) and release_exempt():
        if labels["cut"] in event.labels:
            return Decision(FULL, f"release pull request with label {labels['cut']!r}")
        return Decision(RELEASE_LIGHT, "release pull request without the cut label")
    return Decision(LIGHT, "pull request from this repository")


def api_request(method: str, url: str, token: str) -> object:
    """One bounded GitHub API call; the only network access of the tier scripts."""
    if urllib.parse.urlsplit(url).scheme != "https":
        raise TierError(f"refusing a non-https API URL: {url}")
    request = urllib.request.Request(  # noqa: S310 - the scheme is checked above
        url,
        method=method,
        data=b"" if method == "POST" else None,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with urllib.request.urlopen(request, timeout=API_TIMEOUT_S) as response:  # noqa: S310
        body = response.read().decode("utf-8")
    return json.loads(body) if body else {}


def _paged(fetch: Fetcher, path: str) -> list[object]:
    items: list[object] = []
    for page in range(1, MAX_PAGES + 1):
        chunk = fetch(f"{path}?per_page={PAGE_SIZE}&page={page}")
        if not isinstance(chunk, list):
            raise TierError(f"{path}: the API returned {type(chunk).__name__}, not a list")
        items.extend(chunk)
        if len(chunk) < PAGE_SIZE:
            return items
    raise TierError(f"{path}: more than {MAX_PAGES * PAGE_SIZE} entries")


def live_labels(fetch: Fetcher, repository: str, number: str) -> tuple[str, ...]:
    """Current label names of a pull request (an issue, to the API)."""
    rows = _paged(fetch, f"/repos/{repository}/issues/{number}/labels")
    return tuple(str(row["name"]) for row in rows if isinstance(row, dict) and "name" in row)


def changed_paths(fetch: Fetcher, repository: str, number: str) -> list[str]:
    """Paths a pull request changes, for the release-PR diff check."""
    rows = _paged(fetch, f"/repos/{repository}/pulls/{number}/files")
    return [str(row["filename"]) for row in rows if isinstance(row, dict) and "filename" in row]


def release_exempt_from_script(event: Event, paths: Sequence[str]) -> bool:
    """Ask ``release-pr-exempt.sh`` whether the pull request is the generated release PR."""
    with tempfile.TemporaryDirectory() as tmp:
        diff_file = Path(tmp) / "changed.txt"
        diff_file.write_text("\n".join(paths) + "\n", encoding="utf-8")
        env = {
            **os.environ,
            "HEAD_REF": event.head_ref,
            "PR_AUTHOR": event.author,
            "PR_AUTHOR_TYPE": event.author_type,
            "DIFF_FILE": str(diff_file),
        }
        if BASH is None:
            raise TierError("required executable not found: bash")
        result = run_command(
            [BASH, str(EXEMPT_SCRIPT)],
            allowed_executables=(BASH,),
            cwd=REPO_ROOT,
            env=env,
            text=True,
            capture_output=True,
            check=True,
            timeout_seconds=API_TIMEOUT_S,
        )
    return "exempt=true" in str(result.stdout)


def event_from_environment(env: Mapping[str, str], fetch: Fetcher | None) -> Event:
    """Collect the event facts from the workflow environment, labels live when possible."""
    repository = env.get("GITHUB_REPOSITORY", "")
    number = env.get("PR_NUMBER", "")
    parsed = json.loads(env.get("PR_LABELS", "") or "[]")
    payload = tuple(str(name) for name in parsed) if isinstance(parsed, list) else ()
    labels = payload
    if fetch is not None and number and repository:
        labels = live_labels(fetch, repository, number)
        print(f"ci-tier: labels from the API: {list(labels)}", file=sys.stderr)
    elif number:
        print(
            f"ci-tier: no API token, labels from the event payload: {list(payload)}",
            file=sys.stderr,
        )
    return Event(
        name=env.get("EVENT_NAME", ""),
        repository=repository,
        head_repository=env.get("HEAD_REPOSITORY", ""),
        head_ref=env.get("HEAD_REF", ""),
        author=env.get("PR_AUTHOR", ""),
        author_type=env.get("PR_AUTHOR_TYPE", ""),
        labels=labels,
    )


def write_outputs(path: Path, outputs: Mapping[str, str]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        for name, value in outputs.items():
            if "\n" in value:
                raise TierError(f"output {name} would span lines")
            handle.write(f"{name}={value}\n")


def run(env: Mapping[str, str], config_path: Path, fetch: Fetcher | None) -> Decision:
    config = load_config(config_path)
    event = event_from_environment(env, fetch)

    def exempt() -> bool:
        number = env.get("PR_NUMBER", "")
        paths = changed_paths(fetch, event.repository, number) if fetch and number else []
        return release_exempt_from_script(event, paths)

    return decide(event, config, release_exempt=exempt)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--github-output", type=Path, help="append name=value lines here")
    args = parser.parse_args(argv)
    token = os.environ.get("GH_TOKEN", "")
    api_url = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    fetch: Fetcher | None = (
        (lambda path: api_request("GET", api_url + path, token)) if token else None
    )
    try:
        decision = run(os.environ, args.config, fetch)
    except (TierError, OSError, ValueError, urllib.error.URLError) as exc:
        # Fail closed: a tier that cannot be decided must not quietly shrink the suite.
        print(f"ci-tier: {exc}; running the full suite", file=sys.stderr)
        decision = Decision(FULL, f"tier could not be decided ({exc})")
    outputs = decision.outputs()
    # The aggregator reads the contexts a tier does not owe from the same file.
    config = load_config(args.config)
    outputs["always_json"] = json.dumps(config["always"])
    outputs["full_only_json"] = json.dumps(config["full_only"])
    print(f"ci-tier: tier={outputs['tier']} ({decision.reason})")
    if args.github_output is not None:
        write_outputs(args.github_output, outputs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
