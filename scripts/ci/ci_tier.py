#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Decide which tier of the CI suite an event owes (ADR-2169, ADR-3536).

The definition lives in ``.github/ci-tier.json``; this script is the one
reader of it that decides a tier, and ``.github/workflows/ci-tier.yml`` hands
the answer to every workflow. Three tiers:

``full``
    Every event that is not a pull request from this repository (a master
    push, a dispatch, a schedule), every fork pull request, an own pull
    request carrying the ``full`` label, an own pull request whose changed
    paths match ``full_paths`` or change the tier definition (or whose changed
    paths are unavailable), and a release pull request carrying the ``cut`` label.
``light``
    A pull request whose head repository is this repository, Renovate
    included, when none of its changed paths matches ``full_paths``.
``release-light``
    The machine-generated release pull request without the cut label. The
    release head ref alone is not trusted: ``release-pr-exempt.sh`` (ADR-1151,
    ADR-1388) verifies the author and, for the maintainer's token, the diff.

``--check-full-paths`` verifies against ``git ls-files`` that every pattern in
``full_paths`` matches at least one tracked file.

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
import fnmatch
import http.client
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
TIER_CONFIG_PATH = ".github/ci-tier.json"
DEFAULT_CONFIG = REPO_ROOT / TIER_CONFIG_PATH
EXEMPT_SCRIPT = REPO_ROOT / "scripts" / "ci" / "release-pr-exempt.sh"
API_TIMEOUT_S = 30
API_MAX_BYTES = 32 << 20  # a 100-entry API page is well under 1 MiB
PAGE_SIZE = 100
MAX_PAGES = 30
MAX_FULL_PATHS = 256
MAX_CHANGED_PATHS = PAGE_SIZE * MAX_PAGES
MAX_TRACKED_FILES = 200_000
PULL_REQUEST_EVENTS = frozenset({"pull_request", "pull_request_target"})
_BASH_PATH = shutil.which("bash")
BASH = str(Path(_BASH_PATH).resolve(strict=True)) if _BASH_PATH is not None else None
_GIT_PATH = shutil.which("git")
GIT = str(Path(_GIT_PATH).resolve(strict=True)) if _GIT_PATH is not None else None


def _https_opener() -> urllib.request.OpenerDirector:
    """An opener that speaks https only and follows no redirect.

    ``urlopen`` also opens ``file:``, ``ftp:``, ``data:`` and plain ``http:``
    addresses. This opener has no handler for them, so a caller cannot reach
    one: the address fails as an unknown URL type. It has no redirect handler
    either, so a 3xx answer is an ``HTTPError``: urllib's redirect handler
    would resend the ``Authorization`` header to whichever host the
    ``Location`` names, and every API these scripts call answers 200 directly.
    """
    opener = urllib.request.OpenerDirector()
    for handler in (
        urllib.request.ProxyHandler(),
        urllib.request.UnknownHandler(),
        urllib.request.HTTPSHandler(),
        urllib.request.HTTPDefaultErrorHandler(),
        urllib.request.HTTPErrorProcessor(),
    ):
        opener.add_handler(handler)
    return opener


HTTPS_OPENER = _https_opener()

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
        """Workflow output dictionary for ``--github-output``."""
        return {
            "tier": self.tier,
            "light": "true" if self.tier in (LIGHT, FULL) else "false",
            "full": "true" if self.tier == FULL else "false",
            "reason": self.reason,
        }


def load_config(path: Path = DEFAULT_CONFIG) -> dict[str, Any]:
    """Read the tier definition and refuse a malformed one."""
    config: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    for key in ("labels", "release_ref_prefix", "always", "full_only", "full_paths"):
        if key not in config:
            raise TierError(f"{path}: missing key {key!r}")
    for label in ("full", "cut"):
        if not isinstance(config["labels"].get(label), str):
            raise TierError(f"{path}: labels.{label} must be a string")
    overlap = set(config["always"]) & set(config["full_only"])
    if overlap:
        raise TierError(f"{path}: names in both always and full_only: {sorted(overlap)}")
    check_full_paths_config(path, config)
    check_own_input_lanes(path, config)
    return config


def check_full_paths_config(path: Path, config: Mapping[str, Any]) -> None:
    """Refuse a malformed ``full_paths`` list (ADR-3536)."""
    globs = config["full_paths"]
    if not isinstance(globs, list):
        raise TierError(f"{path}: full_paths must be a list")
    if len(globs) > MAX_FULL_PATHS:
        raise TierError(f"{path}: full_paths exceeds {MAX_FULL_PATHS} entries")
    seen: set[str] = set()
    for index in range(len(globs)):
        entry = globs[index]
        if not isinstance(entry, str) or not entry.strip() or "\n" in entry:
            raise TierError(f"{path}: full_paths entries must be non-empty single-line strings")
        if entry in seen:
            raise TierError(f"{path}: duplicate full_paths entry {entry!r}")
        seen.add(entry)


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


