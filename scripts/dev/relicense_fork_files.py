#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Move fork-authored files to EUPL-1.2, decided by provenance, never by header text.

ADR-1250. A candidate is a tracked text file that declares a licence, or a
source file that declares none. It moves only if it passes every veto below,
each conservative, checked in this order:

1. ``compat-python-vmaf`` -- not under ``compat/python-vmaf/``, which is Netflix's
   ``python/vmaf`` relocated wholesale by ADR-0700.
2. ``upstream-path`` -- neither its path nor its pre-ADR-0700 ``libvmaf/`` path
   exists upstream.
3. ``upstream-name`` -- no upstream file shares its name. Names that carry no
   provenance signal (``__init__.py`` and friends) are exempt.
4. ``vendored-mirror`` -- it is not a verbatim mirror of another repository
   (``[mirrors]`` in ``relicense_provenance.toml``). A mirror's terms are decided
   where it comes from, and a sync script diffs it byte for byte against that
   origin, so it is never rewritten at all, not even repaired.
5. ``documented-port`` / ``provenance-unreviewed`` -- it does not carry someone
   else's code. Which files do is recorded in ``relicense_provenance.toml``:
   kernels by the metric they implement, individual files by review. A file
   whose header claims a derivation nobody has reviewed keeps its terms until
   someone does.
6. ``third-party-copyright`` -- no copyright notice names anyone but Lusoris.
7. ``foreign-licence`` -- every licence it declares is one the fork has used for
   its own work.
8. ``outside-contribution`` -- no fork-local commit that touched it, following
   renames, was authored or co-authored by a person other than the owner. There
   is no CLA and no DCO, so such a contribution arrived under the terms the file
   had then, and relicensing it needs that contributor's consent.

