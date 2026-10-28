#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check documentation links and heading anchors in Markdown files.

Parses inline links and reference definitions, validates that relative file
and directory targets exist inside the repository root, and verifies that
target anchors match heading slugs, explicit attribute IDs, or HTML IDs.
"""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
import urllib.parse
from pathlib import Path

EXIT_SUCCESS = 0
EXIT_FINDINGS = 1
EXIT_UNREADABLE = 2


def slugify(text: str) -> str:
    """Compute MkDocs default TOC slug for heading text."""
    normalized = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")
    cleaned = re.sub(r"[^\w\s-]", "", normalized).strip().lower()
    return re.sub(r"[-\s]+", "-", cleaned)


def mask_fenced_code_blocks(text: str) -> str:
    """Replace content inside fenced code blocks with spaces, preserving newlines."""
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    in_fence = False
    fence_char = ""
    fence_len = 0
    fence_re = re.compile(r"^[ ]{0,3}(`{3,}|~{3,})")

    for line in lines:
        if not in_fence:
            match = fence_re.match(line)
            if match:
                in_fence = True
                fence_char = match.group(1)[0]
                fence_len = len(match.group(1))
                out.append(re.sub(r"[^\n]", " ", line))
            else:
                out.append(line)
        else:
            stripped = line.strip()
            out.append(re.sub(r"[^\n]", " ", line))
            if len(stripped) >= fence_len and set(stripped) == {fence_char}:
                in_fence = False
    return "".join(out)


def mask_inline_code_spans(text: str) -> str:
    """Replace content inside inline code spans with spaces, preserving newlines."""
    pattern = re.compile(r"(?<!`)(`+)(?!`)([\s\S]*?)(?<!`)\1(?!`)")
    return pattern.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def extract_anchors(path: Path) -> set[str]:
    """Extract heading slugs, explicit IDs, and HTML IDs from a Markdown file."""
    text = path.read_text(encoding="utf-8")
    masked = mask_fenced_code_blocks(text)
    anchors: set[str] = set()

    for match in re.finditer(r"\{#([A-Za-z0-9_\-]+)\}", masked):
        anchors.add(match.group(1))
    for match in re.finditer(
        r"""<[a-zA-Z0-9_\-]+(?:\s+[^>]*)?\b(?:id|name)=["']([A-Za-z0-9_\-]+)["']""",
        masked,
    ):
        anchors.add(match.group(1))

    slug_counts: dict[str, int] = {}
    for line in masked.splitlines():
        match = re.match(r"^[ ]{0,3}#{1,6}\s+(.*?)(?:\s+#+)?$", line)
        if not match:
            continue
        heading = re.sub(r"\{#[^}]+\}", "", match.group(1)).strip()
        slug = slugify(heading)
        if not slug:
            continue
        count = slug_counts.get(slug, 0)
        slug_counts[slug] = count + 1
        unique_slug = slug if count == 0 else f"{slug}-{count}"
        anchors.add(unique_slug)
    return anchors


def parse_links(text: str) -> list[tuple[int, str]]:
    """Parse reference definitions and inline links from masked Markdown text."""
    links: list[tuple[int, str]] = []
    ref_re = re.compile(r"^[ ]{0,3}\[([^\]]+)\]:\s*(?:<([^>]+)>|([^\s>]+))(?:\s+.*)?$")
    for lineno, line in enumerate(text.splitlines(), 1):
        match = ref_re.match(line)
        if match:
            target = match.group(2) or match.group(3)
            links.append((lineno, target))

    link_re = re.compile(
        r"\[((?:[^\n\]]|\n(?!\s*\n))*)\]\(\s*(?:<([^>]+)>|([^\s>)]+))(?:\s+[\"'][^\"']*[\"'])?\s*\)"
    )
    for match in link_re.finditer(text):
        lineno = text[: match.start()].count("\n") + 1
        target = match.group(2) or match.group(3)
        links.append((lineno, target))
    return links


def check_target(
    path: Path,
    root: Path,
    lineno: int,
    target: str,
    anchor_cache: dict[Path, set[str]],
) -> str | None:
    """Validate a link target and return a finding string or None if valid."""
    if not target or target.startswith(("http:", "https:", "mailto:")):
        return None
    try:
        display_path = path.relative_to(root)
    except ValueError:
        display_path = path

    if target.startswith("#"):
        frag = urllib.parse.unquote(target[1:])
        anchors = anchor_cache.setdefault(path, extract_anchors(path))
        if frag not in anchors:
            return f"{display_path}:{lineno}: missing anchor {target}"
        return None

    file_part, sep, frag = target.partition("#")
    frag = urllib.parse.unquote(frag)
    target_path = (path.parent / file_part).resolve()
    if not target_path.is_relative_to(root) or not (target_path.is_file() or target_path.is_dir()):
        return f"{display_path}:{lineno}: missing target {target}"

    docs_dir = root / "docs"
    is_docs_md = target_path.suffix.lower() == ".md" and (
        target_path.is_relative_to(docs_dir)
        if docs_dir.exists()
        else target_path.is_relative_to(root)
    )
    if sep and is_docs_md:
        anchors = anchor_cache.setdefault(target_path, extract_anchors(target_path))
        if frag not in anchors:
            return f"{display_path}:{lineno}: missing anchor {target}"
    return None


def collect_markdown_files(paths: list[str], root: Path) -> list[Path]:
    """Collect Markdown files from explicit path arguments or directories."""
    files: list[Path] = []
    for raw in paths:
        candidate = Path(raw)
        if not candidate.is_absolute():
            candidate = (root / candidate) if (root / candidate).exists() else candidate.resolve()
        if not candidate.exists():
            raise FileNotFoundError(f"path does not exist: {raw}")
        if candidate.is_file() and candidate.suffix.lower() == ".md":
            files.append(candidate)
        elif candidate.is_dir():
            files.extend(sorted(candidate.rglob("*.md")))
    return files


def main(argv: list[str] | None = None) -> int:
    """CLI entry point for checking doc links."""
    default_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "paths",
        nargs="*",
        default=["docs/api"],
        help="markdown files or directories to check (default: docs/api)",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=default_root,
        help="repository root directory (default: parent of scripts/)",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()

    try:
        files = collect_markdown_files(args.paths, root)
    except OSError as err:
        print(f"check_doc_links: {err}", file=sys.stderr)
        return EXIT_UNREADABLE

    findings: list[str] = []
    total_links = 0
    anchor_cache: dict[Path, set[str]] = {}

    for file_path in files:
        try:
            raw_text = file_path.read_text(encoding="utf-8")
        except OSError as err:
            print(f"check_doc_links: cannot read {file_path}: {err}", file=sys.stderr)
            return EXIT_UNREADABLE

        masked = mask_inline_code_spans(mask_fenced_code_blocks(raw_text))
        links = parse_links(masked)
        total_links += len(links)
        for lineno, target in links:
            finding = check_target(file_path, root, lineno, target, anchor_cache)
            if finding:
                findings.append(finding)

    if findings:
        for item in findings:
            print(item)
        return EXIT_FINDINGS

    print(f"{total_links} links checked in {len(files)} files")
    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
