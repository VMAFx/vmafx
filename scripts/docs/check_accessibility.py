#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Scan the built documentation site with pa11y-ci and a planted-defect fixture.

The site is checked in the light and dark palettes (the palette follows
``prefers-color-scheme``). Every retained deviation lives in
``scripts/docs/accessibility-allowlist.json`` with a reason and an expiry; an
exception that matches nothing is a failure so the list cannot rot. The fixture
must fail on an image without alt text and a low-contrast pair before a green
site run is trusted (issue 2552).
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from datetime import date
from pathlib import Path
from urllib.parse import unquote, urlparse

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

DEFAULT_SITE = REPO_ROOT / "build-docs" / "site"
DEFAULT_FIXTURE = REPO_ROOT / "scripts" / "docs" / "accessibility-fixture.html"
DEFAULT_ALLOWLIST = REPO_ROOT / "scripts" / "docs" / "accessibility-allowlist.json"
PAGES = (
    "index.html",
    "backends/sycl/overview/index.html",
    "metrics/psnr/index.html",
    "development/netflix-benchmark-baselines/index.html",
    "adr/0001-stash-benchmark-noise-file/index.html",
    "search/index.html",
)
PLANTED_RULES = ("image-alt", "color-contrast")
STANDARD = "WCAG2AA"
TIMEOUT_SECONDS = 120


class AccessibilityError(Exception):
    """The check could not run or the allowlist is malformed."""


