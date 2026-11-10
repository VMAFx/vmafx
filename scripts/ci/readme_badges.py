#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Read, publish and judge the README status badges (docs/development/readme-badges.md).

    readme_badges.py spec --spec SPEC.json --state DIR/state.json
        Read every badge source and write the render list for
        tools/badges/render.mjs plus the state file. Exit 0 once both exist.
    readme_badges.py publish --dir DIR
        Commit DIR/<slug>.svg and DIR/state.json to the badges branch.
    readme_badges.py verdict --state DIR/state.json
        Exit 1 when a source failed this run, so the run turns red.
    readme_badges.py check-readme [--readme README.md]
        Exit 1 when a README image is not a badge rendered onto this
        repository's badges branch, a shields.io badge in for-the-badge with
        the dark label, or a file in the repository. Offline; pre-commit runs
        it on every README change.

Sources: the latest decisive completed push run of each workflow on the
default branch (GitHub API with GITHUB_TOKEN), and the OpenSSF Scorecard
score, OpenSSF Best Practices level and Codacy grade (public APIs, sent no
token). A source that fails keeps its last value for six hours; after that
the badge reads "stale". The repository comes from GITHUB_REPOSITORY.
"""

from __future__ import annotations

import argparse
import base64
import functools
import json
import os
import re
import sys
import urllib.error
import urllib.parse
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from http import HTTPStatus
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import ci_tier
from scripts.dev.check_repository_security import POLICY as SECURITY_POLICY
from scripts.lib.safe_subprocess import (
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
)
from scripts.lib.safe_subprocess import run as run_command

API = "https://api.github.com"
BRANCH = "badges"
STATE = "state.json"
STALE_AFTER = timedelta(hours=6)
GIT_TIMEOUT_S = 30
OBJECT_ID = re.compile(r"[0-9a-f]{40}")
QUIET_WINDOW = timedelta(hours=1)
RUNS_PER_PAGE = 30
PUBLIC_TIMEOUT_S = 20
MAX_RESPONSE_BYTES = 1 << 20
MAX_FILE_BYTES = 64 << 10

GREEN = "#1f883d"
RED = "#cf222e"
AMBER = "#9a6700"
GREY = "#6e7781"
SCORE_MAX = 10  # OpenSSF Scorecard scores run from 0 to 10
# The unrounded score from which the badge is green: the master requirement in
# docs/principles.md (ADR-1247). The badge colours the public score only; the
# exact-head check stays the Scorecard workflow's job.
SCORE_GREEN = 8.5
SCORE_AMBER = 4.0  # unrounded score from which it is amber; red below
TEXT = re.compile(r"[\x20-\x7e]{1,40}")
COLOR = re.compile(r"#[0-9a-f]{6}")

PASSING = ("passing", GREEN)
FAILING = ("failing", RED)
DECISIVE = {
    "success": PASSING,
    "failure": FAILING,
    "timed_out": FAILING,
    "startup_failure": FAILING,
}
# Runs that say nothing about the code: no conclusion yet, cancelled or
# skipped. The badge looks past them. Any other conclusion of the newest run
# (action_required, neutral, stale) reads "unknown", never an older run's value.
# A tuple, not a set: membership then compares and never hashes the API value.
PASSED_OVER = (None, "cancelled", "skipped")
UNKNOWN = ("unknown", GREY)
BEST_PRACTICES = {
    "in_progress": ("in progress", AMBER),
    "passing": PASSING,
    "silver": ("silver", GREEN),
    "gold": ("gold", GREEN),
}
CODACY = {"A": GREEN, "B": GREEN, "C": AMBER, "D": RED, "E": RED, "F": RED}

Value = tuple[str, str]
GitHub = Callable[[str], object]
Public = Callable[[str], object]
Call = Callable[[str, str, object | None], object]


class BadgeError(Exception):
    """A badge source answered something the renderer cannot show."""


@dataclass(frozen=True)
class Badge:
    """One README badge: its file name, label and where its value comes from."""

    slug: str
    label: str
    kind: str
    workflow: str = ""


BADGES = (
    Badge("tests", "Tests", "workflow", "tests-and-quality-gates.yml"),
    Badge("builds", "Builds", "workflow", "libvmaf-build-matrix.yml"),
    Badge("lint", "Lint", "workflow", "lint-and-format.yml"),
    Badge("ffmpeg", "FFmpeg", "workflow", "ffmpeg-integration.yml"),
    Badge("go", "Go", "workflow", "go-ci.yml"),
    Badge("rust", "Rust", "workflow", "rust-ci.yml"),
    Badge("security", "Security", "workflow", "security-scans.yml"),
    Badge("scorecard", "OpenSSF Scorecard", "scorecard"),
    Badge("best-practices", "OpenSSF Best Practices", "best-practices"),
    Badge("codacy", "Codacy", "codacy"),
)


def stamp(moment: datetime) -> str:
    """UTC time as the state file stores it."""
    return moment.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


def workflow_value(runs: object) -> Value:
    """Value of the newest run that has a conclusion and was not cancelled or skipped.

    That run reads "passing" or "failing" when it passed or failed, "unknown"
    otherwise; so does an answer with no such run.
    """
    rows = runs.get("workflow_runs") if isinstance(runs, dict) else None
    if not isinstance(rows, list):
        raise BadgeError("the runs answer has no workflow_runs list")
    for row in rows:
        conclusion = row.get("conclusion") if isinstance(row, dict) else None
        if conclusion not in PASSED_OVER:
            return DECISIVE.get(conclusion, UNKNOWN) if isinstance(conclusion, str) else UNKNOWN
    return UNKNOWN


def scorecard_value(doc: object) -> Value:
    """OpenSSF Scorecard score out of 10: green from 8.5, amber from 4, red below.

    The colour follows the unrounded score, as the master requirement does, so
    8.46 reads "8.5 / 10" in amber.
    """
    score = doc.get("score") if isinstance(doc, dict) else None
    if isinstance(score, bool) or not isinstance(score, int | float) or not 0 <= score <= SCORE_MAX:
        raise BadgeError(f"Scorecard score {score!r} is not a number from 0 to 10")
    color = GREEN if score >= SCORE_GREEN else AMBER if score >= SCORE_AMBER else RED
    return (f"{float(score):.1f} / 10", color)


def best_practices_value(rows: object, repo_url: str) -> Value:
    """Badge level of the Best Practices project registered for ``repo_url``."""
    matches = (
        [r for r in rows if isinstance(r, dict) and r.get("repo_url") == repo_url]
        if isinstance(rows, list)
        else []
    )
    if len(matches) != 1:
        raise BadgeError(f"{len(matches)} Best Practices projects list {repo_url}")
    level = matches[0].get("badge_level")
    if level not in BEST_PRACTICES:
        raise BadgeError(f"Best Practices level {level!r} is not one this badge knows")
    return BEST_PRACTICES[level]


def codacy_value(doc: object) -> Value:
    """Codacy grade letter: A and B green, C amber, D to F red."""
    data = doc.get("data") if isinstance(doc, dict) else None
    letter = data.get("gradeLetter") if isinstance(data, dict) else None
    if letter not in CODACY:
        raise BadgeError(f"Codacy grade {letter!r} is not a letter from A to F")
    return (str(letter), CODACY[letter])


def default_branch(github: GitHub, repository: str) -> str:
    """Name of the repository's default branch, whose push runs the workflow badges show."""
    doc = github(f"/repos/{repository}")
    branch = doc.get("default_branch") if isinstance(doc, dict) else None
    if not isinstance(branch, str) or not branch:
        raise BadgeError(f"{repository}: the API names no default branch")
    return branch


