#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Weekly research-radar collector: what changed in the watched public sources.

Reads the public source registry (``docs/research/radar/sources.yaml``), asks each
automated source what changed inside one time window, and renders one markdown
digest. No model is involved and the run keeps no state: the window is a pure
function of ``--now`` and ``--days``, so two runs with the same inputs write the
same digest, and consecutive weekly windows partition time exactly (a timestamp
belongs to the window ``since <= t < now``; a date-only value to
``since.date() <= d < now.date()``).

An unchanged feed yields an empty digest (empty file, ``empty`` on stdout). A source
that cannot be read is named in the digest, never skipped silently; the run fails
(exit 2) only when every automated source failed.

Source kinds (``manual`` entries are read by the triage lane, not collected):

* ``github_owner``: new repositories, releases and pushes of a user or organisation
* ``github_repo``: releases and pushes of one repository
* ``arxiv_query``: new arXiv papers for a search query
* ``openalex_query``: new OpenAlex works for a title-and-abstract query

Exit status: 0 digest written (possibly empty), 2 usage error, bad registry or every
source failed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field, replace
from datetime import UTC, date, datetime, timedelta
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

SCHEMA = 1
KINDS = ("github_owner", "github_repo", "arxiv_query", "openalex_query", "manual")
AUTOMATED = ("github_owner", "github_repo", "arxiv_query", "openalex_query")
SECTIONS = ("New repositories", "Releases", "Pushes", "New papers")
USER_AGENT = "vmafx-research-radar (+https://github.com/VMAFx/vmafx)"
HTTP_TIMEOUT_S = 20.0
HTTP_ATTEMPTS = 3
HTTP_BACKOFF_S = (1.0, 4.0)
MAX_BODY_BYTES = 8 * 1024 * 1024
MAX_REPOS_PER_OWNER = 100
MAX_RELEASE_LOOKUPS = 10
MAX_PAPERS = 50
MAX_WINDOW_DAYS = 366
MAX_ITEMS_PER_SECTION = 50
MAX_TITLE_CHARS = 160
GITHUB_API = "https://api.github.com"
ARXIV_API = "https://export.arxiv.org/api/query"
OPENALEX_API = "https://api.openalex.org/works"
ATOM = "{http://www.w3.org/2005/Atom}"
_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,63}$")
_REPO_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
_OWNER_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
_SPACE_RE = re.compile(r"\s+")

Fetcher = Callable[[str], bytes]


class RegistryError(ValueError):
    """The registry file is not a valid source registry."""


class FetchError(RuntimeError):
    """One URL could not be read."""


@dataclass(frozen=True)
class Source:
    """One registry entry; only the fields the collector acts on are typed."""

    id: str
    kind: str
    url: str
    why: str
    licence_note: str
    owner: str = ""
    repo: str = ""
    query: str = ""
    include_forks: bool = False
    name_filter: str = ""
    releases_only: bool = False


@dataclass(frozen=True)
class Item:
    """One change worth a line in the digest."""

    section: str
    source_id: str
    title: str
    url: str
    when: str
    detail: str = ""


@dataclass
class Collected:
    """The items and the per-source failures of one run."""

    items: list[Item] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    sources_read: int = 0


# --------------------------------------------------------------------------- registry


def _require(entry: Mapping[str, Any], key: str, kind: type, where: str) -> Any:
    value = entry.get(key)
    if not isinstance(value, kind) or (kind is str and not str(value).strip()):
        raise RegistryError(f"{where}: field '{key}' must be a non-empty {kind.__name__}")
    return value


def _kind_fields(entry: Mapping[str, Any], kind: str, where: str) -> dict[str, Any]:
    """The kind-specific fields of one entry, validated."""
    if kind == "github_owner":
        owner = _require(entry, "owner", str, where)
        if not _OWNER_RE.match(owner):
            raise RegistryError(f"{where}: owner '{owner}' is not a GitHub login")
        name_filter = str(entry.get("name_filter", ""))
        if name_filter:
            re.compile(name_filter)
        return {"owner": owner, "name_filter": name_filter}
    if kind == "github_repo":
        repo = _require(entry, "repo", str, where)
        if not _REPO_RE.match(repo):
            raise RegistryError(f"{where}: repo '{repo}' is not owner/name")
        return {"repo": repo, "releases_only": bool(entry.get("releases_only", False))}
    if kind in ("arxiv_query", "openalex_query"):
        return {"query": _require(entry, "query", str, where)}
    return {}


