#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check the encoder registry rows against their live upstreams (ADR-3388).

Prints one line per row (ok, mismatch, unchecked) and exits 1 when any row
disagrees with its upstream. Set GITHUB_TOKEN to lift the GitHub API rate
limit (`GITHUB_TOKEN=$(gh auth token)`).
"""

from __future__ import annotations

import argparse
import os
import sys
import urllib.request
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from vmafx_api import encoders, upstreams

ROOT = Path(__file__).resolve().parents[2]
TIMEOUT_S = 30


def fetch(url: str) -> bytes:
    if not url.startswith("https://"):
        raise ValueError(f"refusing a non-https upstream URL: {url}")
    request = urllib.request.Request(  # noqa: S310 (https enforced above)
        url, headers={"User-Agent": "vmafx-encoder-registry"}
    )
    token = os.environ.get("GITHUB_TOKEN", "")
    if token and url.startswith("https://api.github.com/"):
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:  # noqa: S310 (https only)
        return response.read()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check encoder registry rows against their upstreams."
    )
    parser.add_argument("--definition", type=Path, default=ROOT / encoders.DEFINITION)
    args = parser.parse_args()
    findings = upstreams.check(encoders.load(args.definition), fetch, date.today())
    for item in findings:
        print(f"{item.verdict:9} {item.id}: {item.detail}")
    counts = {v: sum(f.verdict == v for f in findings) for v in ("ok", "mismatch", "unchecked")}
    print(", ".join(f"{n} {v}" for v, n in counts.items()))
    return 1 if counts["mismatch"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
