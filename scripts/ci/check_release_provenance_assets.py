#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that release workflows upload build-provenance assets (*.intoto.jsonl).

OpenSSF Scorecard (v5.6.0) Signed-Releases check evaluates whether releases carry
cryptographic signatures (score 8) and build-provenance attestations (score 10).
A release scores 10 when at least one release asset matches '*.intoto.jsonl'.

This check inspects all GitHub workflows under '.github/workflows/' (or the
directory given via --dir). A workflow is identified as a releaser if any step
uses 'softprops/action-gh-release' or runs 'gh release create' or 'gh release
upload' (ignoring shell comment lines).

Each releaser workflow must:
1. Write a provenance bundle ending in '.intoto.jsonl' in a 'run:' step
   (via redirection '>', 'cp', or 'jq ... >').
2. Include that '.intoto.jsonl' path in the uploaded asset set, either by
   matching a target glob/argument of the 'gh release' command or by being
   listed in the 'files:' input of 'action-gh-release'.

Exempt workflows (such as workflows uploading only license texts or notices
without build outputs) are recorded in EXEMPT_WORKFLOWS with a documented reason.
Any exemption for a workflow that is not an active releaser is reported as a
stale exception finding.

Exit codes:
  0: all releaser workflows upload .intoto.jsonl assets, no stale exemptions
  1: releaser missing provenance asset, or stale exemption present
  2: workflow file cannot be read or parsed
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import shlex
import sys
from pathlib import Path

import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"

EXEMPT_WORKFLOWS: dict[str, str] = {
    "published-rc-licence-companions.yml": (
        "companion licence files and notices only; no build output or binaries released"
    ),
}

FLAGS_WITH_VALUE = {
    "--repo",
    "-R",
    "--target",
    "--title",
    "-t",
    "--notes",
    "-n",
    "--notes-file",
    "-F",
    "--discussion-category",
}

WRITE_INTOTO_PATTERN = re.compile(
    r"""(?:>+|(?:\bcp\b|\btee\b)\s+.*?)\s*["\']?([a-zA-Z0-9_./-]+\.intoto\.jsonl)["\']?"""
)

GH_RELEASE_PATTERN = re.compile(r"\bgh\s+release\s+(create|upload)\b")


def join_continuation_lines(text: str) -> list[str]:
    """Join shell lines ending with backslash into continuous command strings."""
    lines: list[str] = []
    current = ""
    for raw in text.splitlines():
        stripped = raw.strip()
        if not current and stripped.startswith("#"):
            continue
        if stripped.endswith("\\"):
            current += (" " if current else "") + stripped[:-1].strip()
        else:
            current += (" " if current else "") + stripped
            if current:
                lines.append(current)
            current = ""
    if current:
        lines.append(current)
    return lines


def extract_gh_release_targets(cmd: str) -> list[str]:
    """Extract file/glob targets from a 'gh release create' or 'gh release upload' command."""
    try:
        tokens = shlex.split(cmd)
    except ValueError:
        tokens = cmd.split()

    idx = -1
    for i in range(len(tokens) - 2):
        if (
            tokens[i] == "gh"
            and tokens[i + 1] == "release"
            and tokens[i + 2] in ("create", "upload")
        ):
            idx = i + 3
            break
    if idx == -1:
        return []

    targets: list[str] = []
    tag_seen = False
    i = idx
    while i < len(tokens):
        token = tokens[i]
        if token.startswith("-"):
            if "=" in token:
                i += 1
            elif token in FLAGS_WITH_VALUE:
                i += 2
            else:
                i += 1
        else:
            if not tag_seen:
                tag_seen = True
            else:
                targets.append(token)
            i += 1
    return targets


def intoto_matches_pattern(written_path: str, pattern: str) -> bool:
    """Check whether a written .intoto.jsonl path matches an uploaded file or glob pattern."""
    clean_pat = pattern.strip().strip("'\"")
    if not clean_pat:
        return False
    written_name = Path(written_path).name
    pat_name = Path(clean_pat).name
    if clean_pat == written_path or pat_name == written_name:
        return True
    if fnmatch.fnmatch(written_path, clean_pat) or fnmatch.fnmatch(written_name, clean_pat):
        return True
    return False


def _collect_action_gh_release(step: dict[object, object], upload_patterns: list[str]) -> bool:
    uses = step.get("uses")
    if not (isinstance(uses, str) and uses.strip().startswith("softprops/action-gh-release")):
        return False
    with_block = step.get("with")
    if not isinstance(with_block, dict):
        return True
    files_val = with_block.get("files", "")
    if isinstance(files_val, str):
        for line in files_val.splitlines():
            stripped = line.strip()
            if stripped and not stripped.startswith("#"):
                upload_patterns.append(stripped)
    elif isinstance(files_val, list):
        for item in files_val:
            upload_patterns.append(str(item).strip())
    return True