def read_value(badge: Badge, repository: str, github: GitHub, public: Public) -> Value:
    """Ask the badge's source for its current value."""
    if badge.kind == "workflow":
        branch = default_branch(github, repository)
        query = urllib.parse.urlencode(
            {"branch": branch, "event": "push", "status": "completed", "per_page": RUNS_PER_PAGE}
        )
        return workflow_value(
            github(f"/repos/{repository}/actions/workflows/{badge.workflow}/runs?{query}")
        )
    owner, name = repository.split("/", 1)
    if badge.kind == "scorecard":
        return scorecard_value(
            public(f"https://api.scorecard.dev/projects/github.com/{repository}")
        )
    if badge.kind == "best-practices":
        repo_url = f"https://github.com/{repository}"
        query = urllib.parse.urlencode({"url": repo_url})
        return best_practices_value(
            public(f"https://www.bestpractices.dev/projects.json?{query}"), repo_url
        )
    if badge.kind == "codacy":
        path = f"organizations/gh/{owner}/repositories/{name}"
        return codacy_value(public(f"https://app.codacy.com/api/v3/analysis/{path}"))
    raise BadgeError(f"{badge.slug}: unknown source kind {badge.kind!r}")


def usable(entry: object) -> dict[str, str | None] | None:
    """A previous state entry when its shape is one this script writes, else ``None``."""
    if not isinstance(entry, dict):
        return None
    message, color, fetched = entry.get("message"), entry.get("color"), entry.get("fetched_at")
    if not (isinstance(message, str) and TEXT.fullmatch(message)):
        return None
    if not (isinstance(color, str) and COLOR.fullmatch(color)):
        return None
    if fetched is not None and not isinstance(fetched, str):
        return None
    return {"message": message, "color": color, "fetched_at": fetched}


