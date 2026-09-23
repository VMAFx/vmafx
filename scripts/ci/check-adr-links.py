#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that every ADR link under docs/ resolves to a real file.

Both spellings count: the qualified `adr/NNNN-slug.md` used from elsewhere in
the tree, and the bare `NNNN-slug.md` that ADRs use to cite each other. Scoping
this to the qualified form alone hid 237 broken sibling links inside docs/adr/
while the gate reported the tree clean.

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

A number is only trusted when the ADR it names corroborates the slug -- the
target has to mention, near its top, at least one topic word from what the
citation says it is about. Measured inside docs/adr/: 17 citations resolved by
number to an ADR with nothing in common with the slug, because the number had
been reallocated. `[ADR-0033](0033-hip-applicability.md)` would have become a
link to `0033-codeql-config-moved-to-github.md`, which resolves, reads as
authoritative, and is strictly worse than the dead link it replaced. Those go to
review instead.

A slug whose words were reordered is still the slug half:
`0335-sycl-adaptivecpp-second-toolchain` is `0407-adaptivecpp-second-sycl-toolchain`
with two words swapped, and 0335 now belongs to an unrelated ADR.

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
from collections import Counter
from pathlib import Path

# ](../../adr/0123-some-slug.md), ](adr/0123-some-slug.md) and -- inside
# docs/adr/ itself -- the sibling form ](0123-some-slug.md) and ](../0123-....md).
# The `adr/` segment is group 3 and is None for a sibling link; see SIBLING_ROOT
# for why a bare link is only an ADR link when the citing file is an ADR.
LINK = re.compile(r"\[([^\]]{0,80})\]\(((?:\.\./)*)(adr/)?([0-9]{4})-([a-z0-9-]+\.md)\)")

# docs/research/ names its digests NNNN-slug.md too, so a bare sibling link there
# is a research link and not an ADR citation. Only files under this directory get
# their bare links read as ADR citations.
SIBLING_ROOT = Path("adr")


# Slug words that carry no topic and so corroborate nothing.
FILLER = frozenset({"and", "the", "for", "to", "of", "in", "on", "is", "not", "adr", "a"})

# A slug word shared by at most this many ADR slugs identifies a small family
# rather than a vocabulary. `iir` (1) and `hvs` (3) pin a specific decision;
# `simd` and `bitexact` are spoken by a dozen ADRs that are not each other.
DISCRIMINATING_DF = 5

# Words this short carry no topic: "v2", "gpu" survives, "of" and "io" do not.
MIN_TOPIC_WORD_LEN = 3

# One shared word can be a coincidence; the fallback wants more than one.
MIN_COINCIDENCE_HITS = 2

# How much of a target ADR is read looking for corroboration. Its title and
# opening paragraphs say what it decided; further down is implementation detail
# that would match almost any slug by coincidence.
CORROBORATION_WINDOW = 900


class AdrCorpus:
    """The ADR files, indexed by number, by slug, and by slug word set."""

    def __init__(self, adr_dir: Path) -> None:
        self.dir = adr_dir
        self.by_number: dict[str, list[str]] = {}
        self.by_slug: dict[str, list[str]] = {}
        self.by_words: dict[frozenset[str], list[str]] = {}
        for path in sorted(adr_dir.glob("[0-9][0-9][0-9][0-9]-*.md")):
            self.by_number.setdefault(path.name[:4], []).append(path.name)
            self.by_slug.setdefault(path.name[5:], []).append(path.name)
            self.by_words.setdefault(slug_words(path.name[5:]), []).append(path.name)
        self.slug_df: Counter[str] = Counter()
        for name in self.by_slug:
            self.slug_df.update(slug_words(name))
        self.known = {name for names in self.by_number.values() for name in names}
        self._opening: dict[str, str] = {}

    def opening(self, name: str) -> str:
        """The target's title and abstract, lowercased, comments stripped."""
        if name not in self._opening:
            text = (self.dir / name).read_text(encoding="utf-8", errors="ignore")
            self._opening[name] = re.sub(r"<!--.*?-->", " ", text, flags=re.S)[
                :CORROBORATION_WINDOW
            ].lower()
        return self._opening[name]

    def corroborates(self, slug: str, target: str) -> bool:
        """Does `target` visibly concern what `slug` says the citation is about?

        A repair driven by the number alone asserts that the number still names
        the decision the slug describes. That assertion is checkable: the target
        should mention at least two of the slug's topic words near its top (or
        its only one, for a single-word slug). When it does not, the number is as likely to have been reallocated to an
        unrelated ADR as to have survived, and the link goes to review instead.
        """
        words = [w for w in slug_words(slug) if w not in FILLER and len(w) >= MIN_TOPIC_WORD_LEN]
        if not words:
            return False
        opening = self.opening(target)

        # Prefer the words that identify a decision over the words a whole
        # family of ADRs shares. `[ADR-0138 -- PSNR-HVS SIMD bit-exactness]`
        # cited `0138-psnr-hvs-simd-bitexact`, and `0138-iqa-convolve-avx2-
        # bitexact-double` does say "simd" and "bitexact" -- it is a sibling in
        # the same family, not the same decision. It says nothing about PSNR-HVS.
        # A word carried by no ADR slug at all (df 0) discriminates nothing and
        # is skipped, or `go-grpc-scoring-service` would be judged on "service".
        discriminating = [w for w in words if 1 <= self.slug_df[w] <= DISCRIMINATING_DF]
        if discriminating:
            return any(word in opening for word in discriminating)

        # No identifying word: fall back to needing more than a single
        # coincidence. `vulkan-image-import-feasibility` shares only "image"
        # with `0125-ms-ssim-decimate-simd`, which is about neither Vulkan nor
        # importing.
        return sum(1 for word in words if word in opening) >= min(MIN_COINCIDENCE_HITS, len(words))


