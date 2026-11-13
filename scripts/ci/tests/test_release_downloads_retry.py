#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Required CI jobs fetch GitHub release assets through one retry policy (#3349).

A workflow step or CI script that names a ``github.com/.../releases/download/...``
asset and runs ``curl``/``wget`` itself gives up on curl's short default retry
(Trivy and Gitleaks lost four tries in seven seconds on 2026-11-08). Such a
fetch must call ``scripts/ci/download-release.sh``, which runs
``compat/python-vmaf/tools/retrying_download.py`` (seven attempts over 275 s).

The scan reads tracked files only and never executes them. A direct
``releases/download`` URL is not enough by itself: the tester-bundle workflows
pass python-build-standalone URLs to their own builders, and a URL in an env
block is not a fetch.
"""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

STEP = re.compile(r"(?m)^      - (?:name|uses|run):")
FETCH = re.compile(r"(?<![\w.-])(curl(?:\.exe)?|wget)\s+(?=[\"']?(?:-|https?://|\$|%))")
RELEASE = "releases/download"
WRAPPER = "scripts/ci/download-release.sh"

# Exact path + reason. The wrapper needs Python; this script also runs in a
# macOS/MSYS2 builder stage and has its own bounded download with a forge-API
# fallback, so it keeps its portable path until that fallback is ported too.
ALLOWLIST = {
    "scripts/ci/ffmpeg-shared-series.sh": (
        "portable macOS/MSYS2 builder: its own bounded curl/wget download plus "
        "gh release fallback; no Python dependency in those stages"
    ),
}


def tracked(pathspec: str) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", pathspec],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def blocks(text: str) -> list[tuple[int, str]]:
    """Workflow steps; a shell script is one block."""
    starts = [match.start() for match in STEP.finditer(text)]
    if not starts:
        return [(1, text)]
    starts.append(len(text))
    return [
        (text.count("\n", 0, start) + 1, text[start : starts[index + 1]])
        for index, start in enumerate(starts[:-1])
    ]


def findings(path: str, text: str) -> list[tuple[int, str]]:
    """(line, reason) for direct curl/wget fetches of a release asset."""
    out: list[tuple[int, str]] = []
    for offset, block in blocks(text):
        if RELEASE not in block or not FETCH.search(block):
            continue
        line = offset + block[: block.index(RELEASE)].count("\n")
        out.append((line, "fetch a release asset through " + WRAPPER))
    return out


def audit() -> list[tuple[str, int, str]]:
    found: list[tuple[str, int, str]] = []
    paths = tracked(".github/workflows/*.yml") + tracked("scripts/ci/*.sh")
    for path in paths:
        if path.startswith("scripts/ci/tests/"):
            continue
        if path in ALLOWLIST:
            continue
        text = (ROOT / path).read_text(encoding="utf-8")
        found.extend((path, line, reason) for line, reason in findings(path, text))
    return found


class ReleaseDownloadRetryTest(unittest.TestCase):
    def test_every_release_fetch_uses_the_wrapper(self) -> None:
        self.assertEqual([], audit())

    def test_planted_direct_curl_is_refused(self) -> None:
        step = (
            "      - name: Install tool\n"
            "        run: |\n"
            "          curl -fsSL -o /tmp/tool \\\n"
            "            https://github.com/owner/repo/releases/download/v1/tool\n"
        )
        self.assertEqual([(4, "fetch a release asset through " + WRAPPER)], findings("fixture.yml", step))

    def test_wrapper_call_with_a_url_variable_passes(self) -> None:
        step = (
            "      - name: Install tool\n"
            "        env:\n"
            "          TOOL_URL: https://github.com/owner/repo/releases/download/v1/tool\n"
            "        run: |\n"
            "          bash scripts/ci/download-release.sh \"${TOOL_URL}\" /tmp/tool\n"
        )
        self.assertEqual([], findings("fixture.yml", step))

    def test_allowlist_names_a_reason(self) -> None:
        for path, reason in ALLOWLIST.items():
            self.assertTrue(path.endswith(".sh"))
            self.assertTrue(reason)


if __name__ == "__main__":
    sys.exit(unittest.main())
