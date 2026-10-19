# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The checks that hold ``docs/credits.yaml`` to the repository (ADR-2485).

Five checks, each returning the lines it would print:

1. ``page_drift``: ``docs/credits.md`` equals what the list renders.
2. ``uncredited_paths``: a vendored or inherited third-party path has no entry.
   The paths come from ``REUSE.toml`` (any annotation whose licence is not the
   project's own), from directories named ``third_party``, ``3rdparty`` or
   ``vendor``, from notice files, from font files and from copyright lines that
   name a holder other than the project and its contributors.
3. ``unused_licenses``: a ``LICENSES/*.txt`` text that no entry and no project
   code uses.
4. ``uncredited_adaptations``: a skill or agent file that declares where it was
   derived from (``derived_from`` or ``Adapted from [..](..)``) with no entry
   for that upstream.
5. ``missing_paths``: an entry names a path or an evidence file that is not in
   the checkout.
"""

from __future__ import annotations

import datetime
import re
from pathlib import Path

import tomllib

from scripts.docs.credits_lib import (
    OWN_LICENSES,
    CreditsError,
    Entry,
    Waiver,
    apply_blocks,
    blocks,
    covered,
    license_tokens,
    path_matches,
)

THIRD_PARTY_DIRS = ("third_party", "3rdparty", "vendor")
NOTICE_RE = re.compile(
    r"(^|/)(NOTICE[^/]*|THIRD-PARTY[^/]*|COPYING[^/]*|LICENSE[^/]*|[^/]*\.NOTICE)$"
)
FONT_SUFFIXES = (".woff", ".woff2", ".ttf", ".otf")
SOURCE_SUFFIXES = (
    ".c", ".h", ".cpp", ".hpp", ".cu", ".cuh", ".hip", ".asm", ".py", ".go", ".rs", ".sh", ".m", ".js",
)  # fmt: skip
COPYRIGHT_RE = re.compile(
    r"^\s*(?:/\*+|\*|//|#|;|%)?\s*(?:Copyright(?:\s*\(c\))?|©|\(c\)(?=\s+\d))\s+(?P<rest>.+)$",
    re.MULTILINE,
)
HEADER_LINES = 40
SKIP_TREES = ("docs/", "LICENSES/", ".workingdir/", "testdata/")
ADAPTED_RE = re.compile(r"Adapted from \[[^\]]+\]\((https?://[^)\s]+)\)")
DERIVED_RE = re.compile(r"^\s*derived_from:\s*[\"']?(https?://[^\s\"')]+)", re.M)
SKILL_FILES = re.compile(r"(^|/)(SKILL\.md|[^/]+\.agent\.md)$|^\.agents/agents/[^/]+\.md$")


def normalise_url(url: str) -> str:
    """``host/owner/repo`` in lower case, so scheme, ``.git`` and tails do not matter."""
    bare = re.sub(r"^https?://(www\.)?", "", url.strip().lower()).rstrip("/")
    bare = re.sub(r"\.git$", "", bare)
    return "/".join(bare.split("/")[:3])


def _read(root: Path, name: str) -> str:
    return (root / name).read_text(encoding="utf-8", errors="replace")


def page_drift(root: Path, entries: list[Entry]) -> list[str]:
    page_path = root / "docs" / "credits.md"
    if not page_path.is_file():
        return ["docs/credits.md is missing; run `make docs-fragments-write`"]
    current = page_path.read_text(encoding="utf-8")
    try:
        wanted = apply_blocks(current, blocks(entries))
    except CreditsError as err:
        return [str(err)]
    if wanted != current:
        return ["docs/credits.md differs from docs/credits.yaml; run `make docs-fragments-write`"]
    return []


def reuse_third_party_paths(root: Path, files: list[str]) -> dict[str, str]:
    """Map each file to the non-own licence that REUSE.toml gives it."""
    reuse = root / "REUSE.toml"
    if not reuse.is_file():
        return {}
    annotations = tomllib.loads(reuse.read_text(encoding="utf-8")).get("annotations", [])
    found: dict[str, str] = {}
    for note in annotations:
        expr = str(note.get("SPDX-License-Identifier", ""))
        if not set(license_tokens(expr)) - set(OWN_LICENSES):
            continue
        for pattern in note.get("path", []):
            for name in files:
                if path_matches(pattern, name):
                    found[name] = expr
    return found


def own_holders(root: Path) -> set[str]:
    """Copyright holders REUSE.toml records on project-licensed annotations."""
    reuse = root / "REUSE.toml"
    holders: set[str] = set()
    if not reuse.is_file():
        return holders
    for note in tomllib.loads(reuse.read_text(encoding="utf-8")).get("annotations", []):
        if set(license_tokens(str(note.get("SPDX-License-Identifier", "")))) - set(OWN_LICENSES):
            continue
        text = note.get("SPDX-FileCopyrightText", [])
        for item in [text] if isinstance(text, str) else text:
            holders.add(re.sub(r"^[\d\-\u2013, ]+", "", item).split("<")[0].strip().lower())
    return holders


def _is_own(rest: str, holders: set[str]) -> bool:
    low = rest.lower()
    if any(word in low for word in ("lusoris", "netflix", "vmaf", "template", "<year>", "[xxxx]")):
        return True
    return any(h and h in low for h in holders)


def foreign_copyright_files(root: Path, files: list[str]) -> dict[str, str]:
    """Source files whose header names a holder that is not the project or a contributor."""
    holders = own_holders(root)
    found: dict[str, str] = {}
    for name in files:
        if not name.endswith(SOURCE_SUFFIXES) or name.startswith(SKIP_TREES):
            continue
        head = _read(root, name).splitlines()[:HEADER_LINES]
        for line in head:
            match = COPYRIGHT_RE.match(line)
            if match and not _is_own(match.group("rest"), holders):
                found[name] = match.group("rest").strip()[:60]
                break
    return found


def structural_paths(files: list[str]) -> dict[str, str]:
    """Third-party directories, notice files and fonts, by name alone."""
    found: dict[str, str] = {}
    for name in files:
        parts = name.split("/")
        if any(p in THIRD_PARTY_DIRS for p in parts[:-1]):
            found[name] = "a third_party, 3rdparty or vendor directory"
        elif name.lower().endswith(FONT_SUFFIXES):
            found[name] = "a font file"
        elif (
            NOTICE_RE.search(name)
            and not name.endswith(".md")
            and len(parts) > 1
            and not name.startswith("LICENSES/")
        ):
            found[name] = "a notice or licence file"
    return found


def waived(name: str, waivers: list[Waiver], today: datetime.date) -> bool:
    return any(
        w.path == name and w.rule == "uncredited-path" and w.expires >= today for w in waivers
    )


def waiver_findings(
    waivers: list[Waiver], found: dict[str, str], entries: list[Entry], today: datetime.date
) -> list[str]:
    """An expired waiver, or one that excuses nothing, is itself a finding."""
    lines = []
    for w in waivers:
        if w.expires < today:
            lines.append(
                f"{w.path}: the exception expired on {w.expires}; add an entry or renew it"
            )
        elif w.path not in found or covered(w.path, entries):
            lines.append(f"{w.path}: the exception excuses nothing; remove it")
    return lines


def uncredited_paths(
    root: Path,
    files: list[str],
    entries: list[Entry],
    waivers: list[Waiver] | None = None,
    today: datetime.date | None = None,
) -> list[str]:
    waivers = waivers or []
    today = today or datetime.date.today()
    seen: dict[str, str] = {}
    for source in (
        reuse_third_party_paths(root, files),
        structural_paths(files),
        foreign_copyright_files(root, files),
    ):
        for name, why in source.items():
            seen.setdefault(name, why)
    lines = [
        f"{name}: {why}; no credits entry lists this path"
        for name, why in sorted(seen.items())
        if not covered(name, entries) and not waived(name, waivers, today)
    ]
    return lines + waiver_findings(waivers, seen, entries, today)


def unused_licenses(root: Path, entries: list[Entry]) -> list[str]:
    used = set(OWN_LICENSES)
    for entry in entries:
        used.update(license_tokens(entry.license))
    lines = []
    for text in sorted((root / "LICENSES").glob("*.txt")):
        if text.stem not in used:
            lines.append(
                f"LICENSES/{text.name}: no credits entry and no project code uses {text.stem}"
            )
    return lines


def adaptation_urls(root: Path, files: list[str]) -> dict[str, list[str]]:
    """Map each skill or agent file to the upstream URLs it declares."""
    found: dict[str, list[str]] = {}
    for name in files:
        if not SKILL_FILES.search(name):
            continue
        text = _read(root, name)
        urls = DERIVED_RE.findall(text) + ADAPTED_RE.findall(text)
        if urls:
            found[name] = urls
    return found


def uncredited_adaptations(root: Path, files: list[str], entries: list[Entry]) -> list[str]:
    credited = {normalise_url(e.url) for e in entries}
    lines = []
    for name, urls in sorted(adaptation_urls(root, files).items()):
        for url in urls:
            if normalise_url(url) not in credited:
                lines.append(f"{name}: derived from {url}; no credits entry has that upstream URL")
    return lines


def missing_paths(files: list[str], entries: list[Entry]) -> list[str]:
    lines = []
    for entry in entries:
        for pattern in (*entry.paths, *entry.evidence):
            if not any(path_matches(pattern, name) for name in files):
                lines.append(f"{entry.id}: path '{pattern}' matches no file in the checkout")
    return lines


def run_all(
    root: Path,
    files: list[str],
    entries: list[Entry],
    waivers: list[Waiver] | None = None,
    today: datetime.date | None = None,
) -> dict[str, list[str]]:
    """Every check's findings, keyed by a stable check name."""
    return {
        "page-drift": page_drift(root, entries),
        "uncredited-path": uncredited_paths(root, files, entries, waivers, today),
        "unused-licence": unused_licenses(root, entries),
        "uncredited-adaptation": uncredited_adaptations(root, files, entries),
        "missing-path": missing_paths(files, entries),
    }