def slug_words(slug: str) -> frozenset[str]:
    """The word set of a slug, ignoring order and the trailing `.md`."""
    return frozenset(slug.removesuffix(".md").split("-"))


def resolve_target(number: str, slug: str, corpus: AdrCorpus) -> tuple[str | None, str]:
    """Pick the file a broken link meant, and say which half identified it.

    Slug first: a slug that names a real ADR under a different number means the
    number is the wrong half, and repairing from the number would repoint the
    link at an unrelated decision. See the module docstring for the measurement
    that settles this ordering.

    The number is used only when it corroborates the slug, because a number that
    no longer names the cited decision resolves, reads as authoritative, and is
    worse than the dead link it replaced.
    """
    self_name = f"{number}-{slug}"
    slug_hits = [name for name in corpus.by_slug.get(slug, []) if name != self_name]
    if len(slug_hits) == 1:
        return slug_hits[0], "slug"

    # A slug whose words were reordered or respelled is still the slug half:
    # `0335-sycl-adaptivecpp-second-toolchain` is
    # `0407-adaptivecpp-second-sycl-toolchain` with two words swapped, and its
    # number now belongs to an unrelated ADR about hardware capability priors.
    word_hits = [n for n in corpus.by_words.get(slug_words(slug), []) if n != self_name]
    if not slug_hits and len(word_hits) == 1:
        return word_hits[0], "slug words"

    number_hits = corpus.by_number.get(number, [])
    if len(number_hits) == 1 and not slug_hits and not word_hits:
        if corpus.corroborates(slug, number_hits[0]):
            return number_hits[0], "number"
        return None, "uncorroborated"

    if slug_hits or number_hits or word_hits:
        return None, "ambiguous"
    return None, "unknown"


def repair_one_file(
    path: Path,
    corpus: AdrCorpus,
    *,
    fix: bool,
    bare_is_adr: bool = False,
) -> tuple[list[str], list[str], int]:
    """Resolve one file's ADR links. Returns (findings, unfixable, repaired).

    `bare_is_adr` admits the sibling form `](0123-slug.md)`, which carries no
    `adr/` segment and is only an ADR citation when the citing file is itself
    under docs/adr/.
    """
    text = path.read_text(encoding="utf-8", errors="ignore")
    findings: list[str] = []
    unfixable: list[str] = []
    repaired = 0
    out: list[str] = []
    cursor = 0

    for match in LINK.finditer(text):
        out.append(text[cursor : match.start()])
        cursor = match.end()
        label, prefix, segment, number, slug = (
            match.group(1),
            match.group(2),
            match.group(3) or "",
            match.group(4),
            match.group(5),
        )
        if not segment and not bare_is_adr:
            # A bare NNNN-slug.md outside docs/adr/ names a sibling in whatever
            # directory it sits in -- docs/research/ uses the same convention.
            out.append(match.group(0))
            continue
        if f"{number}-{slug}" in corpus.known:
            out.append(match.group(0))
            continue

        line = text[: match.start()].count("\n") + 1
        target, how = resolve_target(number, slug, corpus)
        if target is None:
            where = "no ADR carries that number or that slug"
            if how == "ambiguous":
                where = "the number and the slug do not agree on one file"
            elif how == "uncorroborated":
                target_name = corpus.by_number[number][0]
                where = (
                    f"the slug names no ADR, and {target_name} -- which carries that "
                    "number now -- does not mention what the slug is about, so the "
                    "number was probably reallocated"
                )
            unfixable.append(f"{path}:{line}: [{label}]({segment}{number}-{slug}) -- {where}")
            out.append(match.group(0))
            continue

        if fix:
            repaired += 1
            # A slug-resolved link -- exact or reordered -- carries the wrong
            # number in its text too.
            # Both spellings occur: a bare "ADR-0241" and a descriptive
            # "ADR-0241 - vmaf_tiny_v3 ship decision". Only the number is
            # rewritten; whatever the author wrote after it is left alone.
            new_label = label
            if how.startswith("slug") and re.match(rf"ADR-{number}\b", label):
                new_label = re.sub(rf"^ADR-{number}\b", f"ADR-{target[:4]}", label)
            out.append(f"[{new_label}]({prefix}{segment}{target})")
            continue

        findings.append(f"{path}:{line}: {segment}{number}-{slug} -> {segment}{target} (by {how})")
        out.append(match.group(0))

    out.append(text[cursor:])
    if repaired:
        path.write_text("".join(out), encoding="utf-8")
    return findings, unfixable, repaired


def scan(docs: Path, corpus: AdrCorpus, *, fix: bool) -> tuple[list[str], list[str], int]:
    """Return (repairable findings, unfixable findings, repaired count)."""
    findings: list[str] = []
    unfixable: list[str] = []
    repaired = 0

    sibling_root = docs / SIBLING_ROOT
    for path in sorted(docs.rglob("*.md")):
        file_findings, file_unfixable, file_repaired = repair_one_file(
            path,
            corpus,
            fix=fix,
            bare_is_adr=path.is_relative_to(sibling_root),
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

    corpus = AdrCorpus(adr_dir)
    findings, unfixable, repaired = scan(docs, corpus, fix=args.fix)

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
        f"check-adr-links: OK ({len(corpus.by_number)} ADR numbers, "
        f"every ADR link under {docs} resolves)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