A file that moves ends with exactly one licence, ``EUPL-1.2``: its tags are
retargeted, prose grants ("Licensed under the BSD+Patent License ...", "Use of this
source code is governed by ...") are removed rather than left contradicting the
tag, a Python ``__license__`` attribute follows the tag, and a source file with no
notice gains one. Tags inside generator strings are retargeted too, because what
they emit is fork-authored.

A file that stays keeps its terms, with two repairs. An identifier that does not
exist becomes the one it meant, per ADR-1255 (``BSD-3-Clause-Plus-Patent`` and
``BSD+Patent`` become ``BSD-2-Clause-Patent``). A file carrying someone else's
code regains their copyright notice, and its tag gains their licence, because
every one of those licences requires the notice to travel with the code.

Usage:
    relicense_fork_files.py --list     # verdict per candidate, no writes
    relicense_fork_files.py --write    # apply every rewrite, repair and attribution
    relicense_fork_files.py --check    # exit 1 if any of those is pending

Exit codes: 0 done or clean, 1 ``--check`` found pending work, 2 usage, data or git
error.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from collections import Counter
from collections.abc import Callable, Iterable, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from functools import partial
from pathlib import Path, PurePosixPath

import tomllib

GIT = shutil.which("git") or "/usr/bin/git"
TARGET = "EUPL-1.2"
# Split so this file's own source is never mistaken for a tag to rewrite.
TAG = "SPDX-License-" + "Identifier:"
OWNER = "Lusoris"
OWNER_EMAILS = frozenset({"lusoris@proton.me", "lusoris@pm.me"})
PROVENANCE = Path(__file__).with_name("relicense_provenance.toml")

# Every identifier the fork has used for its own work. Two are not SPDX
# identifiers at all; REPAIRS maps them to the licence they meant (ADR-1255).
FORK_IDS = frozenset(
    {
        "BSD-2-Clause-Patent",
        "BSD-3-Clause-Plus-Patent",
        "BSD+Patent",
        "BSD-3-Clause",
        "BSD-3-Clause-Clear",
        "MIT",
        TARGET,
    }
)
REPAIRS = {"BSD-3-Clause-Plus-Patent": "BSD-2-Clause-Patent", "BSD+Patent": "BSD-2-Clause-Patent"}
UPSTREAM_LICENCE = "BSD-2-Clause-Patent"
ID = r"(?:LicenseRef-[A-Za-z0-9.-]+|[A-Za-z0-9][A-Za-z0-9.+-]*[A-Za-z0-9+])"
TAG_RE = re.compile(
    re.escape(TAG)
    + r"(?P<gap>[ \t]*)(?P<expr>"
    + ID
    + r"(?:[ \t]+(?:OR|AND|WITH)[ \t]+"
    + ID
    + r")*)"
)
# An expression is a licence declaration, not prose that happens to name the
# tag, when its first identifier looks like one.
LICENCE_LIKE = re.compile(r"^(?:LicenseRef-|[A-Z0-9][A-Za-z0-9.+-]*$)")
OPERATORS = frozenset({"OR", "AND", "WITH"})
PY_LICENSE_ATTR = re.compile(r"""^(?P<head>__license__\s*=\s*)(?P<q>["'])[^"'\n]*(?P=q)""", re.M)

NO_SIGNAL_NAMES = frozenset({"__init__.py", "conftest.py", "meson.build"})
UPSTREAM_REMAP = (("core/", "libvmaf/"),)

EXCLUDED_PREFIXES = (
    "docs/",
    "changelog.d/",
    "LICENSES/",
    "subprojects/",
    "model/",
    "core/test/data/",
    "python/test/resource/",
    "compat/python-vmaf/resource/",
)
EXCLUDED_SUFFIXES = (".md",)
EXCLUDED_NAMES = frozenset({"LICENSE", "COPYING"})
# The tool, its data and its tests carry licence text as data.
SELF = frozenset(
    {
        "scripts/dev/relicense_fork_files.py",
        "scripts/dev/relicense_provenance.toml",
        "scripts/dev/tests/test_relicense_fork_files.py",
    }
)

BLOCK_EXT = frozenset(
    {
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".cc",
        ".cxx",
        ".hxx",
        ".cu",
        ".cuh",
        ".mm",
        ".m",
        ".metal",
        ".hip",
    }
)
SLASH_EXT = frozenset({".go", ".rs", ".proto"})
HASH_EXT = frozenset({".py", ".sh", ".bash"})
HASH_NAMES = frozenset({"meson.build"})

PROSE_START = re.compile(
    r"Licensed under the BSD\+Patent License|Use of this source code is governed by"
)
PROSE_LONG_END = "limitations under the License."
PROSE_URL_END = re.compile(r"opensource\.org/licenses/BSDplusPatent")
PROSE_GO_END = re.compile(r"LICENSE file\.")
PROSE_SCAN = 14
HEADER_SCAN = 60
COPYRIGHT_SCAN = 200
NOTICE_SCAN = 40
DECORATION = re.compile(r"^[ \t]*(?:#+|//+|/\*+|\*+|--|;+|<!--)?[ \t]*")
COPYRIGHT_NOTICE = re.compile(
    r"copyright\s*(?:\(c\)|©|\d{4})|^copyright\s+the\s|^(?:\(c\)|©)\s*\d{4}", re.I
)
COPYRIGHT_LINE = re.compile(r"^copyright\b", re.I)
SOURCE_NOTICE = re.compile(r"^copyright\s*(?:\(c\)|©|\d{4})", re.I)
FILE_COPYRIGHT = re.compile(r"SPDX-FileCopyrightText:(.*)", re.I)
BLANK_COMMENT = frozenset({"", "*", "#", "//", "--", ";"})
CLOSERS = frozenset({"*/", "**/", '"""', "'''"})
CODING_COOKIE = re.compile(r"^[ \t\f]*#.*?coding[:=]")

# A header sentence that says the file carries someone else's code: a verb of
# derivation near a reference to upstream or third-party code. "Translation
# unit" is a C term, not a derivation.
DERIVATION_VERB = re.compile(
    r"\b(?:port(?:ed|s|ing)?|cop(?:y|ied)|translat\w*(?!\s+units?\b)|transliterat\w*|"
    r"deriv(?:ed|ative)|adapt(?:ed)?|lift(?:ed)?|verbatim|mirror(?:s|ed)?|based on|"
    r"taken from|duplicat\w*|re-?implement\w*)\b",
    re.I,
)
THIRD_PARTY_NAMES = re.compile(r"\b(?:libjxl|xiph|daala|iqa|nvidia|netflix)(?:\b|_)", re.I)
SOURCE_REF = re.compile(r"[\w/.-]+\.(?:c|h|cpp|cu|cuh|comp|py|asm)\b")
MIN_SIGNAL_NAME = 6
# Include and import lines name dependencies, not origins.
IMPORT_LINE = re.compile(r"^\s*(?:#\s*(?:include|import)\b|import\s|from\s\S+\s+import\s|use\s)")


@dataclass(frozen=True)
class Verdict:
    """Where a candidate file lands and why."""

    path: str
    reason: str

    @property
    def moves(self) -> bool:
        return self.reason == "moves"


@dataclass(frozen=True)
class Source:
    """Code a port was taken from: the notices and licences that travel with it."""

    notices: tuple[str, ...]
    licences: tuple[str, ...]


@dataclass(frozen=True)
class Family:
    """Files that carry one origin, chosen by role: the code a path implements."""

    name: str
    match: re.Pattern[str]
    sources: tuple[str, ...]


@dataclass
class Provenance:
    """The reviewed provenance: named origins, families, and per-file verdicts."""

    sources: dict[str, Source] = field(default_factory=dict)
    families: list[Family] = field(default_factory=list)
    ports: dict[str, list[str]] = field(default_factory=dict)
    not_ports: set[str] = field(default_factory=set)
    mirrors: dict[str, re.Pattern[str]] = field(default_factory=dict)

    def mirror_of(self, path: str) -> str | None:
        """The repository ``path`` is a verbatim mirror of, or None."""
        return next((name for name, match in self.mirrors.items() if match.search(path)), None)

    def port_sources(self, path: str) -> tuple[str, ...] | None:
        """The origins ``path`` carries code from, or None when it carries none."""
        if path in self.ports:
            return tuple(self.ports[path])
        if path in self.not_ports:
            return None
        return next((f.sources for f in self.families if f.match.search(path)), None)


@dataclass(frozen=True)
class Upstream:
    """The upstream tree, indexed the ways the vetoes look it up."""

    paths: frozenset[str]
    names: frozenset[str]
    signal: frozenset[str]


def die(message: str) -> None:
    print(f"relicense_fork_files: {message}", file=sys.stderr)
    raise SystemExit(2)


def git(repo: Path, *args: str) -> str:
    proc = subprocess.run(  # noqa: S603 -- fixed git argv, absolute binary, no shell
        [GIT, "-C", str(repo), *args], capture_output=True, text=True, check=False
    )
    if proc.returncode != 0:
        die(f"git {' '.join(args[:3])} failed: {proc.stderr.strip()}")
    return proc.stdout


def decoration(line: str) -> str:
    """The leading comment markers and whitespace of ``line``."""
    m = DECORATION.match(line)
    return m.group(0) if m else ""


def undecorated(line: str) -> str:
    return line[len(decoration(line)) :].strip()


def notices_in(text: str) -> tuple[str, ...]:
    """Copyright notices at the top of ``text`` naming anyone but the owner."""
    return tuple(
        body
        for body in (undecorated(line) for line in text.splitlines()[:NOTICE_SCAN])
        if SOURCE_NOTICE.match(body) and OWNER not in body
    )


def load_provenance(path: Path, repo: Path) -> Provenance:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        die(f"cannot read {path}: {exc}")
        raise
    prov = Provenance()
    for key, src in data.get("sources", {}).items():
        notices = list(src.get("notices", []))
        for origin in src.get("notices_from", []):
            found = notices_in((repo / origin).read_text(encoding="utf-8"))
            if not found:
                die(f"{path}: source {key}: {origin} carries no copyright notice")
            notices += found
        prov.sources[key] = Source(tuple(dict.fromkeys(notices)), tuple(src["licences"]))
    for fam in data.get("families", []):
        prov.families.append(Family(fam["name"], re.compile(fam["match"]), tuple(fam["from"])))
    for file, entry in data.get("ports", {}).items():
        prov.ports[file] = list(entry["from"])
    prov.not_ports = set(data.get("not_ports", {}))
    prov.mirrors = {name: re.compile(m["match"]) for name, m in data.get("mirrors", {}).items()}
    overlap = prov.not_ports & prov.ports.keys()
    if overlap:
        die(f"{path}: listed as both carrying and not carrying other code: {sorted(overlap)}")
    named = {s for f in prov.families for s in f.sources}
    named |= {s for v in prov.ports.values() for s in v}
    unknown = named - prov.sources.keys()
    if unknown:
        die(f"{path}: unknown sources {sorted(unknown)}")
    return prov


def comment_style(path: str) -> str | None:
    """The comment syntax a notice added to ``path`` uses, or None."""
    p = PurePosixPath(path)
    if p.name in HASH_NAMES or p.suffix in HASH_EXT:
        return "hash"
    if p.suffix in BLOCK_EXT:
        return "block"
    if p.suffix in SLASH_EXT:
        return "slash"
    return None


def licence_ids(expr: str) -> list[str]:
    return [tok for tok in expr.split() if tok not in OPERATORS]


def is_declaration(expr: str) -> bool:
    ids = licence_ids(expr)
    return bool(ids) and bool(LICENCE_LIKE.match(ids[0]))


def declarations(text: str) -> list[list[str]]:
    """Every licence expression the text declares, as identifier lists."""
    return [
        licence_ids(m.group("expr"))
        for m in TAG_RE.finditer(text)
        if is_declaration(m.group("expr"))
    ]


def is_foreign(decls: Iterable[list[str]]) -> bool:
    return any(any(i not in FORK_IDS for i in ids) or set(ids) == {"MIT"} for ids in decls)


def has_third_party_copyright(text: str) -> bool:
    for line in text.splitlines()[:COPYRIGHT_SCAN]:
        if OWNER.lower() in line.lower():
            continue
        if COPYRIGHT_NOTICE.search(undecorated(line)) or FILE_COPYRIGHT.search(line):
            return True
    return False


def upstream_counterpart(path: str, upstream_paths: frozenset[str]) -> bool:
    if path in upstream_paths:
        return True
    return any(
        path.startswith(fork) and upstream + path[len(fork) :] in upstream_paths
        for fork, upstream in UPSTREAM_REMAP
    )


def signal_names(upstream_paths: Iterable[str]) -> frozenset[str]:
    """Upstream source-file names specific enough to identify their origin."""
    return frozenset(
        name
        for name in (PurePosixPath(p).name for p in upstream_paths)
        if SOURCE_REF.fullmatch(name)
        and len(name) >= MIN_SIGNAL_NAME
        and name not in NO_SIGNAL_NAMES
    )


def is_blank_comment(line: str) -> bool:
    return line.strip() in BLANK_COMMENT


def is_closer(line: str) -> bool:
    return line.strip() in CLOSERS


def prose_blocks(lines: Sequence[str]) -> list[tuple[int, int]]:
    """Inclusive line spans of prose licence grants."""
    spans = []
    i = 0
    while i < len(lines):
        m = PROSE_START.search(lines[i])
        if not m:
            i += 1
            continue
        window = range(i, min(len(lines), i + PROSE_SCAN))
        if m.group(0).startswith("Use of"):
            end = next((j for j in window if PROSE_GO_END.search(lines[j])), None)
        else:
            end = next((j for j in window if PROSE_LONG_END in lines[j]), None)
            if end is None:
                end = next((j for j in window if PROSE_URL_END.search(lines[j])), None)
        if end is None:
            # A grant truncated to its opening sentence is still a grant.
            end = i
        spans.append((i, end))
        i = end + 1
    return spans


def suppression_lines(lines: list[str]) -> set[int]:
    """Indices of clang-tidy suppression comments, block comments in full.

    A NOLINT justification explains a lint exception ("this file mirrors the C
    spelling of the surface it exercises"); it never says where code came from,
    and its wording would otherwise read as a derivation statement.
    """
    found: set[int] = set()
    i = 0
    while i < len(lines):
        line = lines[i]
        at = line.find("NOLINT")
        opener = line.rfind("/*", 0, at) if at >= 0 else -1
        if at < 0 or (opener < 0 and "//" not in line[:at]):
            i += 1
            continue
        end = i
        if opener >= 0:
            while end < len(lines) - 1 and "*/" not in lines[end][opener if end == i else 0 :]:
                end += 1
        found.update(range(i, end + 1))
        i = end + 1
    return found


def descriptive_lines(text: str) -> list[str]:
    """The file's lines minus its licence scaffolding, includes and lint notes.

    Grants, tags, copyright notices and blank comment lines are dropped so the
    window the detector reads is the same before and after a rewrite; includes
    name dependencies, which are not origins; clang-tidy suppression comments
    justify an exception, which is not an origin either.
    """
    lines = text.splitlines()
    grant = {i for start, end in prose_blocks(lines) for i in range(start, end + 1)}
    grant |= suppression_lines(lines)
    return [
        line
        for i, line in enumerate(lines)
        if i not in grant
        and TAG not in line
        and not SOURCE_NOTICE.match(undecorated(line))
        and not is_blank_comment(line)
        and not IMPORT_LINE.match(line)
    ]


def derivation_statements(text: str, up: Upstream) -> list[str]:
    """Header lines that say the file carries upstream or third-party code."""
    lines = descriptive_lines(text)[:HEADER_SCAN]
    found = []
    for i, line in enumerate(lines):
        if not DERIVATION_VERB.search(line):
            continue
        window = " ".join(lines[max(0, i - 1) : i + 3])
        refs = SOURCE_REF.findall(window)
        upstream_ref = any(
            upstream_counterpart(ref, up.paths) or PurePosixPath(ref).name in up.signal
            for ref in refs
        )
        if upstream_ref or THIRD_PARTY_NAMES.search(window):
            found.append(line.strip())
    return found


def normalised_prefix(line: str) -> str:
    marker = decoration(line).strip()
    if not marker:
        return ""
    if marker.startswith(("*", "/*")):
        return " * "
    if marker.startswith("//"):
        return "// "
    return marker + " "


def header_for(style: str, year: str) -> list[str]:
    if style == "hash":
        return [f"# Copyright {year} {OWNER}\n", f"# {TAG} {TARGET}\n"]
    if style == "slash":
        return [f"// Copyright {year} {OWNER}\n", f"// {TAG} {TARGET}\n"]
    return ["/*\n", f" * Copyright {year} {OWNER}\n", " *\n", f" * {TAG} {TARGET}\n", " */\n"]


def owner_copyright_line(lines: Sequence[str]) -> int | None:
    """Index of the owner's copyright line near the top, if there is one."""
    return next(
        (
            i
            for i, line in enumerate(lines[:NOTICE_SCAN])
            if OWNER in line and COPYRIGHT_LINE.match(undecorated(line))
        ),
        None,
    )


def insert_header(lines: list[str], style: str, year: str) -> list[str]:
    """Give an untagged file its tag: under its copyright line, or as a new header."""
    owner = owner_copyright_line(lines)
    if owner is not None and decoration(lines[owner]).strip():
        tag = f"{decoration(lines[owner])}{TAG} {TARGET}\n"
        return [*lines[: owner + 1], tag, *lines[owner + 1 :]]
    at = 1 if lines and lines[0].startswith("#!") else 0
    if style == "hash" and len(lines) > at and CODING_COOKIE.match(lines[at]):
        at += 1
    if at and not lines[at - 1].endswith("\n"):
        lines[at - 1] += "\n"
    header = header_for(style, year)
    rest = lines[at:]
    if rest and rest[0].strip():
        header.append("\n")
    return lines[:at] + header + rest


def retarget(text: str, mapping: dict[str, str] | None) -> str:
    """Point every licence tag at TARGET, or, given ``mapping``, repair identifiers."""

    def swap(m: re.Match[str]) -> str:
        expr = m.group("expr")
        if not is_declaration(expr):
            return m.group(0)
        gap = m.group("gap") or " "
        if mapping is None:
            return f"{TAG}{gap}{TARGET}"
        return f"{TAG}{gap}{' '.join(mapping.get(tok, tok) for tok in expr.split())}"

    return TAG_RE.sub(swap, text)


def drop_prose(lines: list[str], start: int, end: int) -> None:
    """Delete a prose grant and the comment scaffolding it leaves dangling."""
    prefix = normalised_prefix(lines[start])
    del lines[start : end + 1]
    seam = start
    before = lines[seam - 1] if seam > 0 else ""
    if not prefix and OWNER in before and "copyright" in before.lower():
        # A docstring grant: its copyright line goes too, the header replaces both.
        del lines[seam - 1]
        seam -= 1
    while 0 < seam < len(lines) and is_blank_comment(lines[seam - 1]):
        if not (is_blank_comment(lines[seam]) or is_closer(lines[seam])):
            break
        del lines[seam - 1]
        seam -= 1


def rewrite(text: str, path: str, year: Callable[[], str]) -> str:
    """The content ``path`` has once it is EUPL-1.2. Idempotent.

    ``year`` supplies the copyright year of a notice that has to be created; it
    is only called when one is.
    """
    lines = text.splitlines(keepends=True)
    spans = prose_blocks(lines)
    tagged = bool(declarations(text))
    for start, end in reversed(spans):
        prefix = normalised_prefix(lines[start])
        if not tagged and prefix and start == spans[0][0]:
            lines[start : end + 1] = [f"{prefix}{TAG} {TARGET}\n"]
            after = start + 1
            if (
                after + 1 < len(lines)
                and is_blank_comment(lines[after])
                and is_closer(lines[after + 1])
            ):
                del lines[after]
        else:
            drop_prose(lines, start, end)
    text = retarget("".join(lines), None)
    text = PY_LICENSE_ATTR.sub(
        lambda m: f"{m.group('head')}{m.group('q')}{TARGET}{m.group('q')}", text
    )
    style = comment_style(path)
    if not declarations(text) and style is not None:
        lines = text.splitlines(keepends=True)
        needs_year = owner_copyright_line(lines) is None
        text = "".join(insert_header(lines, style, year() if needs_year else ""))
    return text


def repair(text: str) -> str:
    """Fix identifiers that do not exist in a file that keeps its terms (ADR-1255)."""
    return retarget(text, REPAIRS)


def resolve_sources(prov: Provenance, names: Iterable[str]) -> list[Source]:
    return [prov.sources[name] for name in names]


def holder(notice: str) -> str:
    """Who a copyright notice credits, without the years: comparable across files."""
    body = re.sub(r"^copyright\s*(?:\(c\)|©)?\s*[\d,\s-]*", "", notice, flags=re.I)
    return body.strip().rstrip(".").lower()


def tag_prose_grant(text: str) -> str:
    """Replace a kept file's BSD+Patent prose grant with the tag it means."""
    lines = text.splitlines(keepends=True)
    spans = [(a, b) for a, b in prose_blocks(lines) if "BSD+Patent" in lines[a]]
    if not spans:
        return text
    start, end = spans[0]
    prefix = normalised_prefix(lines[start]) or " * "
    lines[start : end + 1] = [f"{prefix}{TAG} {UPSTREAM_LICENCE}\n"]
    if (
        start + 2 < len(lines)
        and is_blank_comment(lines[start + 1])
        and is_closer(lines[start + 2])
    ):
        del lines[start + 1]
    return "".join(lines)


def attribute(text: str, sources: Sequence[Source], path: str = "<text>") -> str:
    """Restore the notices and licences of the code a kept file carries. Idempotent."""
    lines = text.splitlines(keepends=True)
    anchor = owner_copyright_line(lines)
    if anchor is None:
        anchor = next(
            (
                i
                for i, line in enumerate(lines[:NOTICE_SCAN])
                if SOURCE_NOTICE.match(undecorated(line))
            ),
            None,
        )
    if anchor is None:
        die(f"{path}: no copyright line to anchor restored notices on")
        raise AssertionError
    deco = decoration(lines[anchor])
    credited = {holder(undecorated(line)) for line in lines[:NOTICE_SCAN]}
    missing = [n for s in sources for n in s.notices if holder(n) not in credited]
    lines[anchor:anchor] = [f"{deco}{n}\n" for n in dict.fromkeys(missing)]
    text = repair("".join(lines))
    extra = list(dict.fromkeys(lic for s in sources for lic in s.licences))
    if not declarations(text) and set(extra) != {UPSTREAM_LICENCE}:
        # The prose grant already says BSD-2-Clause-Patent; only a licence it
        # cannot express needs the grant turned into a tag first.
        text = tag_prose_grant(text)

    def extend(m: re.Match[str]) -> str:
        expr = m.group("expr")
        if not is_declaration(expr):
            return m.group(0)
        ids = licence_ids(expr)
        wanted = [lic for lic in extra if lic not in ids]
        if not wanted:
            return m.group(0)
        return f"{TAG}{m.group('gap') or ' '}{' AND '.join([expr, *wanted])}"

    return TAG_RE.sub(extend, text, count=1)


def is_candidate_path(path: str) -> bool:
    if path in SELF or path.startswith(EXCLUDED_PREFIXES) or path.endswith(EXCLUDED_SUFFIXES):
        return False
    return PurePosixPath(path).name not in EXCLUDED_NAMES


def has_notice(text: str) -> bool:
    return bool(declarations(text)) or bool(prose_blocks(text.splitlines()))


def read_text(path: Path) -> str | None:
    """Decoded content; None for binary, a lone NUL for text that is not UTF-8."""
    data = path.read_bytes()
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return "\0"


def person_email(person: str) -> str:
    if "<" not in person:
        return ""
    return person[person.rfind("<") + 1 : person.rfind(">")].strip().lower()


def is_owner_or_tool(person: str) -> bool:
    email = person_email(person)
    lowered = person.lower()
    return (
        email in OWNER_EMAILS
        or email.endswith("noreply@anthropic.com")
        or "[bot]" in lowered
        or "renovate" in lowered
    )


def outside_people(repo: Path, path: str, upstream_ref: str) -> set[str]:
    """Non-owner people in the fork-local history of ``path``, following renames."""
    log = git(
        repo,
        "log",
        "--follow",
        "--format=%an <%ae>%x1f%(trailers:key=Co-authored-by,valueonly,separator=%x1f)%x1e",
        "HEAD",
        "--not",
        upstream_ref,
        "--",
        path,
    )
    people = set()
    for record in log.split("\x1e"):
        for raw in record.split("\x1f"):
            person = raw.strip()
            if person and not is_owner_or_tool(person):
                people.add(person)
    return people


def static_verdict(path: str, text: str, up: Upstream, prov: Provenance) -> str | None:
    """The first veto that needs no history, or None when all pass."""
    name = PurePosixPath(path).name
    if path.startswith("compat/python-vmaf/"):
        reason = "compat-python-vmaf"
    elif upstream_counterpart(path, up.paths):
        reason = "upstream-path"
    elif name not in NO_SIGNAL_NAMES and name in up.names:
        reason = "upstream-name"
    elif prov.mirror_of(path) is not None:
        reason = "vendored-mirror"
    elif prov.port_sources(path) is not None:
        reason = "documented-port"
    elif path not in prov.not_ports and derivation_statements(text, up):
        reason = "provenance-unreviewed"
    elif has_third_party_copyright(text):
        reason = "third-party-copyright"
    elif is_foreign(declarations(text)):
        reason = "foreign-licence"
    else:
        reason = None
    return reason


def classify(repo: Path, upstream_ref: str, prov: Provenance, jobs: int = 16) -> list[Verdict]:
    paths = frozenset(git(repo, "ls-tree", "-r", "--name-only", upstream_ref).splitlines())
    up = Upstream(paths, frozenset(PurePosixPath(p).name for p in paths), signal_names(paths))
    verdicts: list[Verdict] = []
    pending: list[str] = []
    for row in git(repo, "ls-files", "-s").splitlines():
        meta, path = row.split("\t", 1)
        full = repo / path
        if meta.startswith("120000") or not is_candidate_path(path) or not full.is_file():
            continue
        text = read_text(full)
        if text is None:
            continue
        if text == "\0":
            if comment_style(path):
                verdicts.append(Verdict(path, "not-utf8"))
            continue
        if not has_notice(text) and comment_style(path) is None:
            continue
        reason = static_verdict(path, text, up, prov)
        if reason:
            verdicts.append(Verdict(path, reason))
        else:
            pending.append(path)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        found = list(pool.map(partial(outside_people, repo, upstream_ref=upstream_ref), pending))
    verdicts += [
        Verdict(p, "outside-contribution" if people else "moves")
        for p, people in zip(pending, found, strict=True)
    ]
    return sorted(verdicts, key=lambda v: v.path)


def first_year(repo: Path, path: str, upstream_ref: str) -> str:
    """Year of the first fork-local commit that touched ``path``.

    Not ``--follow``: rename detection pairs small or empty files with unrelated
    upstream files and would date a new file by someone else's history.
    """
    years = git(
        repo, "log", "--format=%ad", "--date=format:%Y", "HEAD", "--not", upstream_ref, "--", path
    ).split()
    if years:
        return years[-1]
    return git(repo, "log", "-1", "--format=%ad", "--date=format:%Y", "HEAD").strip()


def wanted_content(
    repo: Path, upstream_ref: str, prov: Provenance, verdict: Verdict, text: str
) -> str:
    if verdict.moves:
        return rewrite(text, verdict.path, partial(first_year, repo, verdict.path, upstream_ref))
    origins = prov.port_sources(verdict.path)
    if verdict.reason == "documented-port" and origins is not None:
        return attribute(text, resolve_sources(prov, origins), verdict.path)
    return repair(text)


def stale_reviews(repo: Path, prov: Provenance) -> list[str]:
    return sorted(p for p in (*prov.ports, *prov.not_ports) if not (repo / p).is_file())


def action_of(verdict: Verdict) -> str:
    if verdict.moves:
        return "relicense"
    return "attribute" if verdict.reason == "documented-port" else "repair"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n", 1)[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list", action="store_true", help="print a verdict per candidate")
    mode.add_argument("--write", action="store_true", help="apply every pending change")
    mode.add_argument("--check", action="store_true", help="exit 1 if any change is pending")
    parser.add_argument(
        "--upstream-ref", default="upstream/master", help="upstream tree (default: %(default)s)"
    )
    parser.add_argument("--repo", type=Path, default=Path.cwd(), help="checkout (default: cwd)")
    parser.add_argument(
        "--provenance",
        type=Path,
        default=PROVENANCE,
        help="reviewed provenance (default: %(default)s)",
    )
    parser.add_argument(
        "--jobs", type=int, default=16, help="parallel history queries (default: %(default)s)"
    )
    args = parser.parse_args(argv)

    repo = Path(git(args.repo, "rev-parse", "--show-toplevel").strip())
    git(repo, "rev-parse", "--verify", "--quiet", args.upstream_ref + "^{tree}")
    prov = load_provenance(args.provenance, repo)
    verdicts = classify(repo, args.upstream_ref, prov, args.jobs)
    tally = Counter(v.reason for v in verdicts)
    print(", ".join(f"{r} {n}" for r, n in sorted(tally.items())), file=sys.stderr)

    if args.list:
        for v in verdicts:
            print(f"{v.reason}\t{v.path}")
        return 0

    pending = 0
    for v in verdicts:
        if v.reason in ("not-utf8", "vendored-mirror"):
            continue
        current = (repo / v.path).read_text(encoding="utf-8")
        wanted = wanted_content(repo, args.upstream_ref, prov, v, current)
        if current == wanted:
            continue
        pending += 1
        if args.write:
            (repo / v.path).write_text(wanted, encoding="utf-8")
        else:
            print(f"{action_of(v)}\t{v.path}")
    stale = stale_reviews(repo, prov)
    for path in stale:
        print(f"stale-review\t{path}")
    print(f"{'rewrote' if args.write else 'pending'}: {pending}", file=sys.stderr)
    return 1 if stale or (args.check and pending) else 0


if __name__ == "__main__":
    raise SystemExit(main())