def read_time(text: str) -> datetime | None:
    """The time ``text`` names in ISO 8601 with a UTC offset; ``None`` when unreadable or naive."""
    try:
        value = datetime.fromisoformat(text)
    except ValueError:
        return None
    return value if value.utcoffset() is not None else None


def carried(previous: Mapping[str, str | None] | None, now: datetime) -> dict[str, str | None]:
    """Keep the last value up to STALE_AFTER after it was read; then show "stale".

    A read time that is unreadable, names no UTC offset or lies in the future
    counts as stale.
    """
    fetched = previous.get("fetched_at") if previous else None
    read_at = read_time(fetched) if previous and fetched else None
    if previous and read_at is not None and timedelta(0) <= now - read_at <= STALE_AFTER:
        return dict(previous)
    return {"message": "stale", "color": GREY, "fetched_at": fetched}


def refresh(
    badges: tuple[Badge, ...],
    previous: Mapping[str, object],
    now: datetime,
    read: Callable[[Badge], Value],
) -> tuple[dict[str, dict[str, str | None]], list[str]]:
    """New state entries for every badge, and one line per source that failed."""
    entries: dict[str, dict[str, str | None]] = {}
    failures: list[str] = []
    for badge in badges:
        try:
            message, color = read(badge)
        except (BadgeError, ci_tier.TierError, OSError, ValueError, KeyError, TypeError) as error:
            entries[badge.slug] = carried(usable(previous.get(badge.slug)), now)
            failures.append(f"{badge.slug}: {error}")
            continue
        entries[badge.slug] = {"message": message, "color": color, "fetched_at": stamp(now)}
    return entries, failures


def render_list(
    badges: tuple[Badge, ...], entries: Mapping[str, Mapping[str, str | None]]
) -> list[dict[str, str]]:
    """The list tools/badges/render.mjs takes, in README order."""
    return [
        {
            "slug": b.slug,
            "label": b.label,
            "message": str(entries[b.slug]["message"]),
            "color": str(entries[b.slug]["color"]),
        }
        for b in badges
    ]


def public_json(url: str) -> object:
    """GET a public JSON API without credentials; https only, bounded in time and size."""
    return ci_tier.https_json(
        "GET", url, {"Accept": "application/json"}, None, PUBLIC_TIMEOUT_S, MAX_RESPONSE_BYTES
    )


def field(doc: object, *keys: str) -> object:
    """``doc[key][key]...`` of a JSON answer; BadgeError when a level is missing or no object."""
    value = doc
    for key in keys:
        if not isinstance(value, dict) or key not in value:
            raise BadgeError(f"the answer has no {'.'.join(keys)}")
        value = value[key]
    return value


def previous_state(github: GitHub, repository: str) -> tuple[dict[str, object], str | None]:
    """Badge entries of the badges branch's state file; none when the branch is new.

    A failed read other than a 404 raises BadgeError: refreshing without the
    last values would turn every failing source "stale" at once, so the run
    stops and the branch keeps what it has. A file that was read but cannot be
    parsed holds no value worth keeping; it reads as empty, with a failure line.
    """
    try:
        doc = github(f"/repos/{repository}/contents/{STATE}?ref={BRANCH}")
    except urllib.error.HTTPError as error:
        error.close()  # it holds the answer body; the 404 and the raise below end here
        if error.code == HTTPStatus.NOT_FOUND:
            return {}, None
        raise BadgeError(f"{STATE}: {error}; the badges keep their values") from error
    except (ci_tier.TierError, OSError, ValueError) as error:
        raise BadgeError(f"{STATE}: {error}; the badges keep their values") from error
    try:
        text = base64.b64decode(str(field(doc, "content"))).decode("utf-8")
        badges = json.loads(text).get("badges")
    except (BadgeError, TypeError, ValueError, AttributeError) as error:
        return {}, f"{STATE}: unreadable ({error})"
    return (badges if isinstance(badges, dict) else {}), None


