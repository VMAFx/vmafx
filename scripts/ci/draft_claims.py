#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check numeric tokens in a forge-text draft against evidence files."""

from __future__ import annotations

import re
import sys
from pathlib import Path

TOKEN = re.compile(r"(?<![A-Za-z0-9.])\d[0-9A-Za-z]*(?:\.\d[0-9A-Za-z]*)*")
THOUSANDS = re.compile(r"(?<=\d),(?=\d{3}(?!\d))")
# Accept up to three nested pairs inside the destination; deeper links stay visible.
LINK_TEXT = r"(?:\\[^\n]|[^()\\])*+"
LINK_LEVEL_1 = rf"{LINK_TEXT}(?:\({LINK_TEXT}\){LINK_TEXT})*+"
LINK_LEVEL_2 = rf"{LINK_TEXT}(?:\({LINK_LEVEL_1}\){LINK_TEXT})*+"
LINK_LEVEL_3 = rf"{LINK_TEXT}(?:\({LINK_LEVEL_2}\){LINK_TEXT})*+"
LINK = re.compile(rf"\]\({LINK_LEVEL_3}\)")
MAX_BYTES = 1024 * 1024
MIN_ARGS = 3


def tokens(text: str) -> list[str]:
    """Return distinct numeric tokens in their first-seen order."""
    normalized = THOUSANDS.sub("", LINK.sub("]", text)).replace("_", " ")
    return list(dict.fromkeys(token for token in TOKEN.findall(normalized) if len(token) > 1))


def read_input(path: Path) -> str:
    """Read at most one MiB of UTF-8 text, refusing larger inputs."""
    with path.open("rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError(f"file exceeds {MAX_BYTES} bytes")
    return data.decode("utf-8")


def main(argv: list[str]) -> int:
    if len(argv) < MIN_ARGS:
        print(
            "usage: draft_claims.py <draft.md> <evidence.md> [<evidence.md> ...]", file=sys.stderr
        )
        return 2
    contents: list[str] = []
    for name in argv[1:]:
        try:
            contents.append(read_input(Path(name)))
        except (OSError, UnicodeError, ValueError) as error:
            print(f"{name}: {error}", file=sys.stderr)
            return 2
    draft = tokens(contents[0])
    have = {token for content in contents[1:] for token in tokens(content)}
    missing = [token for token in draft if token not in have]
    for token in missing:
        print(f"missing in evidence: {token}")
    print(f"draft tokens {len(draft)}, missing {len(missing)}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
