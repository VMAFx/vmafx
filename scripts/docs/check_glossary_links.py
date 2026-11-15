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

The known-term list lives in ``TERMS`` below; grow it as the glossary grows.
Wired into ``make docs-fragments-check``.

Exit status: 0 clean, 1 findings, 2 inputs cannot be read.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GLOSSARY = ROOT / "docs" / "reference" / "glossary.md"
METRICS_DIR = ROOT / "docs" / "metrics"

EXIT_SUCCESS = 0
EXIT_FAILURE = 1
EXIT_UNREADABLE = 2

# term -> glossary anchor. Case-insensitive word matching on metric pages.
TERMS = {
    "NEG": "neg",
    "CTC": "ctc",
    "HDR": "hdr",
    "SDR": "sdr",
    "pooling": "pooling",
    "3H": "3h",
}


def _read(path: Path) -> str:
    with path.open("rb") as handle:
        return handle.read().decode("utf-8")


def glossary_anchors(text: str) -> set[str]:
    """Collect the explicit anchors: `## Title {#anchor}`."""
    return set(re.findall(r"^#{1,6}\s+.*\{#([a-z0-9-]+)\}", text, re.MULTILINE))


def term_pattern(term: str) -> re.Pattern[str]:
    """Whole-word, case-insensitive pattern; a hyphen counts as a boundary."""
    return re.compile(rf"(?<![A-Za-z0-9-]){re.escape(term)}(?![A-Za-z0-9-])", re.IGNORECASE)


def main(argv: list[str] | None = None) -> int:
    del argv  # no options yet; kept for symmetry with the other docs checks
    try:
        glossary_text = _read(GLOSSARY)
        metric_pages = sorted(METRICS_DIR.rglob("*.md"))
    except OSError as exc:
        print(f"UNREADABLE: {exc}")
        return EXIT_UNREADABLE

    failures: list[str] = []
    anchors = glossary_anchors(glossary_text)

    # Gate 1: every known term has an entry.
    for term, anchor in TERMS.items():
        if anchor not in anchors:
            failures.append(f"glossary has no entry anchor '#{anchor}' for known term '{term}'")

    # Gate 2 + 3: metric pages link their terms and the glossary itself.
    for page in metric_pages:
        rel = METRICS_DIR.name + "/" + page.relative_to(METRICS_DIR).as_posix()
        text = _read(page)
        depth = len(page.parent.relative_to(METRICS_DIR).parts)
        glossary_rel = "../" * (depth + 1) + "reference/glossary.md"
        for term, anchor in TERMS.items():
            if not term_pattern(term).search(text):
                continue
            link = f"glossary.md#{anchor}"
            if link not in text:
                failures.append(
                    f"{rel}: uses '{term}' but never links the glossary entry "
                    f"({glossary_rel}#{anchor})"
                )
        if "glossary.md" not in text:
            failures.append(
                f"{rel}: no link to the glossary ({glossary_rel}) — every metric " "page links it"
            )

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
