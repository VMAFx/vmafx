# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Live upstream check of the encoder registry (ADR-3388).

Every row's `ffmpeg_encoder` must be an encoder FFmpeg master registers in
`libavcodec/allcodecs.c`, and every row whose `upstream` is a repository on a
forge with an API (GitHub, GitLab, Gitiles, Bitbucket) must state the
maintenance class its last commit gives and, where the forge reports a
licence the allow-list below classifies, that licence class. A row on any
other host is reported as not checked, never as passing. `fetch` is passed in,
so the tests run against stubs and the command line against the network.
"""

from __future__ import annotations

import json
import re
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, date, datetime
from email.utils import parsedate_to_datetime
from urllib.parse import quote, urlsplit

from .encoders import Encoder, Registry

Fetch = Callable[[str], bytes]
ALLCODECS = "https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/allcodecs.c"
ENCODER = re.compile(r"extern const FFCodec ff_([a-z0-9_]+)_encoder;")
ACTIVE_DAYS = 183  # last commit under six months old
SLOW_DAYS = 730  # under two years; older is dead
# Encoder names whose allcodecs.c symbol is not the name with `-` as `_`
# (libvpxenc.c: ff_libvpx_vp8_encoder has .p.name "libvpx").
SYMBOLS = {"libvpx": "libvpx_vp8"}
GITLAB_HOSTS = frozenset({"gitlab.com", "code.videolan.org"})
LICENCE_CLASS = {
    **dict.fromkeys(
        ("MIT", "ISC", "Zlib", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause",
         "BSD-3-Clause-Clear", "BSD-2-Clause-Patent", "BSD-4-Clause"),
        "permissive",
    ),
    **dict.fromkeys(
        ("LGPL-2.1", "LGPL-2.1-only", "LGPL-2.1-or-later", "LGPL-3.0", "LGPL-3.0-only",
         "LGPL-3.0-or-later", "MPL-2.0"),
        "copyleft-weak",
    ),
    **dict.fromkeys(
        ("GPL-2.0", "GPL-2.0-only", "GPL-2.0-or-later", "GPL-3.0", "GPL-3.0-only",
         "GPL-3.0-or-later", "AGPL-3.0", "AGPL-3.0-only", "AGPL-3.0-or-later"),
        "copyleft-strong",
    ),
}  # fmt: skip
JUDGED = frozenset({"patent-licensed-binary", "proprietary-sdk", "research-noncommercial"})


@dataclass(frozen=True)
class Live:
    last_commit: date
    archived: bool
    spdx: str  # "" when the forge reports none


@dataclass(frozen=True)
class Finding:
    id: str
    verdict: str  # ok, mismatch or unchecked
    detail: str


def ffmpeg_encoders(fetch: Fetch) -> frozenset[str]:
    names = frozenset(ENCODER.findall(fetch(ALLCODECS).decode()))
    if not names:
        raise ValueError(f"{ALLCODECS}: no encoder declarations found")
    return names


def maintenance(live: Live, today: date) -> str:
    if live.archived:
        return "archived"
    age = (today - live.last_commit).days
    if age < ACTIVE_DAYS:
        return "active"
    return "slow" if age < SLOW_DAYS else "dead"


def _github(owner: str, repo: str, fetch: Fetch) -> Live:
    meta = json.loads(fetch(f"https://api.github.com/repos/{owner}/{repo}"))
    head = json.loads(fetch(f"https://api.github.com/repos/{owner}/{repo}/commits?per_page=1"))
    spdx = (meta.get("license") or {}).get("spdx_id") or ""
    stamp = head[0]["commit"]["committer"]["date"]
    return Live(_day(stamp), bool(meta["archived"]), "" if spdx == "NOASSERTION" else spdx)


def _gitlab(host: str, path: str, fetch: Fetch) -> Live:
    meta = json.loads(fetch(f"https://{host}/api/v4/projects/{quote(path, safe='')}"))
    return Live(_day(meta["last_activity_at"]), bool(meta.get("archived", False)), "")


def _gitiles(host: str, path: str, fetch: Fetch) -> Live:
    body = fetch(f"https://{host}/{path}/+log/HEAD?format=JSON&n=1").decode()
    log = json.loads(body.split("\n", 1)[1])  # first line is the )]}' guard
    stamp = parsedate_to_datetime(log["log"][0]["committer"]["time"] + " +0000")
    return Live(stamp.date(), False, "")


def _bitbucket(path: str, fetch: Fetch) -> Live:
    url = f"https://api.bitbucket.org/2.0/repositories/{path}/commits?pagelen=1"
    head = json.loads(fetch(url))
    return Live(_day(head["values"][0]["date"]), False, "")


def _day(stamp: str) -> date:
    return datetime.fromisoformat(stamp.replace("Z", "+00:00")).astimezone(UTC).date()


def live_state(upstream: str, fetch: Fetch) -> Live | None:
    """The forge state of `upstream`, or None for a host without a forge API."""
    parts = urlsplit(upstream)
    path = parts.path.strip("/").removesuffix(".git")
    segments = path.split("/")
    if parts.hostname == "github.com" and len(segments) == 2:  # noqa: PLR2004 (owner/repo)
        return _github(segments[0], segments[1], fetch)
    if parts.hostname in GITLAB_HOSTS:
        return _gitlab(parts.hostname, path, fetch)
    if parts.hostname and parts.hostname.endswith(".googlesource.com"):
        return _gitiles(parts.hostname, path, fetch)
    if parts.hostname == "bitbucket.org" and len(segments) == 2:  # noqa: PLR2004
        return _bitbucket(path, fetch)
    return None


def _licence_problem(row: Encoder, live: Live) -> str:
    found = LICENCE_CLASS.get(live.spdx, "")
    if not found or row.licence_class in JUDGED or row.licence_class == found:
        return ""
    return f"licence_class {row.licence_class!r}, forge reports {live.spdx} ({found})"


def check_row(row: Encoder, live: Live | None, today: date) -> Finding:
    if not row.upstream:
        return Finding(row.id, "unchecked", "no upstream")
    if live is None:
        return Finding(row.id, "unchecked", f"no forge API for {row.upstream}")
    state = f"last commit {live.last_commit}, licence {live.spdx or 'not reported'}"
    problems = [_licence_problem(row, live)]
    found = maintenance(live, today)
    if row.maintenance != found:
        problems.append(f"maintenance {row.maintenance!r}, live {found!r}")
    problems = [p for p in problems if p]
    if problems:
        return Finding(row.id, "mismatch", "; ".join(problems) + f" ({state})")
    return Finding(row.id, "ok", state)


def check(registry: Registry, fetch: Fetch, today: date) -> list[Finding]:
    """One finding per row; the FFmpeg name check runs before the forge checks."""
    known = ffmpeg_encoders(fetch)
    out = []
    for row in registry.encoders:
        named = _ffmpeg_finding(row, known)
        if named:
            out.append(named)
            continue
        out.append(check_row(row, live_state(row.upstream, fetch) if row.upstream else None, today))
    return out


def _ffmpeg_finding(row: Encoder, known: frozenset[str]) -> Finding | None:
    """The FFmpeg name verdict; None when the forge checks decide the row.

    allcodecs.c names the C symbol, where FFmpeg's `-` in an encoder name such
    as libaom-av1 is `_` and SYMBOLS holds the names that differ further. A row that awaits FFmpeg (an ADR-0339 placeholder) is
    unchecked until FFmpeg ships the encoder and a mismatch from then on, so the
    placeholder gets flipped.
    """
    if not row.ffmpeg_encoder:
        return None
    symbol = SYMBOLS.get(row.ffmpeg_encoder, row.ffmpeg_encoder.replace("-", "_"))
    present = symbol in known
    if row.awaits_ffmpeg:
        if present:
            return Finding(row.id, "mismatch", f"FFmpeg master now has {row.ffmpeg_encoder}")
        return Finding(row.id, "unchecked", f"awaits {row.ffmpeg_encoder} in FFmpeg master")
    if not present:
        return Finding(row.id, "mismatch", f"FFmpeg master has no {row.ffmpeg_encoder}")
    return None
