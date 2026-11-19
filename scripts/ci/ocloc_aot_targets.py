#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Select the default AOT targets accepted by the installed ocloc."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "core" / "src" / "sycl"))
from check_aot_image import parse_ocloc_ip_version  # noqa: E402

from scripts.lib.safe_subprocess import (  # noqa: E402
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
)
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

TIMEOUT = 60
OPTIONS = ROOT / "core" / "meson_options.txt"


def default_targets(path: Path = OPTIONS) -> list[str]:
    text = path.read_text(encoding="utf-8")
    match = re.search(
        r"option\('sycl_icpx_aot_targets',\s*type:\s*'string',\s*value:\s*'([^']*)'",
        text,
    )
    if match is None:
        raise ValueError("cannot find sycl_icpx_aot_targets default")
    targets = match.group(1).split(",")
    if not all(re.fullmatch(r"[a-z0-9-]+", target) for target in targets):
        raise ValueError("invalid or empty AOT target list")
    if len(set(targets)) != len(targets):
        raise ValueError("duplicate AOT targets")
    return targets


def probe(ocloc: str, target: str, timeout: float = TIMEOUT) -> tuple[bool, str, bool]:
    try:
        result = run_command(
            [ocloc, "ids", target],
            allowed_executables=[ocloc],
            capture_output=True,
            text=True,
            timeout_seconds=timeout,
        )
    except (
        OSError,
        CommandValidationError,
        CommandTimedOut,
        CommandOutputLimitExceeded,
    ) as error:
        return False, (str(error).splitlines() or ["probe error"])[0], True
    output = result.stdout or result.stderr
    first = output.splitlines()[0] if output.splitlines() else "(no output)"
    return (
        result.returncode == 0 and parse_ocloc_ip_version(result.stdout) is not None,
        first,
        False,
    )


def select(ocloc: str, targets: list[str], expected: set[str], output: Path | None) -> int:
    accepted, refused = [], []
    failed = False
    for target in targets:
        ok, first, error = probe(ocloc, target)
        failed |= error
        (accepted if ok else refused).append(target)
        print(f"{target}: {'accepted' if ok else 'refused'}: {first}")
    if failed or set(refused) != expected or not accepted:
        print(f"AOT targets refused: measured={sorted(refused)} expected={sorted(expected)}")
        if not accepted:
            print("No accepted AOT targets")
        return 1
    if output is not None:
        with output.open("a", encoding="utf-8") as stream:
            stream.write(f"targets={','.join(accepted)}\n")
    print(f"::notice::Dropped AOT targets: {', '.join(refused) if refused else '(none)'}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ocloc", help="ocloc executable path or name")
    parser.add_argument("--expect-refused", required=True, help="expected refused targets, CSV")
    parser.add_argument("--github-output", type=Path, help="GitHub output file")
    args = parser.parse_args()
    expected = set(filter(None, args.expect_refused.split(",")))
    try:
        return select(args.ocloc, default_targets(), expected, args.github_output)
    except (OSError, ValueError) as error:
        print(f"AOT target probe failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