def parse_source(entry: object, index: int) -> Source:
    """Validate one registry entry and turn it into a ``Source``."""
    where = f"sources[{index}]"
    if not isinstance(entry, dict):
        raise RegistryError(f"{where}: an entry must be a mapping")
    sid = _require(entry, "id", str, where)
    if not _ID_RE.match(sid):
        raise RegistryError(f"{where}: id '{sid}' must match {_ID_RE.pattern}")
    kind = _require(entry, "kind", str, where)
    if kind not in KINDS:
        raise RegistryError(f"{where}: kind '{kind}' is not one of {KINDS}")
    common = {
        "id": sid,
        "kind": kind,
        "url": _require(entry, "url", str, where),
        "why": _require(entry, "why", str, where),
        "licence_note": _require(entry, "licence_note", str, where),
        "include_forks": bool(entry.get("include_forks", False)),
    }
    return Source(**common, **_kind_fields(entry, kind, where))


def load_registry(path: Path) -> list[Source]:
    """Read and validate the registry; ids are unique."""
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise RegistryError(f"{path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise RegistryError(f"{path}: top level must be a mapping with schema: {SCHEMA}")
    entries = data.get("sources")
    if not isinstance(entries, list) or not entries:
        raise RegistryError(f"{path}: 'sources' must be a non-empty list")
    sources = [parse_source(entry, i) for i, entry in enumerate(entries)]
    seen: set[str] = set()
    for src in sources:
        if src.id in seen:
            raise RegistryError(f"{path}: duplicate source id '{src.id}'")
        seen.add(src.id)
    return sources


# --------------------------------------------------------------------------- time


def parse_time(text: str) -> datetime:
    """An ISO 8601 timestamp or date as an aware UTC datetime."""
    stamp = datetime.fromisoformat(text)
    return stamp.replace(tzinfo=UTC) if stamp.tzinfo is None else stamp.astimezone(UTC)


@dataclass(frozen=True)
class Window:
    """The half-open interval one digest covers."""

    since: datetime
    now: datetime

    def holds_time(self, text: str) -> bool:
        """True when a timestamp falls in ``since <= t < now``."""
        return bool(text) and self.since <= parse_time(text) < self.now

    def holds_date(self, text: str) -> bool:
        """True when a date-only value falls in ``since.date() <= d < now.date()``."""
        return bool(text) and self.since.date() <= date.fromisoformat(text[:10]) < self.now.date()

    @property
    def label(self) -> str:
        year, week, _ = self.now.isocalendar()
        return f"{year}-W{week:02d}"


# --------------------------------------------------------------------------- fetching


def http_fetch(url: str, token: str = "") -> bytes:
    """GET ``url`` with a timeout and a bounded body, retrying a few times."""
    headers = {"User-Agent": USER_AGENT, "Accept": "*/*"}
    if token and url.startswith(GITHUB_API + "/"):
        headers["Authorization"] = f"Bearer {token}"
        headers["Accept"] = "application/vnd.github+json"
    last: Exception | None = None
    for attempt in range(HTTP_ATTEMPTS):
        try:
            if not url.startswith("https://"):
                raise FetchError(f"{url}: only https is fetched")
            request = urllib.request.Request(url, headers=headers)  # noqa: S310 (https checked)
            with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_S) as response:  # noqa: S310
                body: bytes = response.read(MAX_BODY_BYTES + 1)
            if len(body) > MAX_BODY_BYTES:
                raise FetchError(f"{url}: body above {MAX_BODY_BYTES} bytes")
            return body
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last = exc
            if attempt < len(HTTP_BACKOFF_S):
                time.sleep(HTTP_BACKOFF_S[attempt])
    raise FetchError(f"{url}: {last}")


