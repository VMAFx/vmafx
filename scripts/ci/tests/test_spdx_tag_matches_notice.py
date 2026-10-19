# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""An SPDX line must name the licences whose text its file carries (ADR-1250).

A tag is metadata about notices that are already in the file. It can lag behind
them in two ways, and both were on master after the SPDX backfill:

* the file carries a licence text the tag does not name (``ciede.c`` had the
  MIT permission notice and a ``BSD-2-Clause-Patent`` tag; ``psnr_hvs.c`` had
  the two-condition BSD text and a ``BSD-3-Clause`` tag);
* the tag names the fork's or Netflix's licence in a file whose only notice is
  someone else's (``vidinput.c``: Daala's two-condition BSD text under a
  ``BSD-2-Clause-Patent`` tag).

The scan reads the head of every tracked text file that has an SPDX line. It
does not judge provenance (``scripts/dev/relicense_fork_files.py`` does); it
only compares the tag with the text next to it.
"""

# The patterns and fixtures below spell SPDX lines as data.
# REUSE-IgnoreStart

from __future__ import annotations

import re
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Files that quote licence texts as data (the relicensing tool and its tests)
# or are licence texts themselves. The Pelorus sync script left this list with
# ADR-2817: since Pelorus v0.3.0 it writes an EUPL-1.2 tag, not a grant.
NOT_SCANNED_PREFIXES = ("LICENSES/", "subprojects/", "docs/", "changelog.d/", ".workingdir")
QUOTES_LICENCE_TEXT = frozenset(
    {
        "scripts/ci/tests/test_spdx_tag_matches_notice.py",
        "scripts/dev/relicense_fork_files.py",
        "scripts/dev/tests/test_relicense_fork_files.py",
    }
)
HEAD_BYTES = 20_000
TAG_LINE = re.compile(r"SPDX-License-Identifier:[ \t]*([^\n]+)")
BSD_TEXT = re.compile(r"Redistribution and use in source and binary forms")
NON_ENDORSEMENT = re.compile(r"[Nn]either (?:the )?names?\b|endorse or promote")
MIT_TEXT = "Permission is hereby granted, free of charge"
PATENT_GRANT = re.compile(r"BSD\+Patent License|opensource\.org/licenses/BSDplusPatent")
OWN_HOLDER = re.compile(r"Copyright[^\n]*(?:Netflix|Lusoris)")
OWN_LICENCES = frozenset({"BSD-2-Clause-Patent", "EUPL-1.2"})


def licences_shown(text: str) -> set[str]:
    """The licences whose text or grant appears in *text*."""
    shown: set[str] = set()
    for match in BSD_TEXT.finditer(text):
        block = text[match.start() : match.start() + 2500]
        shown.add("BSD-3-Clause" if NON_ENDORSEMENT.search(block) else "BSD-2-Clause")
    if MIT_TEXT in text:
        shown.add("MIT")
    if PATENT_GRANT.search(text):
        shown.add("BSD-2-Clause-Patent")
    return shown


def tag_ids(text: str) -> set[str] | None:
    """The identifiers of the first SPDX line, or None when the file has none."""
    match = TAG_LINE.search(text[:6000])
    if match is None:
        return None
    expr = match.group(1).strip()
    expr = re.sub(r"(\*/|-->|\"\"\"|\"|')\s*$", "", expr).strip().strip("()")
    return {part.strip("() ") for part in re.split(r"\s+(?:AND|OR|WITH)\s+", expr) if part.strip()}


def problems_in(text: str) -> list[str]:
    """What is wrong between the SPDX line of *text* and the notices in it."""
    ids = tag_ids(text)
    shown = licences_shown(text)
    if ids is None or not shown:
        return []
    found = []
    missing = sorted(shown - ids)
    if missing:
        found.append(f"carries the text of {', '.join(missing)}, which the tag does not name")
    claims_own = sorted(ids & OWN_LICENCES)
    has_own_notice = bool(OWN_HOLDER.search(text[:6000])) or "BSD-2-Clause-Patent" in shown
    if claims_own and not has_own_notice:
        found.append(
            f"tag names {', '.join(claims_own)}, but the only notices in the file are someone else's"
        )
    return found


def tracked_files() -> list[str]:
    git = shutil.which("git")
    if git is None:
        raise unittest.SkipTest("git is not installed")
    out = subprocess.run(  # noqa: S603 -- fixed argv
        [git, "-C", str(ROOT), "ls-files", "-z"], capture_output=True, check=True
    ).stdout
    return [name for name in out.decode("utf-8").split("\0") if name]


class TagMatchesNotice(unittest.TestCase):
    def test_every_tagged_file_names_the_licences_it_carries(self) -> None:
        failures = []
        scanned = 0
        for name in tracked_files():
            if name.startswith(NOT_SCANNED_PREFIXES) or name in QUOTES_LICENCE_TEXT:
                continue
            path = ROOT / name
            try:
                text = path.read_bytes()[:HEAD_BYTES].decode("utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            scanned += 1
            failures += [f"{name}: {problem}" for problem in problems_in(text)]
        self.assertGreater(scanned, 1000, "the scan reached too few files to mean anything")
        self.assertEqual(failures, [], "\n" + "\n".join(failures))


class Detector(unittest.TestCase):
    TWO = (
        "Copyright (c) 2002-2013 Daala project contributors.  All rights reserved.\n"
        "SPDX-License-Identifier: {tag}\n\n"
        "Redistribution and use in source and binary forms, with or without\n"
        "modification, are permitted provided that the following conditions are met:\n"
        "- Redistributions of source code must retain the above copyright notice.\n"
        "- Redistributions in binary form must reproduce the above copyright notice.\n"
    )
    THREE = TWO + "- Neither the name of the project nor the names of its contributors may\n"

    def test_matching_tags_pass(self) -> None:
        self.assertEqual(problems_in(self.TWO.format(tag="BSD-2-Clause")), [])
        self.assertEqual(problems_in(self.THREE.format(tag="BSD-3-Clause")), [])

    def test_wrong_bsd_variant_is_reported(self) -> None:
        found = problems_in(self.TWO.format(tag="BSD-3-Clause"))
        self.assertEqual(len(found), 1)
        self.assertIn("BSD-2-Clause", found[0])
        self.assertTrue(problems_in(self.THREE.format(tag="BSD-2-Clause")))

    def test_own_licence_over_a_foreign_notice_is_reported(self) -> None:
        found = problems_in(self.TWO.format(tag="BSD-2-Clause-Patent"))
        self.assertEqual(len(found), 2)
        both = problems_in(self.TWO.format(tag="BSD-2-Clause-Patent AND BSD-2-Clause"))
        self.assertEqual(len(both), 1)
        self.assertIn("someone else's", both[0])

    def test_own_notice_supports_the_own_licence(self) -> None:
        text = " *  Copyright 2016-2026 Netflix, Inc.\n" + self.TWO.format(
            tag="BSD-2-Clause-Patent AND BSD-2-Clause"
        )
        self.assertEqual(problems_in(text), [])

    def test_mit_text_needs_mit_in_the_tag(self) -> None:
        text = (
            " *  Copyright 2016-2026 Netflix, Inc.\n"
            " *  SPDX-License-Identifier: {tag}\n"
            "Copyright (c) 2019 Joshua Holmer\n"
            "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
        )
        self.assertTrue(problems_in(text.format(tag="BSD-2-Clause-Patent")))
        self.assertEqual(problems_in(text.format(tag="BSD-2-Clause-Patent AND MIT")), [])

    def test_a_tag_without_licence_text_is_not_judged(self) -> None:
        # A SIMD kernel that carries Xiph.Org's notice line but not the licence
        # text: the provenance tool decides its tag, not this scan.
        text = (
            " * Copyright 2001-2012 Xiph.Org and contributors.\n * Copyright 2026 Lusoris\n"
            " * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause\n"
        )
        self.assertEqual(problems_in(text), [])
        self.assertEqual(problems_in("int main(void);\n"), [])


if __name__ == "__main__":
    unittest.main()

# REUSE-IgnoreEnd
