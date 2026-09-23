#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that every `adr/NNNN-slug.md` link under docs/ resolves to a real file.

An ADR link carries the decision's identity twice -- once as the number and once
as the slug -- and either half can go stale independently:

  stale slug    the ADR was renamed. The number still names the right decision,
                so the link is repaired FROM THE NUMBER.

  wrong number  the writer had the right ADR in mind, wrote its slug correctly,
                and mistyped or mis-remembered the number. The slug still names
                the right decision, so the link is repaired FROM THE SLUG -- and
                so is the `[ADR-NNNN]` link text, which carries the wrong number
                too.

Slug wins when both could apply, and that ordering is load-bearing rather than a
preference. The reason is in the history: `af227b026` (PR #310, 2026-05-03) and
`fb14bc332` (PR #752, 2026-05-10) were ADR collision sweeps that renumbered
duplicate-numbered ADRs -- the second renamed 50 files, moving
`0241-vmaf-tiny-v3-mlp-medium.md` to `0389-vmaf-tiny-v3-mlp-medium.md` and 27
others into the 0388-0415 band. Each sweep moved the file and its index fragment
and left every inbound citation pointing at the old number. So for that whole
class the slug is the half that survived and the number is the half that rotted. Measured on this repository 2026-09-23: of 73 broken links, 36 had a
slug matching a real ADR under a different number and 37 had a stale slug under a
correct number. Repairing all 73 by number was tried first and was wrong for the
36: it silently repointed them at unrelated decisions -- `[ADR-0241]` in a
tiny-AI evaluation digest became a link to the HIP PSNR kernel-template ADR --
which resolves, reads as authoritative, and is worse than the dead link it
replaced. An independent two-pass review caught all 36.

`mkdocs build --strict` catches none of this: it validates the nav and page
rendering, not the target of an inline relative link.

Neither half resolving is reported and never auto-fixed: the citation is either a
typo or names an ADR nobody wrote, and only a human knows which. The same applies
when a slug or a number is carried by more than one file.

What this does NOT check is a citation where both halves agree and both are the
wrong decision. That needs review, not a parser -- see
T-STALE-ADR-CITATIONS-2026-09-16.

Exit: 0 clean, 1 findings, 2 usage error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# ](../../adr/0123-some-slug.md) and ](adr/0123-some-slug.md) alike.
LINK = re.compile(r"\[([^\]]{0,80})\]\(((?:\.\./)*)adr/([0-9]{4})-([a-z0-9-]+\.md)\)")


def adr_index(adr_dir: Path) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    """Index the ADR corpus by number and by slug."""
    by_number: dict[str, list[str]] = {}
    by_slug: dict[str, list[str]] = {}
    for path in sorted(adr_dir.glob("[0-9][0-9][0-9][0-9]-*.md")):
        by_number.setdefault(path.name[:4], []).append(path.name)
        by_slug.setdefault(path.name[5:], []).append(path.name)
    return by_number, by_slug


def resolve_target(
    number: str,
    slug: str,
    by_number: dict[str, list[str]],
    by_slug: dict[str, list[str]],
) -> tuple[str | None, str]:
    """Pick the file a broken link meant, and say which half identified it.

    Slug first: a slug that names a real ADR under a different number means the
    number is the wrong half, and repairing from the number would repoint the
    link at an unrelated decision. See the module docstring for the measurement
    that settles this ordering.
    """
    slug_hits = [name for name in by_slug.get(slug, []) if name != f"{number}-{slug}"]
    if len(slug_hits) == 1:
        return slug_hits[0], "slug"
    number_hits = by_number.get(number, [])
    if len(number_hits) == 1:
        return number_hits[0], "number"
    if slug_hits or number_hits:
        return None, "ambiguous"
    return None, "unknown"


def repair_one_file(
    path: Path,
    by_number: dict[str, list[str]],
    by_slug: dict[str, list[str]],
    known: set[str],
    *,
    fix: bool,
) -> tuple[list[str], list[str], int]:
    """Resolve one file's adr/ links. Returns (findings, unfixable, repaired)."""
    text = path.read_text(encoding="utf-8", errors="ignore")
    findings: list[str] = []
    unfixable: list[str] = []
    repaired = 0
    out: list[str] = []
    cursor = 0

    for match in LINK.finditer(text):
        out.append(text[cursor : match.start()])
        cursor = match.end()
        label, prefix, number, slug = (
            match.group(1),
            match.group(2),
            match.group(3),
            match.group(4),
        )
        if f"{number}-{slug}" in known:
            out.append(match.group(0))
            continue

        line = text[: match.start()].count("\n") + 1
        target, how = resolve_target(number, slug, by_number, by_slug)
        if target is None:
            where = "no ADR carries that number or that slug"
            if how == "ambiguous":
                where = "the number and the slug do not agree on one file"
            unfixable.append(f"{path}:{line}: [{label}](adr/{number}-{slug}) -- {where}")
            out.append(match.group(0))
            continue

        if fix:
            repaired += 1
            # A slug-resolved link carries the wrong number in its text too.
            # Both spellings occur: a bare "ADR-0241" and a descriptive
            # "ADR-0241 - vmaf_tiny_v3 ship decision". Only the number is
            # rewritten; whatever the author wrote after it is left alone.
            new_label = label
            if how == "slug" and re.match(rf"ADR-{number}\b", label):
                new_label = re.sub(rf"^ADR-{number}\b", f"ADR-{target[:4]}", label)
            out.append(f"[{new_label}]({prefix}adr/{target})")
            continue

        findings.append(f"{path}:{line}: adr/{number}-{slug} -> adr/{target} (by {how})")
        out.append(match.group(0))

    out.append(text[cursor:])
    if repaired:
        path.write_text("".join(out), encoding="utf-8")
    return findings, unfixable, repaired


def scan(
    docs: Path, by_number: dict[str, list[str]], by_slug: dict[str, list[str]], *, fix: bool
) -> tuple[list[str], list[str], int]:
    """Return (repairable findings, unfixable findings, repaired count)."""
    findings: list[str] = []
    unfixable: list[str] = []
    repaired = 0
    known = {name for names in by_number.values() for name in names}

    for path in sorted(docs.rglob("*.md")):
        file_findings, file_unfixable, file_repaired = repair_one_file(
            path, by_number, by_slug, known, fix=fix
        )
        findings.extend(file_findings)
        unfixable.extend(file_unfixable)
        repaired += file_repaired

    return findings, unfixable, repaired


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--docs", type=Path, default=Path("docs"))
    parser.add_argument(
        "--fix",
        action="store_true",
        help="repair links whose number or slug identifies exactly one ADR",
    )
    args = parser.parse_args(argv[1:])

    docs = args.docs
    adr_dir = docs / "adr"
    if not adr_dir.is_dir():
        print(f"error: {adr_dir}: no such directory", file=sys.stderr)
        return 2

    by_number, by_slug = adr_index(adr_dir)
    findings, unfixable, repaired = scan(docs, by_number, by_slug, fix=args.fix)

    if repaired:
        print(f"check-adr-links: repaired {repaired} link(s)")
    for finding in findings:
        print(f"::error title=broken ADR link::{finding}", file=sys.stderr)
    for finding in unfixable:
        print(f"::error title=unresolvable ADR link::{finding}", file=sys.stderr)

    if findings:
        print(
            f"\n{len(findings)} link(s) name an ADR that exists under a different "
            "number or slug. Run: python3 scripts/ci/check-adr-links.py --fix\n"
            "A '(by slug)' repair also rewrites the [ADR-NNNN] text, because a link "
            "the slug resolved carries the wrong number in both halves.",
            file=sys.stderr,
        )
    if unfixable:
        print(
            f"\n{len(unfixable)} link(s) cannot be resolved automatically. Fix the "
            "citation by hand, or drop the link and cite the ADR by number in plain "
            "text so a reader is not sent to a 404.",
            file=sys.stderr,
        )
    if findings or unfixable:
        return 1

    print(
        f"check-adr-links: OK ({len(by_number)} ADR numbers, every adr/ link under {docs} resolves)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
