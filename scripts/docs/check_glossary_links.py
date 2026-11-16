#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Hold docs/metrics/ pages to the glossary (issue 2332).

Three gates, all structural — no binary needed:

1. every term the checker knows about must have an entry anchor in
   docs/reference/glossary.md (a term without an entry fails);
2. every docs/metrics/ page that uses a known term must link that term's
   glossary entry at least once (a use without a link fails);
3. every docs/metrics/ page links the glossary at least once, so a reader on
   any metric page is one click from the definitions ("a glossary linked
   from every metric page").

Gate 2 and 3 reuse ``parse_links`` and ``check_target`` from
``scripts/docs/check_doc_links.py``: a link has to resolve to the glossary
file and, for gate 2, to the term's real anchor. A bare ``glossary.md#neg``
substring, a link inside an HTML comment, a too-shallow path or a wrong
anchor does not satisfy the gate.

The known-term list lives in ``TERMS`` below; grow it as the glossary grows.
Wired into ``make docs-fragments-check``.

Exit status: 0 clean, 1 findings, 2 inputs cannot be read.
"""

from __future__ import annotations

import re
import sys
import urllib.parse
from pathlib import Path

_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))

from docs.check_doc_links import (  # noqa: E402
    check_target,
    mask_fenced_code_blocks,
    mask_inline_code_spans,
    parse_links,
)

ROOT = Path(__file__).resolve().parents[2]
GLOSSARY = ROOT / "docs" / "reference" / "glossary.md"
METRICS_DIR = ROOT / "docs" / "metrics"

EXIT_SUCCESS = 0
EXIT_FAILURE = 1
EXIT_UNREADABLE = 2

# term -> glossary anchor. Case-insensitive word matching on metric pages.
# Every `## Title {#anchor}` entry in docs/reference/glossary.md has at least
# one term here; `test_every_glossary_entry_has_a_term` keeps the two in step.
TERMS = {
    "Viewing condition": "viewing-condition",
    "3H": "3h",
    "Phone model": "phone-model",
    "Default model": "default-model",
    "Feature extractor": "feature-extractor",
    "Motion": "motion",
    "Pooling": "pooling",
    "NEG": "neg",
    "CTC": "ctc",
    "HDR": "hdr",
    "SDR": "sdr",
    "PQ": "pqr-hlg",
    "HLG": "pqr-hlg",
    "BT.709": "bt709-bt2020",
    "BT.2020": "bt709-bt2020",
    "Range": "range",
    "Frame alignment": "frame-alignment",
    "Scaling": "scaling",
    "Interlacing": "interlacing",
    "Chroma subsampling": "chroma-subsampling",
    "10-bit": "10-bit",
    "Confidence interval": "confidence-interval",
    "Score transform": "score-transform",
}


def _read(path: Path) -> str:
    with path.open("rb") as handle:
        return handle.read().decode("utf-8")


def _mask_html_comments(text: str) -> str:
    """Replace HTML comments with spaces, preserving newlines."""
    return re.sub(
        r"<!--.*?-->",
        lambda match: re.sub(r"[^\n]", " ", match.group(0)),
        text,
        flags=re.DOTALL,
    )


def _masked(text: str) -> str:
    """Mask code fences, inline code spans and HTML comments before parsing."""
    return mask_inline_code_spans(mask_fenced_code_blocks(_mask_html_comments(text)))


def glossary_anchors(text: str) -> set[str]:
    """Collect the explicit anchors: `## Title {#anchor}`."""
    return set(re.findall(r"^#{1,6}\s+.*\{#([a-z0-9-]+)\}", text, re.MULTILINE))


def term_pattern(term: str) -> re.Pattern[str]:
    """Whole-word, case-insensitive pattern; a hyphen stays part of the word."""
    return re.compile(rf"(?<![A-Za-z0-9-]){re.escape(term)}(?![A-Za-z0-9-])", re.IGNORECASE)


def _has_glossary_link(
    page: Path,
    links: list[tuple[int, str]],
    anchor: str | None,
    anchor_cache: dict[Path, set[str]],
) -> bool:
    """True when a parsed link resolves to the glossary (and optionally the anchor)."""
    glossary = GLOSSARY.resolve()
    for lineno, target in links:
        if target.startswith(("http:", "https:", "mailto:")):
            continue
        file_part, sep, fragment = target.partition("#")
        if not file_part:
            continue
        if (page.parent / file_part).resolve() != glossary:
            continue
        if anchor is not None and (not sep or urllib.parse.unquote(fragment) != anchor):
            continue
        if check_target(page, ROOT, lineno, target, anchor_cache) is None:
            return True
    return False


def _check_page(
    page: Path,
    terms: dict[str, str],
    anchor_cache: dict[Path, set[str]],
) -> list[str]:
    """Return the gate-2 and gate-3 findings for one metric page."""
    text = _read(page)
    masked = _masked(text)
    links = parse_links(masked)
    rel = METRICS_DIR.name + "/" + page.relative_to(METRICS_DIR).as_posix()
    depth = len(page.parent.relative_to(METRICS_DIR).parts)
    glossary_rel = "../" * (depth + 1) + "reference/glossary.md"

    failures: list[str] = []
    for term, anchor in terms.items():
        if not term_pattern(term).search(masked):
            continue
        if not _has_glossary_link(page, links, anchor, anchor_cache):
            failures.append(
                f"{rel}: uses '{term}' but never links the glossary entry ({glossary_rel}#{anchor})"
            )
    if not _has_glossary_link(page, links, None, anchor_cache):
        failures.append(
            f"{rel}: no link to the glossary ({glossary_rel}) — every metric page links it"
        )
    return failures


def _check_pages(
    pages: list[Path],
    terms: dict[str, str],
) -> list[str]:
    """Run gates 2 and 3 over every page, stopping on unreadable input."""
    anchor_cache: dict[Path, set[str]] = {}
    failures: list[str] = []
    for page in pages:
        failures.extend(_check_page(page, terms, anchor_cache))
    return failures


def main(argv: list[str] | None = None) -> int:
    del argv  # no options yet; kept for symmetry with the other docs checks
    try:
        glossary_text = _read(GLOSSARY)
        metric_pages = sorted(METRICS_DIR.rglob("*.md"))
        anchors = glossary_anchors(glossary_text)
        failures: list[str] = []

        # Gate 1: every known term has an entry.
        for term, anchor in TERMS.items():
            if anchor not in anchors:
                failures.append(f"glossary has no entry anchor '#{anchor}' for known term '{term}'")

        # Gate 2 + 3: metric pages link their terms and the glossary itself.
        failures.extend(_check_pages(metric_pages, TERMS))
    except (OSError, UnicodeDecodeError) as exc:
        print(f"UNREADABLE: {exc}")
        return EXIT_UNREADABLE

    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return EXIT_FAILURE
    print(
        f"OK — glossary holds {len(TERMS)} known terms with entries, and all "
        f"{len(metric_pages)} metric pages link their terms and the glossary"
    )
    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