def _check_path_bounds(paths: Sequence[str], globs: Sequence[str]) -> None:
    """Bound both inputs before config-path escalation or glob matching."""
    if len(paths) > MAX_CHANGED_PATHS:
        raise TierError(f"more than {MAX_CHANGED_PATHS} changed paths")
    if len(globs) > MAX_FULL_PATHS:
        raise TierError(f"more than {MAX_FULL_PATHS} full_paths entries")


def _matching_full_path(paths: Sequence[str], globs: Sequence[str]) -> tuple[str, str] | None:
    """Return the first ``(changed_path, glob)`` pair matched under ``fnmatchcase``."""
    _check_path_bounds(paths, globs)
    for path_idx in range(len(paths)):
        candidate = paths[path_idx]
        for glob_idx in range(len(globs)):
            pattern = globs[glob_idx]
            if fnmatch.fnmatchcase(candidate, pattern):
                return candidate, pattern
    return None


def _decide_own_pull_request(
    config: Mapping[str, Any],
    changed_paths: Callable[[], Sequence[str] | None],
) -> Decision:
    """Decide between ``full`` and ``light`` for a non-exempt own pull request."""
    full_paths: Sequence[str] = config.get("full_paths", ())
    paths = changed_paths()
    if paths is None:
        return Decision(FULL, "changed paths unavailable")
    _check_path_bounds(paths, full_paths)
    if TIER_CONFIG_PATH in paths:
        return Decision(FULL, f"changed tier configuration {TIER_CONFIG_PATH!r}")
    matched = _matching_full_path(paths, full_paths)
    if matched is not None:
        path, glob = matched
        return Decision(FULL, f"changed path {path!r} matches full_paths {glob!r}")
    return Decision(LIGHT, "pull request from this repository")


def decide(
    event: Event,
    config: Mapping[str, Any],
    *,
    release_exempt: Callable[[], bool],
    changed_paths: Callable[[], Sequence[str] | None] = lambda: None,
) -> Decision:
    """Return the tier an event owes; callables run only for branches that need them."""
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
    return _decide_own_pull_request(config, changed_paths)


def api_request(method: str, url: str, token: str, payload: object | None = None) -> object:
    """One bounded GitHub API call; the only network access of the tier scripts.

    ``payload`` goes out as the JSON request body; without one a POST sends
    an empty body.
    """
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    data = b"" if method == "POST" else None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    return https_json(method, url, headers, data, API_TIMEOUT_S, API_MAX_BYTES)


def https_json(
    method: str,
    url: str,
    headers: Mapping[str, str],
    data: bytes | None,
    timeout_s: float,
    limit: int,
) -> object:
    """One https request whose answer is JSON, bounded in time and in size.

    The scheme check and the size cap live here so every caller gets them; an
    empty answer reads as an empty object. A broken answer (cut off, a bad
    status line, an overlong header) raises ``http.client.HTTPException``,
    which is no ``OSError``; it becomes a ``TierError`` here so every caller
    treats it as the failed request it is. An error status stays an
    ``HTTPError`` for the caller to read its code.
    """
    if urllib.parse.urlsplit(url).scheme != "https":
        raise TierError(f"refusing a non-https API URL: {url}")
    request = urllib.request.Request(  # noqa: S310 - the scheme is checked above
        url, method=method, data=data, headers=dict(headers)
    )
    try:
        with HTTPS_OPENER.open(request, timeout=timeout_s) as response:
            body = response.read(limit + 1)
    except http.client.HTTPException as exc:
        raise TierError(f"{url}: broken answer ({type(exc).__name__}: {exc})") from exc
    if len(body) > limit:
        raise TierError(f"{url}: answer larger than {limit} bytes")
    return json.loads(body.decode("utf-8")) if body else {}


def _paged(fetch: Fetcher, path: str) -> list[object]:
    """Read every page of a paginated API collection up to ``MAX_PAGES``."""
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
    """Paths a pull request changes, for release-PR and ``full_paths`` checks."""
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
    """Append ``name=value`` lines to ``path``, refusing multi-line values."""
    with path.open("a", encoding="utf-8") as handle:
        for name, value in outputs.items():
            if "\n" in value:
                raise TierError(f"output {name} would span lines")
            handle.write(f"{name}={value}\n")