def blob_sha(data: bytes) -> str:
    """Git object id of a blob holding ``data``, as git computes it (stdin skips filters)."""
    try:
        result = run_command(
            ["git", "hash-object", "--stdin"],
            allowed_executables=("git",),
            input_data=data,
            capture_output=True,
            timeout_seconds=GIT_TIMEOUT_S,
        )
    except (CommandValidationError, CommandTimedOut, CommandOutputLimitExceeded, OSError) as error:
        raise BadgeError(f"git hash-object: {error}") from error
    sha = result.stdout.decode("ascii", "replace").strip()
    if result.returncode or not OBJECT_ID.fullmatch(sha):
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise BadgeError(f"git hash-object exited {result.returncode}: {sha or detail}")
    return sha


def collect(directory: Path, badges: tuple[Badge, ...]) -> dict[str, bytes]:
    """The files to publish: exactly one SVG per badge plus the state file, nothing else."""
    wanted = {f"{b.slug}.svg" for b in badges} | {STATE}
    found = {p.name for p in directory.iterdir()}
    if found != wanted:
        raise BadgeError(
            f"{directory}: missing {sorted(wanted - found)}, unexpected {sorted(found - wanted)}"
        )
    files: dict[str, bytes] = {}
    for name in sorted(wanted):
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_FILE_BYTES:
            raise BadgeError(f"{path}: not a regular file of at most {MAX_FILE_BYTES} bytes")
        data = path.read_bytes()
        text = data.decode("utf-8")
        if name == STATE:
            json.loads(text)
        elif not text.startswith("<svg "):
            raise BadgeError(f"{path}: not an SVG document")
        files[name] = data
    return files


def branch_head(call: Call, repository: str) -> dict[str, str] | None:
    """Commit id, tree id and commit time of the badges branch; ``None`` when it does not exist."""
    try:
        ref = call("GET", f"/repos/{repository}/git/ref/heads/{BRANCH}", None)
    except urllib.error.HTTPError as error:
        if error.code == HTTPStatus.NOT_FOUND:
            error.close()  # it holds the answer body; the caller never sees this one
            return None
        raise
    sha = str(field(ref, "object", "sha"))
    commit = call("GET", f"/repos/{repository}/git/commits/{sha}", None)
    return {
        "sha": sha,
        "tree": str(field(commit, "tree", "sha")),
        "date": str(field(commit, "committer", "date")),
    }


def unchanged(
    call: Call, repository: str, head: Mapping[str, str], files: Mapping[str, bytes], now: datetime
) -> bool:
    """True when every SVG matches the branch and its last commit is younger than QUIET_WINDOW.

    The state file is left out on purpose: its read times change every run, so
    comparing it would commit every run. The branch's read times therefore
    trail the newest read by less than QUIET_WINDOW plus one schedule interval.
    """
    committed = read_time(head["date"])
    if committed is None or now - committed >= QUIET_WINDOW:
        return False
    rows = field(call("GET", f"/repos/{repository}/git/trees/{head['tree']}", None), "tree")
    if not isinstance(rows, list):
        raise BadgeError("the tree answer has no tree list")
    current = {str(field(e, "path")): str(field(e, "sha")) for e in rows}
    if set(current) != set(files):
        return False
    return all(current[name] == blob_sha(data) for name, data in files.items() if name != STATE)


def publish(call: Call, repository: str, files: Mapping[str, bytes], now: datetime) -> str:
    """Commit ``files`` as the whole badges tree.

    GitHub signs the commit, and the ref moves only forward.
    """
    head = branch_head(call, repository)
    if head and unchanged(call, repository, head, files, now):
        return f"unchanged at {head['sha']}"
    entries = [
        {"path": name, "mode": "100644", "type": "blob", "content": data.decode("utf-8")}
        for name, data in sorted(files.items())
    ]
    tree = call("POST", f"/repos/{repository}/git/trees", {"tree": entries})
    message = f"chore(badges): render the README badges at {stamp(now)}"
    parents = [head["sha"]] if head else []
    payload = {"message": message, "tree": field(tree, "sha"), "parents": parents}
    commit = call("POST", f"/repos/{repository}/git/commits", payload)
    sha = str(field(commit, "sha"))
    verification = commit.get("verification") if isinstance(commit, dict) else None
    if not isinstance(verification, dict) or verification.get("verified") is not True:
        raise BadgeError(f"commit {sha} is not signed by GitHub; the branch stays put")
    if head:
        call("PATCH", f"/repos/{repository}/git/refs/heads/{BRANCH}", {"sha": sha, "force": False})
    else:
        call("POST", f"/repos/{repository}/git/refs", {"ref": f"refs/heads/{BRANCH}", "sha": sha})
    return sha


