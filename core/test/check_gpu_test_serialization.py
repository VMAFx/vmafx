#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Require every configured shared-device GPU test to run exclusively.

Meson's documented ``intro-tests.json`` schema exposes both suite membership
and the effective ``is_parallel`` flag.  The ``gpu`` suite is this project's
public classification for tests that may use an accelerator, so every member
must opt out of parallel execution to avoid sharing one physical device.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("introspection", type=Path)
    parser.add_argument("suite", help="Meson suite to enforce (qualified or bare)")
    args = parser.parse_args()

    try:
        tests = json.loads(args.introspection.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        print(
            f"ERROR: cannot read Meson test introspection {args.introspection}: {error}",
            file=sys.stderr,
        )
        return 2
    if not isinstance(tests, list) or any(not isinstance(test, dict) for test in tests):
        print("ERROR: Meson test introspection must be a list of objects", file=sys.stderr)
        return 2

    gpu_tests = [
        test
        for test in tests
        if any(
            suite == args.suite or suite.endswith(f":{args.suite}")
            for suite in test.get("suite", [])
        )
    ]
    if not gpu_tests:
        print("SKIP: no GPU tests are configured")
        return 77

    parallel = [test["name"] for test in gpu_tests if test.get("is_parallel", True)]
    if parallel:
        print("GPU tests must set is_parallel: false; parallel registrations:")
        print("\n".join(f"  {name}" for name in parallel))
        return 1

    print(f"{len(gpu_tests)} GPU tests are registered for exclusive execution")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