def _collect_run_step(run_text: str, written_intoto: list[str], upload_patterns: list[str]) -> bool:
    is_releaser = False
    for line in run_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        for m in WRITE_INTOTO_PATTERN.findall(line):
            if m not in written_intoto:
                written_intoto.append(m)

    for cmd in join_continuation_lines(run_text):
        if GH_RELEASE_PATTERN.search(cmd):
            is_releaser = True
            upload_patterns.extend(extract_gh_release_targets(cmd))
    return is_releaser


def inspect_workflow(path: Path) -> tuple[bool, list[str], list[str]]:
    """Inspect a parsed workflow.

    Returns:
      (is_releaser, written_intoto_paths, uploaded_patterns)
    """
    text = path.read_text(encoding="utf-8")
    doc = yaml.safe_load(text)
    if not isinstance(doc, dict):
        return False, [], []

    jobs = doc.get("jobs", {})
    if not isinstance(jobs, dict):
        return False, [], []

    is_releaser = False
    written_intoto: list[str] = []
    upload_patterns: list[str] = []

    for job in jobs.values():
        if not isinstance(job, dict):
            continue
        for step in job.get("steps", []):
            if not isinstance(step, dict):
                continue
            if _collect_action_gh_release(step, upload_patterns):
                is_releaser = True
            run_val = step.get("run")
            if isinstance(run_val, str) and _collect_run_step(
                run_val, written_intoto, upload_patterns
            ):
                is_releaser = True

    return is_releaser, written_intoto, upload_patterns


def _evaluate_releaser(
    filename: str, written_intoto: list[str], upload_patterns: list[str]
) -> str | None:
    matched = any(intoto_matches_pattern(w, pat) for w in written_intoto for pat in upload_patterns)
    if matched:
        return None
    if not written_intoto:
        return f"{filename}: no .intoto.jsonl build provenance written"
    if not upload_patterns:
        return f"{filename}: no release assets uploaded"
    return (
        f"{filename}: written provenance ({', '.join(written_intoto)}) "
        f"does not match uploaded assets ({', '.join(upload_patterns)})"
    )


def check_workflows(directory: Path) -> tuple[list[str], list[str], list[str]]:
    """Scan directory and return (passing_releasers, findings, exempt_releasers)."""
    findings: list[str] = []
    passing: list[str] = []
    exempt: list[str] = []
    releasers_found: set[str] = set()

    for path in sorted(directory.iterdir()):
        if path.suffix not in (".yml", ".yaml") or not path.is_file():
            continue
        is_releaser, written_intoto, upload_patterns = inspect_workflow(path)
        if not is_releaser:
            continue

        releasers_found.add(path.name)
        if path.name in EXEMPT_WORKFLOWS:
            exempt.append(f"{path.name} ({EXEMPT_WORKFLOWS[path.name]})")
            continue

        finding = _evaluate_releaser(path.name, written_intoto, upload_patterns)
        if finding is None:
            passing.append(path.name)
        else:
            findings.append(finding)

    for exempt_name in sorted(EXEMPT_WORKFLOWS.keys()):
        exempt_file = directory / exempt_name
        if exempt_file.exists() and exempt_name not in releasers_found:
            findings.append(
                f"{exempt_name}: exempt in check_release_provenance_assets "
                "but not a releaser (stale exemption)"
            )

    return passing, findings, exempt


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dir", type=Path, default=WORKFLOW_DIR, help="workflow directory")
    args = parser.parse_args(argv)

    try:
        passing, findings, exempt = check_workflows(args.dir)
    except (OSError, UnicodeDecodeError, yaml.YAMLError) as exc:
        print(f"check_release_provenance_assets: cannot read {args.dir}: {exc}", file=sys.stderr)
        return 2

    for item in passing:
        print(f"  OK: {item}")
    for item in exempt:
        print(f"  EXEMPT: {item}")

    if findings:
        print(
            f"check_release_provenance_assets: {len(findings)} findings:",
            file=sys.stderr,
        )
        for f in findings:
            print(f"  ERROR: {f}", file=sys.stderr)
        return 1

    print(
        f"check_release_provenance_assets: OK ({len(passing)} releasers upload provenance, "
        f"{len(exempt)} exempt)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
