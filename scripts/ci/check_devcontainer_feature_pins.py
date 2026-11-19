#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every Dev Container feature reference is an exact release or a digest.

A major-only feature tag (``common-utils:2``) moves to a new release without a
review. The devcontainer manager tracks exact tags, so the file can stay on a
release and still receive update pull requests. A digest reference is also
immutable and accepted.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONFIG = REPO / ".devcontainer/devcontainer.json"
EXACT_RELEASE = re.compile(r"^\d+\.\d+\.\d+$")
DIGEST = re.compile(r"^.*@sha256:[0-9a-f]{64}$")


def findings(config: object) -> list[str]:
    if not isinstance(config, dict):
        return ["devcontainer.json is not an object"]
    features = config.get("features")
    if not isinstance(features, dict):
        return ["features is missing or not an object"]
    if not features:
        return ["features is empty"]
    problems: list[str] = []
    for reference in features:
        if not isinstance(reference, str):
            problems.append(f"feature reference is not a string: {reference!r}")
            continue
        if "@sha256:" in reference:
            if not DIGEST.fullmatch(reference):
                problems.append(f"{reference} is not a sha256 digest reference")
            continue
        if ":" not in reference:
            problems.append(f"{reference} has no version or digest")
            continue
        version = reference.rsplit(":", 1)[1]
        if not EXACT_RELEASE.fullmatch(version):
            problems.append(f"{reference} is not an exact release (want x.y.z)")
    return problems


def main(argv: list[str]) -> int:
    path = Path(argv[1]) if len(argv) > 1 else CONFIG
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"check-devcontainer-feature-pins: {path}: {error}", file=sys.stderr)
        return 2
    problems = findings(config)
    for problem in problems:
        print(f"::error title=devcontainer feature pin::{problem}", file=sys.stderr)
    if problems:
        return 1
    features = config["features"]
    print(f"check-devcontainer-feature-pins: OK ({len(features)} exact feature pins)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