# The one README badge design, as an allow-list: a badge rendered onto this
# repository's badges branch, a shields.io badge in for-the-badge with the dark
# label, or a file in the repository. Anything else, a reference-style image
# whose address this check cannot see included, is a fault. The Praetor-managed
# governance block may also hold this repository's own GitHub workflow badge
# until Praetor issue 1893 offers the style; then governance_badge() goes.
FENCE = re.compile(r"^(?P<mark>`{3,}|~{3,})[^\n]*\n.*?^(?P=mark)[ \t]*$", re.M | re.S)
CODE_SPAN = re.compile(r"`[^`\n]+`")
MANAGED_BLOCK = re.compile(
    r"<!-- praetor:readme-governance:start -->.*?<!-- praetor:readme-governance:end -->", re.S
)
IMAGE_START = re.compile(r"!\[")
INLINE_IMAGE = re.compile(r'!\[[^\]\n]*\]\((?P<url>[^)\s]+)(?:[ \t]+"[^"\n]*")?\)')
REFERENCE_IMAGE = re.compile(r"!\[(?P<alt>[^\]\n]*)\](?:\[(?P<ref>[^\]\n]*)\])?")
REFERENCE_DEF = re.compile(r"^ {0,3}\[(?P<ref>[^\]\n]+)\]:[ \t]*<?(?P<url>[^\s>]+)>?", re.M)
HTML_IMAGE = re.compile(r"<(?:img|source)\b[^>]*>", re.I)
HTML_SOURCE = re.compile(r'\b(?:src|srcset)[ \t]*=[ \t]*"(?P<url>[^"]*)"', re.I)
REPOSITORY_PART = re.compile(r"[A-Za-z0-9_.-]+")
LOCAL_IMAGE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_./-]*")
SHIELDS_HOST = "img.shields.io"
SHIELDS_DESIGN = {"style": "for-the-badge", "labelColor": "111a35"}