def _token_fetcher(token: str) -> Fetcher:
    """The network fetcher with a GitHub token bound."""

    def fetch(url: str) -> bytes:
        return http_fetch(url, token)

    return fetch


def fixture_fetcher(table: Mapping[str, Any]) -> Fetcher:
    """A fetcher that answers from a url -> body table (offline runs and tests)."""

    def fetch(url: str) -> bytes:
        if url not in table:
            raise FetchError(f"{url}: not in the fixture table")
        body = table[url]
        return body.encode("utf-8") if isinstance(body, str) else json.dumps(body).encode("utf-8")

    return fetch


def _json(fetch: Fetcher, url: str) -> Any:
    try:
        return json.loads(fetch(url))
    except json.JSONDecodeError as exc:
        raise FetchError(f"{url}: not JSON ({exc})") from exc


# --------------------------------------------------------------------------- text


def clean_text(text: str) -> str:
    """One-line, bounded, notification-safe text from an untrusted title."""
    flat = _SPACE_RE.sub(" ", text).strip()
    flat = flat.replace("@", "@\u200b").replace("#", "#\u200b")
    flat = flat.replace("[", "(").replace("]", ")").replace("|", "/")
    return flat if len(flat) <= MAX_TITLE_CHARS else flat[: MAX_TITLE_CHARS - 3] + "..."


# --------------------------------------------------------------------------- GitHub


def github_repos_url(owner: str) -> str:
    """The listing URL of an owner's repositories, most recently pushed first."""
    return f"{GITHUB_API}/users/{owner}/repos?per_page={MAX_REPOS_PER_OWNER}&sort=pushed&direction=desc"


def github_repo_url(repo: str) -> str:
    """The URL of one repository record."""
    return f"{GITHUB_API}/repos/{repo}"


def github_releases_url(repo: str) -> str:
    """The URL of a repository's most recent releases."""
    return f"{GITHUB_API}/repos/{repo}/releases?per_page=5"


def _repo_items(
    src: Source, rec: Mapping[str, Any], win: Window, fetch: Fetcher, check_releases: bool = True
) -> list[Item]:
    """Items for one repository record: new, pushed and released inside the window."""
    full = str(rec.get("full_name", ""))
    url = str(rec.get("html_url", f"https://github.com/{full}"))
    desc = clean_text(str(rec.get("description") or ""))
    out: list[Item] = []
    created, pushed = str(rec.get("created_at", "")), str(rec.get("pushed_at", ""))
    if not src.releases_only:
        if win.holds_time(created):
            out.append(Item("New repositories", src.id, full, url, created, desc))
        elif win.holds_time(pushed):
            out.append(Item("Pushes", src.id, full, url, pushed, desc))
    if win.holds_time(pushed) or win.holds_time(created):
        if check_releases:
            out.extend(_release_items(src, full, win, fetch))
        else:
            out = [replace(i, detail=f"{i.detail} (releases not checked: lookup cap)") for i in out]
    return out


def _release_items(src: Source, full: str, win: Window, fetch: Fetcher) -> list[Item]:
    releases = _json(fetch, github_releases_url(full))
    out = []
    for rel in releases[:5]:
        stamp = str(rel.get("published_at") or "")
        if rel.get("draft") or not win.holds_time(stamp):
            continue
        name = clean_text(str(rel.get("name") or rel.get("tag_name") or ""))
        out.append(Item("Releases", src.id, f"{full} {name}", str(rel.get("html_url", "")), stamp))
    return out


def collect_github_owner(src: Source, win: Window, fetch: Fetcher) -> list[Item]:
    """New repositories, pushes and releases of one owner."""
    records = _json(fetch, github_repos_url(src.owner))
    pattern = re.compile(src.name_filter, re.IGNORECASE) if src.name_filter else None
    out: list[Item] = []
    lookups = 0
    for rec in records[:MAX_REPOS_PER_OWNER]:
        if (rec.get("fork") and not src.include_forks) or rec.get("archived"):
            continue
        if pattern and not pattern.search(str(rec.get("name", ""))):
            continue
        active = win.holds_time(str(rec.get("pushed_at", ""))) or win.holds_time(
            str(rec.get("created_at", ""))
        )
        lookups += 1 if active else 0
        out.extend(_repo_items(src, rec, win, fetch, lookups <= MAX_RELEASE_LOOKUPS))
    return out


