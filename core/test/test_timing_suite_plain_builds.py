#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The `timing` suite exists only in plain optimised builds (Q-325 follow-up).

A wall-clock budget measures the code only when nothing slows it down:
coverage counters, a sanitizer or a build without optimisation make the
33 ms budget of ``test_vmafx_window_live_timing`` measure the
instrumentation instead (the Coverage Gate failed it on master 88fcae5b0).
``core/test/meson.build`` therefore registers the `timing` tests only when
``vmafx_timing_build`` holds, and compiles the harness with the budget off
otherwise.

Reads this build's Meson introspection and holds:

- an instrumented or unoptimised build (``b_coverage``, ``b_sanitize``,
  ``optimization`` 0 or g) registers no test in the `timing` suite;
- a plain optimised Linux build registers ``test_vmafx_window_live_timing``
  in the `timing` suite;
- every `timing` test is ``is_parallel: false`` (it runs alone).

Planted cases (an instrumented build that lists a `timing` test, a plain
Linux build without one, a parallel `timing` test) are refused by the same
check before the build is read.

Usage: test_timing_suite_plain_builds.py <build dir> <host system>.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

TIMING_TEST = "test_vmafx_window_live_timing"
UNOPTIMISED = ("0", "g")
ARGC = 3  # the script, the build directory, the host system


def build_is_plain(options: dict[str, object]) -> bool:
    """A build without coverage counters or a sanitizer, with optimisation."""
    sanitize = options.get("b_sanitize")
    # Meson >= 1.8 introspects b_sanitize as an array, older ones as a string.
    sanitizers = sanitize if isinstance(sanitize, list) else [sanitize]
    no_sanitizer = all(s in ("", "none", None) for s in sanitizers)
    return (
        options.get("b_coverage") is False
        and no_sanitizer
        and str(options.get("optimization")) not in UNOPTIMISED
    )


def timing_tests(tests: list[dict]) -> list[dict]:
    return [t for t in tests if any(s.split(":")[-1] == "timing" for s in t["suite"])]


def errors(options: dict[str, object], tests: list[dict], host: str) -> list[str]:
    found = timing_tests(tests)
    names = sorted(t["name"] for t in found)
    out = [f"{t['name']} runs in parallel" for t in found if t["is_parallel"]]
    if not build_is_plain(options) and found:
        out.append(f"instrumented or unoptimised build registers timing tests: {names}")
    if build_is_plain(options) and host == "linux" and TIMING_TEST not in names:
        out.append(f"plain optimised Linux build lacks {TIMING_TEST}: {names}")
    return out


def planted_cases_refused() -> list[str]:
    plain = {"b_coverage": False, "b_sanitize": [], "optimization": "3"}
    timed = {"name": TIMING_TEST, "suite": ["libvmaf:timing"], "is_parallel": False}
    cases = {
        "coverage build with a timing test": (dict(plain, b_coverage=True), [timed]),
        "sanitizer build with a timing test": (dict(plain, b_sanitize=["thread"]), [timed]),
        "sanitizer string with a timing test": (dict(plain, b_sanitize="address"), [timed]),
        "debug build with a timing test": (dict(plain, optimization="0"), [timed]),
        "plain Linux build without the timing test": (plain, []),
        "parallel timing test": (plain, [dict(timed, is_parallel=True)]),
    }
    missed = [name for name, (opts, tests) in cases.items() if not errors(opts, tests, "linux")]
    if errors(plain, [timed], "linux") or errors(dict(plain, b_coverage=True), [], "linux"):
        missed.append("a correct build is refused")
    return missed


def main(argv: list[str]) -> int:
    if len(argv) != ARGC:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    missed = planted_cases_refused()
    if missed:
        print(f"planted cases not refused: {missed}", file=sys.stderr)
        return 1
    info = Path(argv[1]) / "meson-info"
    options = {
        o["name"]: o["value"]
        for o in json.loads((info / "intro-buildoptions.json").read_text(encoding="utf-8"))
    }
    tests = json.loads((info / "intro-tests.json").read_text(encoding="utf-8"))
    found = errors(options, tests, argv[2])
    for line in found:
        print(line, file=sys.stderr)
    kind = "plain optimised" if build_is_plain(options) else "instrumented or unoptimised"
    print(f"{kind} build, timing tests: {sorted(t['name'] for t in timing_tests(tests))}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