@functools.cache
def home_repository() -> str:
    """``owner/name`` of this repository, as its security policy file declares it."""
    try:
        name = json.loads(SECURITY_POLICY.read_text(encoding="utf-8"))["repository"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise BadgeError(f"{SECURITY_POLICY}: no repository name ({error})") from error
    if not isinstance(name, str):
        raise BadgeError(f"{SECURITY_POLICY}: repository {name!r} is not owner/name")
    owner, slash, repo = name.partition("/")
    if not (slash and REPOSITORY_PART.fullmatch(owner) and REPOSITORY_PART.fullmatch(repo)):
        raise BadgeError(f"{SECURITY_POLICY}: repository {name!r} is not owner/name")
    return name


def repository_pattern() -> str:
    """This repository's ``owner/name`` as a regex; GitHub matches both case-insensitively."""
    return f"(?i:{re.escape(home_repository())})"


@functools.cache
def rendered_badge() -> re.Pattern[str]:
    """A badge rendered onto this repository's badges branch, with its slug."""
    return re.compile(
        rf"https://raw\.githubusercontent\.com/{repository_pattern()}/{re.escape(BRANCH)}"
        r"/(?P<slug>[a-z0-9-]+)\.svg"
    )


@functools.cache
def governance_badge() -> re.Pattern[str]:
    """This repository's own Praetor docs workflow badge; only the governance block may show it."""
    return re.compile(
        rf"https://github\.com/{repository_pattern()}"
        r"/actions/workflows/praetor-docs\.yml/badge\.svg"
    )


def blanked(match: re.Match[str]) -> str:
    """The matched text as spaces, newlines kept, so positions and line numbers stay."""
    return re.sub(r"[^\n]", " ", match[0])


def local_image(url: str) -> bool:
    """True for a relative path inside the repository: no scheme, no leading slash, no ``..``."""
    return LOCAL_IMAGE.fullmatch(url) is not None and ".." not in url.split("/")


def missing_file(url: str, root: Path) -> str | None:
    """Why the local image ``url`` shows nothing from ``root``; ``None`` when it is a file there.

    The path is resolved with its symbolic links, so a link that leaves
    ``root`` is a fault as well.
    """
    base = root.resolve()
    target = (base / url).resolve()
    if not target.is_relative_to(base):
        return f"{url}: resolves outside {base}"
    if not target.is_file():
        return f"{url}: no such file next to the README"
    return None


def image_fault(url: str, root: Path) -> str | None:
    """Why the image at ``url`` breaks the one badge design; ``None`` when it keeps it.

    ``root`` is the directory of the README, which relative paths start from.
    """
    rendered = rendered_badge().fullmatch(url)
    if rendered:
        if rendered["slug"] in {badge.slug for badge in BADGES}:
            return None
        return f"{url}: no rendered badge is named {rendered['slug']!r}"
    parts = urllib.parse.urlsplit(url)
    if parts.scheme == "https" and parts.netloc == SHIELDS_HOST:
        query = urllib.parse.parse_qs(parts.query, keep_blank_values=True)
        wrong = [
            f"{key}={value}" for key, value in SHIELDS_DESIGN.items() if query.get(key) != [value]
        ]
        return f"{url}: a shields.io badge needs {' and '.join(wrong)}" if wrong else None
    if local_image(url):
        return missing_file(url, root)
    return f"{url}: not a rendered badge, a for-the-badge shields.io badge or a local image"


def image_url(text: str, start: int, refs: Mapping[str, str]) -> str | None:
    """Address of the Markdown image at ``start``; ``None`` when it names no known reference."""
    inline = INLINE_IMAGE.match(text, start)
    if inline:
        return inline["url"]
    reference = REFERENCE_IMAGE.match(text, start)
    if reference is None:
        return None
    key = reference["ref"] or reference["alt"]
    return refs.get(key.strip().lower())


def markdown_faults(
    text: str,
    span: tuple[int, int],
    refs: Mapping[str, str],
    extra: re.Pattern[str] | None,
    root: Path,
) -> list[str]:
    """One line per Markdown image in ``text[span]`` that breaks the design.

    An image whose address ``extra`` matches passes as well.
    """
    faults: list[str] = []
    for found in IMAGE_START.finditer(text, *span):
        line = text.count("\n", 0, found.start()) + 1
        url = image_url(text, found.start(), refs)
        if url is None:
            faults.append(f"{line}: an image without an inline address this check can read")
        elif not (extra and extra.fullmatch(url)) and (fault := image_fault(url, root)):
            faults.append(f"{line}: {fault}")
    return faults


def html_faults(text: str, root: Path) -> list[str]:
    """One line per HTML ``<img>`` or ``<source>`` that shows anything but a local file."""
    faults: list[str] = []
    for tag in HTML_IMAGE.finditer(text):
        line = text.count("\n", 0, tag.start()) + 1
        urls = [
            candidate.split()[0]
            for source in HTML_SOURCE.finditer(tag[0])
            for candidate in source["url"].split(",")
            if candidate.strip()
        ]
        if not urls:
            faults.append(f"{line}: an HTML image without a double-quoted src or srcset")
        for url in urls:
            if not local_image(url):
                faults.append(f"{line}: {url}: an HTML image must be a local file")
            elif fault := missing_file(url, root):
                faults.append(f"{line}: {fault}")
    return faults


def design_faults(readme: str, root: Path) -> list[str]:
    """Every image in ``readme`` that breaks the one badge design, as ``line: reason``.

    ``root`` is the directory of the README, which relative paths start from.
    """
    text = CODE_SPAN.sub(blanked, FENCE.sub(blanked, readme))
    faults: list[str] = []
    for block in MANAGED_BLOCK.finditer(text):
        refs = {
            d["ref"].strip().lower(): d["url"]
            for d in REFERENCE_DEF.finditer(text, block.start(), block.end())
        }
        faults += markdown_faults(text, block.span(), refs, governance_badge(), root)
    outside = MANAGED_BLOCK.sub(blanked, text)
    faults += markdown_faults(outside, (0, len(outside)), {}, None, root)
    faults += html_faults(text, root)
    return sorted(faults, key=lambda fault: int(fault.split(":", 1)[0]))


def cmd_check_readme(args: argparse.Namespace) -> int:
    """Print every design fault of the README; 1 when there is one."""
    readme = args.readme.resolve()
    faults = design_faults(readme.read_text(encoding="utf-8"), readme.parent)
    for fault in faults:
        print(f"{args.readme}:{fault}", file=sys.stderr)
    if faults:
        print(
            f"{len(faults)} README images break the one badge design"
            " (docs/development/readme-badges.md)",
            file=sys.stderr,
        )
        return 1
    print(f"{args.readme}: every badge has the one design")
    return 0


def write_json(path: Path, doc: object) -> None:
    """Write ``doc`` as sorted, indented JSON, creating the parent directory."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def cmd_spec(args: argparse.Namespace, env: Mapping[str, str], now: datetime) -> int:
    """Read every source, then write the render list and the new state."""
    repository, token = env["GITHUB_REPOSITORY"], env["GITHUB_TOKEN"]

    @functools.cache
    def github(path: str) -> object:
        """GET ``path`` from the GitHub API with the workflow token, once per run."""
        return ci_tier.api_request("GET", API + path, token)

    previous, problem = previous_state(github, repository)
    entries, failures = refresh(
        BADGES, previous, now, lambda b: read_value(b, repository, github, public_json)
    )
    failures = ([problem] if problem else []) + failures
    write_json(args.spec, render_list(BADGES, entries))
    write_json(
        args.state,
        {"version": 1, "generated_at": stamp(now), "failures": failures, "badges": entries},
    )
    for line in failures:
        print(f"::warning title=Badge source failed::{line}")
    print(
        f"{len(BADGES) - len(failures)} of {len(BADGES)} badge sources read; state in {args.state}"
    )
    return 0


def cmd_publish(args: argparse.Namespace, env: Mapping[str, str], now: datetime) -> int:
    """Commit the rendered SVGs and the state file to the badges branch."""
    repository, token = env["GITHUB_REPOSITORY"], env["GITHUB_TOKEN"]

    def call(method: str, path: str, payload: object | None) -> object:
        """Send one GitHub API request with the workflow token."""
        return ci_tier.api_request(method, API + path, token, payload)

    print(f"{BRANCH}: {publish(call, repository, collect(args.dir, BADGES), now)}")
    return 0


def cmd_verdict(args: argparse.Namespace) -> int:
    """Turn the run red, with one annotation per line, when a source failed."""
    failures = json.loads(args.state.read_text(encoding="utf-8")).get("failures")
    if not isinstance(failures, list):
        raise BadgeError(f"{args.state}: no failures list")
    for line in failures:
        print(f"::error title=Badge source failed::{line}")
    if failures:
        print(
            f"{len(failures)} badge sources failed;"
            f" the badges keep their last value up to {STALE_AFTER}"
        )
        return 1
    print("every badge source answered")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the four subcommands and their paths."""
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    spec = sub.add_parser("spec", help="read the sources, write the render list and state")
    spec.add_argument("--spec", type=Path, required=True)
    spec.add_argument("--state", type=Path, required=True)
    pub = sub.add_parser("publish", help="commit the rendered badges to the badges branch")
    pub.add_argument("--dir", type=Path, required=True)
    verdict = sub.add_parser("verdict", help="fail when a source failed this run")
    verdict.add_argument("--state", type=Path, required=True)
    check = sub.add_parser("check-readme", help="fail when a README image breaks the badge design")
    check.add_argument("--readme", type=Path, default=Path("README.md"))
    return parser.parse_args(argv)


def main(argv: list[str] | None = None, env: Mapping[str, str] | None = None) -> int:
    """Run one subcommand; 2 for a missing input or a handled error."""
    args = parse_args(sys.argv[1:] if argv is None else argv)
    env = os.environ if env is None else env
    now = datetime.now(UTC)
    try:
        if args.command == "verdict":
            return cmd_verdict(args)
        if args.command == "check-readme":
            return cmd_check_readme(args)
        missing = [k for k in ("GITHUB_REPOSITORY", "GITHUB_TOKEN") if not env.get(k)]
        if missing:
            raise BadgeError(f"set {', '.join(missing)}")
        handler = cmd_spec if args.command == "spec" else cmd_publish
        return handler(args, env, now)
    except (BadgeError, ci_tier.TierError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"readme_badges.py {args.command}: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