def load_allowlist(path: Path) -> list[dict[str, str]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AccessibilityError(f"cannot read {path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise AccessibilityError(f"{path}: schema_version must be 1")
    exceptions = data.get("exceptions")
    if not isinstance(exceptions, list):
        raise AccessibilityError(f"{path}: exceptions must be a list")
    parsed: list[dict[str, str]] = []
    for index, entry in enumerate(exceptions):
        if not isinstance(entry, dict):
            raise AccessibilityError(f"{path}: exceptions[{index}] must be an object")
        for field in ("page", "rule", "selector", "reason", "expires"):
            if not isinstance(entry.get(field), str) or not entry[field]:
                raise AccessibilityError(f"{path}: exceptions[{index}].{field} must be a string")
        try:
            expiry = date.fromisoformat(entry["expires"])
        except ValueError as exc:
            raise AccessibilityError(
                f"{path}: exceptions[{index}].expires must be YYYY-MM-DD"
            ) from exc
        if expiry < date.today():
            raise AccessibilityError(f"{path}: exceptions[{index}] expired on {entry['expires']}")
        parsed.append(
            {field: entry[field] for field in ("page", "rule", "selector", "reason", "expires")}
        )
    return parsed


def page_for(url: str, pages: tuple[str, ...]) -> str | None:
    path = unquote(urlparse(url).path).replace("\\", "/")
    matches = [page for page in pages if path.endswith("/" + page) or path == page]
    return max(matches, key=len) if matches else None


def issue_key(page: str, issue: dict[str, object]) -> tuple[str, str, str]:
    return page, str(issue.get("code", "")), str(issue.get("selector", ""))


def filter_issues(
    results: dict[str, list[dict[str, object]]],
    pages: tuple[str, ...],
    exceptions: list[dict[str, str]],
    used: set[int],
) -> list[str]:
    findings: list[str] = []
    for url, issues in results.items():
        page = page_for(url, pages)
        if page is None:
            findings.append(f"pa11y returned a page outside the fixed set: {url}")
            continue
        for issue in issues:
            key = issue_key(page, issue)
            matched = next(
                (
                    index
                    for index, exception in enumerate(exceptions)
                    if (exception["page"], exception["rule"], exception["selector"]) == key
                ),
                None,
            )
            if matched is None:
                findings.append(
                    f"{page}: {key[1]} at {key[2]}: {issue.get('message', 'no message')}"
                )
            else:
                used.add(matched)
    return findings


def write_config(
    directory: Path,
    urls: tuple[str, ...],
    dark: bool,
    chrome: str | None,
) -> Path:
    args = ["--no-sandbox", "--disable-gpu"]
    if dark:
        args.append("--force-dark-mode")
    launch: dict[str, object] = {"args": args}
    if chrome:
        launch["executablePath"] = chrome
    config = {
        "defaults": {
            "standard": STANDARD,
            "runners": ["axe"],
            "timeout": 30000,
            "chromeLaunchConfig": launch,
        },
        "urls": list(urls),
    }
    path = directory / ("dark.json" if dark else "light.json")
    path.write_text(json.dumps(config), encoding="utf-8")
    return path


def run_pa11y(pa11y_ci: str, node: str | None, config: Path) -> dict[str, object]:
    binary = Path(pa11y_ci)
    if binary.suffix == ".js":
        if node is None:
            raise AccessibilityError("--node is required when --pa11y-ci names a .js file")
        command = [node, str(binary)]
    else:
        command = [pa11y_ci]
    completed = run_command(
        [*command, "--config", str(config), "--json", "--threshold", "999"],
        allowed_executables=(command[0],),
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
        timeout_seconds=TIMEOUT_SECONDS * 4,
    )
    if completed.returncode not in (0, 1, 2):
        raise AccessibilityError(
            f"pa11y-ci exited {completed.returncode}: {completed.stderr.strip()}"
        )
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise AccessibilityError(f"pa11y-ci produced no JSON: {completed.stderr.strip()}") from exc
    if not isinstance(report, dict) or not isinstance(report.get("results"), dict):
        raise AccessibilityError("pa11y-ci JSON has no results object")
    return report


def scan(
    pa11y_ci: str,
    node: str | None,
    chrome: str | None,
    urls: tuple[str, ...],
    pages: tuple[str, ...],
    exceptions: list[dict[str, str]],
    used: set[int],
    directory: Path,
    dark: bool,
) -> tuple[list[str], dict[str, object]]:
    config = write_config(directory, urls, dark, chrome)
    report = run_pa11y(pa11y_ci, node, config)
    results = report["results"]
    assert isinstance(results, dict)
    findings = filter_issues(results, pages, exceptions, used)
    return findings, report


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--site", type=Path, default=DEFAULT_SITE)
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    parser.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST)
    parser.add_argument("--pa11y-ci", default="pa11y-ci", help="pa11y-ci binary or .js entry point")
    parser.add_argument("--node", help="node binary, needed when --pa11y-ci is a .js file")
    parser.add_argument("--chrome", help="Chrome/Edge executable for local pa11y runs")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if not args.site.is_dir():
            raise AccessibilityError(f"built site is missing: {args.site}; run mkdocs build first")
        if not args.fixture.is_file():
            raise AccessibilityError(f"planted-defect fixture is missing: {args.fixture}")
        exceptions = load_allowlist(args.allowlist)
        used: set[int] = set()
        with tempfile.TemporaryDirectory(prefix="docs-a11y-") as tmp:
            directory = Path(tmp)
            fixture_findings, _ = scan(
                args.pa11y_ci,
                args.node,
                args.chrome,
                (str(args.fixture),),
                (args.fixture.name,),
                [],
                used,
                directory,
                dark=True,
            )
            fixture_rules = {
                finding.split(": ")[1].split(" at ")[0] for finding in fixture_findings
            }
            missing = set(PLANTED_RULES) - fixture_rules
            if missing:
                raise AccessibilityError(
                    "the planted-defect fixture did not fail on: " + ", ".join(sorted(missing))
                )
            findings: list[str] = []
            for dark in (False, True):
                site_findings, _ = scan(
                    args.pa11y_ci,
                    args.node,
                    args.chrome,
                    tuple(str(args.site / page) for page in PAGES),
                    PAGES,
                    exceptions,
                    used,
                    directory,
                    dark=dark,
                )
                findings.extend(
                    f"{'dark' if dark else 'light'}: {finding}" for finding in site_findings
                )
        unused = [exception for index, exception in enumerate(exceptions) if index not in used]
        for exception in unused:
            findings.append(
                f"allowlist entry for {exception['page']} {exception['rule']} "
                f"({exception['selector']}) matches no issue"
            )
    except AccessibilityError as exc:
        print(f"docs-accessibility: error: {exc}", file=sys.stderr)
        return 2
    if findings:
        for finding in findings:
            print(f"docs-accessibility: {finding}", file=sys.stderr)
        print(
            f"docs-accessibility: {len(findings)} finding(s); "
            "fix them or add a dated reason to scripts/docs/accessibility-allowlist.json",
            file=sys.stderr,
        )
        return 1
    print(
        f"docs-accessibility: OK ({len(PAGES)} pages light+dark, "
        f"{len(exceptions)} exception(s), planted defect failed as expected)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