def collect_github_repo(src: Source, win: Window, fetch: Fetcher) -> list[Item]:
    """Pushes and releases of one repository."""
    return _repo_items(src, _json(fetch, github_repo_url(src.repo)), win, fetch)


# --------------------------------------------------------------------------- papers


def arxiv_url(query: str) -> str:
    """The arXiv API URL for a query, newest submissions first."""
    params = {
        "search_query": query,
        "sortBy": "submittedDate",
        "sortOrder": "descending",
        "max_results": str(MAX_PAPERS),
    }
    return f"{ARXIV_API}?{urllib.parse.urlencode(params)}"


def openalex_url(query: str, win: Window) -> str:
    """The OpenAlex URL for a title-and-abstract query inside the window's dates."""
    last_day = (win.now.date() - timedelta(days=1)).isoformat()
    flt = (
        f'title_and_abstract.search:"{query}",'
        f"from_publication_date:{win.since.date().isoformat()},to_publication_date:{last_day}"
    )
    params = {
        "filter": flt,
        "sort": "publication_date:desc",
        "per-page": str(MAX_PAPERS),
        "select": "id,display_name,publication_date,doi",
    }
    return f"{OPENALEX_API}?{urllib.parse.urlencode(params)}"


def _safe_xml(body: bytes, url: str) -> ET.Element:
    """Parse an Atom feed; a document type declaration is refused (entity expansion)."""
    if b"<!DOCTYPE" in body.upper() or b"<!ENTITY" in body.upper():
        raise FetchError(f"{url}: a document type declaration is not accepted")
    try:
        return ET.fromstring(body)  # noqa: S314 (DOCTYPE refused above)
    except ET.ParseError as exc:
        raise FetchError(f"{url}: not XML ({exc})") from exc


def collect_arxiv(src: Source, win: Window, fetch: Fetcher) -> list[Item]:
    """New arXiv papers for one query."""
    url = arxiv_url(src.query)
    root = _safe_xml(fetch(url), url)
    out: list[Item] = []
    for entry in root.findall(f"{ATOM}entry")[:MAX_PAPERS]:
        published = (entry.findtext(f"{ATOM}published") or "").strip()
        if not win.holds_time(published):
            continue
        title = clean_text(entry.findtext(f"{ATOM}title") or "")
        link = (entry.findtext(f"{ATOM}id") or "").strip().replace("http://", "https://", 1)
        out.append(Item("New papers", src.id, title, link, published, "arXiv"))
    return out


def collect_openalex(src: Source, win: Window, fetch: Fetcher) -> list[Item]:
    """New OpenAlex works for one query."""
    data = _json(fetch, openalex_url(src.query, win))
    out: list[Item] = []
    for work in data.get("results", [])[:MAX_PAPERS]:
        published = str(work.get("publication_date") or "")
        if not win.holds_date(published):
            continue
        link = str(work.get("doi") or work.get("id") or "")
        title = clean_text(str(work.get("display_name") or ""))
        out.append(Item("New papers", src.id, title, link, published, "OpenAlex"))
    return out


COLLECTORS: dict[str, Callable[[Source, Window, Fetcher], list[Item]]] = {
    "github_owner": collect_github_owner,
    "github_repo": collect_github_repo,
    "arxiv_query": collect_arxiv,
    "openalex_query": collect_openalex,
}


# --------------------------------------------------------------------------- digest


def _dedupe(items: Iterable[Item]) -> list[Item]:
    """One item per (section, url) and one paper per normalised title."""
    seen: set[tuple[str, str]] = set()
    out: list[Item] = []
    for item in items:
        key = item.title.lower() if item.section == "New papers" else item.url
        if (item.section, key) in seen:
            continue
        seen.add((item.section, key))
        out.append(item)
    return out


def _sorted(items: Iterable[Item]) -> list[Item]:
    """Section order, then newest first, then source and url for a stable tie-break."""
    out = sorted(items, key=lambda item: (item.source_id, item.url))
    out.sort(key=lambda item: item.when, reverse=True)
    out.sort(key=lambda item: SECTIONS.index(item.section))
    return out


