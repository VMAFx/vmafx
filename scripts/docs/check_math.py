#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Compile every formula of the built documentation site with KaTeX.

``pymdownx.arithmatex`` (generic mode) turns each ``$...$`` and ``$$...$$`` of
the Markdown sources into ``<span|div class="arithmatex">`` carrying
``\\(...\\)`` or ``\\[...\\]``. This check reads those elements from the built
HTML, compiles each one with the vendored KaTeX in strict mode with
``throwOnError`` (``scripts/docs/check_math_worker.js``) and fails on any
formula KaTeX rejects, so invalid TeX fails the strict documentation build
instead of showing as red text on the site (ADR-2705).

    mkdocs build --strict && python3 scripts/docs/check_math.py --site build-docs/site

Needs Node.js (any current release) on PATH; without it the check exits 3 and
says so, it never reports a pass it did not run. Runs on Linux, macOS and
Windows (pathlib and subprocess only).

Exit status: 0 when every formula compiles, 1 when one does not, 2 on a usage
error, 3 when Node.js is missing.
"""

from __future__ import annotations

import argparse
import html
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
WORKER = Path(__file__).resolve().with_name("check_math_worker.js")
ELEMENT = re.compile(
    r'<(?P<tag>span|div) class=(?:"arithmatex"|arithmatex)>(?P<body>.*?)</(?P=tag)>', re.DOTALL
)
TIMEOUT_SECONDS = 600


class Formula:
    """One formula found in a built page."""

    def __init__(self, page: Path, tex: str, display: bool) -> None:
        self.page = page
        self.tex = tex
        self.display = display


def parse_formulas(page: Path, text: str) -> list[Formula]:
    found = []
    for match in ELEMENT.finditer(text):
        body = html.unescape(match.group("body")).strip()
        if body.startswith("\\(") and body.endswith("\\)"):
            found.append(Formula(page, body[2:-2], False))
        elif body.startswith("\\[") and body.endswith("\\]"):
            found.append(Formula(page, body[2:-2], True))
        else:
            found.append(Formula(page, body, match.group("tag") == "div"))
    return found


def collect(site: Path) -> list[Formula]:
    formulas: list[Formula] = []
    for page in sorted(site.rglob("*.html")):
        formulas.extend(parse_formulas(page, page.read_text(encoding="utf-8", errors="replace")))
    return formulas


def compile_all(formulas: list[Formula], node: str) -> list[dict[str, Any]]:
    payload = json.dumps([{"tex": f.tex, "display": f.display} for f in formulas])
    done = subprocess.run(  # noqa: S603 - fixed argv, no shell
        [node, str(WORKER)],
        input=payload,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=TIMEOUT_SECONDS,
        check=False,
    )
    if done.returncode != 0:
        raise RuntimeError(f"node exited {done.returncode}: {done.stderr.strip()}")
    failures: list[dict[str, Any]] = json.loads(done.stdout)
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--site", type=Path, default=ROOT / "build-docs" / "site")
    args = parser.parse_args(argv)
    if not args.site.is_dir():
        print(
            f"check_math: {args.site} is not a built site (run mkdocs build first)", file=sys.stderr
        )
        return 2
    node = shutil.which("node")
    if node is None:
        print("check_math: node not found on PATH; the formulas were NOT checked", file=sys.stderr)
        return 3
    formulas = collect(args.site)
    try:
        failures = compile_all(formulas, node) if formulas else []
    except (RuntimeError, json.JSONDecodeError, subprocess.TimeoutExpired) as err:
        print(f"check_math: {err}", file=sys.stderr)
        return 2
    for failure in failures:
        formula = formulas[int(failure["index"])]
        rel = (
            formula.page.relative_to(args.site)
            if formula.page.is_relative_to(args.site)
            else formula.page
        )
        print(f"check_math: {rel}: {failure['message']}", file=sys.stderr)
        print(f"check_math:   formula: {formula.tex[:200]!r}", file=sys.stderr)
    if failures:
        return 1
    print(f"check_math: {len(formulas)} formulas compile with KaTeX in strict mode")
    return 0


if __name__ == "__main__":
    sys.exit(main())