def _tracked_files(root: Path) -> tuple[str, ...]:
    """Return every path ``git ls-files`` reports under ``root``."""
    if GIT is None:
        raise TierError("required executable not found: git")
    resolved_root = root.resolve()
    git_env = (
        None
        if resolved_root == REPO_ROOT.resolve()
        else {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    )
    try:
        result = run_command(
            [GIT, "-C", str(resolved_root), "ls-files", "-z"],
            allowed_executables=(GIT,),
            env=git_env,
            text=True,
            capture_output=True,
            check=True,
            timeout_seconds=API_TIMEOUT_S,
        )
    except (OSError, RuntimeError, ValueError) as exc:
        raise TierError(f"{resolved_root}: git ls-files failed ({exc})") from exc
    raw = str(result.stdout).split("\0")
    if len(raw) > MAX_TRACKED_FILES:
        raise TierError(f"{resolved_root}: more than {MAX_TRACKED_FILES} tracked files")
    return tuple(raw[idx] for idx in range(len(raw)) if raw[idx])


def unmatched_full_paths(config_path: Path = DEFAULT_CONFIG, root: Path = REPO_ROOT) -> list[str]:
    """Return every ``full_paths`` glob that matches no tracked file under ``root``."""
    config = load_config(config_path)
    tracked = _tracked_files(root)
    globs: list[str] = list(config["full_paths"])
    missing: list[str] = []
    for glob_idx in range(len(globs)):
        pattern = globs[glob_idx]
        if not any(
            fnmatch.fnmatchcase(tracked[file_idx], pattern) for file_idx in range(len(tracked))
        ):
            missing.append(pattern)
    return missing


def check_full_paths(config_path: Path = DEFAULT_CONFIG, root: Path = REPO_ROOT) -> int:
    """Verify every ``full_paths`` glob matches at least one tracked file."""
    try:
        missing = unmatched_full_paths(config_path, root)
    except (TierError, OSError, ValueError) as exc:
        print(f"ci-tier: {exc}", file=sys.stderr)
        return 1
    if missing:
        for idx in range(len(missing)):
            print(
                f"ci-tier: full_paths glob {missing[idx]!r} matches no tracked file in {root}",
                file=sys.stderr,
            )
        return 1
    return 0


def run(env: Mapping[str, str], config_path: Path, fetch: Fetcher | None) -> Decision:
    """Decide the tier for one workflow environment, fetching PR files at most once."""
    config = load_config(config_path)
    event = event_from_environment(env, fetch)
    number = env.get("PR_NUMBER", "")
    cached_paths: list[str] | None = None
    fetched = False

    def paths_or_none() -> Sequence[str] | None:
        """Fetch and cache pull-request changed paths, or ``None`` on failure."""
        nonlocal cached_paths, fetched
        if fetched:
            return cached_paths
        fetched = True
        if fetch is None or not number or not event.repository:
            return None
        try:
            cached_paths = changed_paths(fetch, event.repository, number)
        except (TierError, OSError, ValueError, urllib.error.URLError) as exc:
            print(f"ci-tier: changed paths unavailable ({exc})", file=sys.stderr)
            cached_paths = None
        return cached_paths

    def exempt() -> bool:
        """Evaluate ``release-pr-exempt.sh`` using the shared changed-paths cache."""
        paths = paths_or_none()
        if paths is None:
            return False
        return release_exempt_from_script(event, paths)

    return decide(event, config, release_exempt=exempt, changed_paths=paths_or_none)


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point for workflow tier decisions and ``--check-full-paths``."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", type=Path, default=None)
    parser.add_argument("--github-output", type=Path, help="append name=value lines here")
    parser.add_argument(
        "--check-full-paths",
        action="store_true",
        help="verify every full_paths glob matches at least one tracked file",
    )
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    args = parser.parse_args(argv)
    config_path = args.config or (
        args.repo_root / TIER_CONFIG_PATH if args.check_full_paths else DEFAULT_CONFIG
    )
    if args.check_full_paths:
        return check_full_paths(config_path, args.repo_root)
    token = os.environ.get("GH_TOKEN", "")
    api_url = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    fetch: Fetcher | None = (
        (lambda path: api_request("GET", api_url + path, token)) if token else None
    )
    try:
        decision = run(os.environ, config_path, fetch)
    except (TierError, OSError, ValueError, urllib.error.URLError) as exc:
        # Fail closed: a tier that cannot be decided must not quietly shrink the suite.
        print(f"ci-tier: {exc}; running the full suite", file=sys.stderr)
        decision = Decision(FULL, f"tier could not be decided ({exc})")
    outputs = decision.outputs()
    # The aggregator reads the contexts a tier does not owe from the same file.
    config = load_config(config_path)
    outputs["always_json"] = json.dumps(config["always"])
    outputs["full_only_json"] = json.dumps(config["full_only"])
    print(f"ci-tier: tier={outputs['tier']} ({decision.reason})")
    if args.github_output is not None:
        write_outputs(args.github_output, outputs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