def collect(sources: Sequence[Source], win: Window, fetch: Fetcher) -> Collected:
    """Run every automated source; a failing source is recorded, not fatal."""
    result = Collected()
    for src in sources:
        collector = COLLECTORS.get(src.kind)
        if collector is None:
            continue
        try:
            result.items.extend(collector(src, win, fetch))
            result.sources_read += 1
        except (FetchError, KeyError, TypeError, ValueError, AttributeError) as exc:
            result.errors.append(f"{src.id}: {exc}")
    result.items = _dedupe(_sorted(result.items))
    return result


def _section_lines(name: str, items: Sequence[Item]) -> list[str]:
    lines = [f"## {name} ({len(items)})", ""]
    for item in items[:MAX_ITEMS_PER_SECTION]:
        extra = f", {item.detail}" if item.detail else ""
        lines.append(f"- [{item.title}]({item.url}) ({item.source_id}, {item.when[:10]}{extra})")
    if len(items) > MAX_ITEMS_PER_SECTION:
        lines.append(f"- ... and {len(items) - MAX_ITEMS_PER_SECTION} more")
    return [*lines, ""]


def render_digest(result: Collected, win: Window) -> str:
    """The markdown digest, or an empty string when nothing changed and nothing failed."""
    if not result.items and not result.errors:
        return ""
    last = (win.now - timedelta(seconds=1)).date().isoformat()
    lines = [
        f"# Research radar digest {win.label}",
        "",
        f"Window: {win.since.date().isoformat()} to {last} (UTC). "
        f"Sources read: {result.sources_read}. Items: {len(result.items)}.",
        "",
        "Triage this digest with the "
        "[triage procedure](https://github.com/VMAFx/vmafx/blob/master/docs/research/radar/triage.md).",
        "",
    ]
    for name in SECTIONS:
        group = [item for item in result.items if item.section == name]
        if group:
            lines.extend(_section_lines(name, group))
    if result.errors:
        lines.extend(["## Sources that could not be read", ""])
        lines.extend(f"- {clean_text(err)}" for err in result.errors)
        lines.append("")
    return "\n".join(lines)


# --------------------------------------------------------------------------- CLI


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--registry", type=Path, default=root / "docs/research/radar/sources.yaml")
    parser.add_argument(
        "--output", type=Path, required=True, help="digest file (empty when nothing changed)"
    )
    parser.add_argument(
        "--now", default="", help="window end, ISO 8601 (default: the current UTC time)"
    )
    parser.add_argument("--days", type=int, default=7, help="window length in days")
    parser.add_argument("--fixtures", type=Path, help="JSON table url -> body; no network")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """Collect, write the digest, print ``empty`` or the item count."""
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    if not 1 <= args.days <= MAX_WINDOW_DAYS:
        print(f"radar_collect: --days must be 1..{MAX_WINDOW_DAYS}", file=sys.stderr)
        return 2
    try:
        sources = load_registry(args.registry)
        now = parse_time(args.now) if args.now else datetime.now(UTC).replace(microsecond=0)
        if args.fixtures:
            fetch = fixture_fetcher(json.loads(args.fixtures.read_text(encoding="utf-8")))
        else:
            token = os.environ.get("GITHUB_TOKEN", "")
            fetch = _token_fetcher(token)
    except (RegistryError, OSError, ValueError) as exc:
        print(f"radar_collect: {exc}", file=sys.stderr)
        return 2
    win = Window(since=now - timedelta(days=args.days), now=now)
    result = collect(sources, win, fetch)
    for err in result.errors:
        print(f"radar_collect: source failed: {err}", file=sys.stderr)
    args.output.write_text(render_digest(result, win), encoding="utf-8")
    total = sum(1 for s in sources if s.kind in AUTOMATED)
    print(f"{len(result.items)} items" if result.items else "empty")
    print(
        f"sources read {result.sources_read}/{total}, failed {len(result.errors)}", file=sys.stderr
    )
    return 2 if total and result.sources_read == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
