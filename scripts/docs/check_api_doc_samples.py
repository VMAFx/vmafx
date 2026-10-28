#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Scan documentation markdown files for compile-marked C samples.

Blocks must be fenced ```c blocks preceded on the previous non-blank line by
`<!-- compile -->`. Samples must be complete translation units containing at
least one `#include` directive.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

COMPILE_TAG_RE = re.compile(r"^<!--\s*compile\s*-->$")
INCLUDE_RE = re.compile(r"^\s*#\s*include\b", re.M)


@dataclass(frozen=True)
class Sample:
    """A marked C code sample extracted from documentation."""

    doc_path: Path
    line: int
    index: int
    slug: str
    code: str

    @property
    def filename(self) -> str:
        """The output filename for this sample."""
        return f"{self.slug}_{self.index}.c"


def page_slug(path: Path, docs_dir: Path) -> str:
    """Generate a filename-safe slug from a document path."""
    try:
        rel = path.resolve().relative_to(docs_dir.resolve()).with_suffix("")
    except ValueError:
        rel = path.with_suffix("")
    slug = re.sub(r"[^a-zA-Z0-9_]+", "_", str(rel)).strip("_")
    return slug or "index"


def scan_file(path: Path, docs_dir: Path) -> tuple[list[Sample], list[str]]:
    """Scan a single markdown file for compile-marked C blocks."""
    lines = path.read_text(encoding="utf-8").splitlines()
    samples: list[Sample] = []
    errors: list[str] = []
    slug = page_slug(path, docs_dir)
    sample_index = 0

    last_non_blank = ""
    in_fence = False
    fence_start_line = 0
    marked = False
    code_lines: list[str] = []

    for lineno, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not in_fence:
            if stripped.startswith("```"):
                in_fence = True
                fence_start_line = lineno
                code_lines = []
                fence_tag = stripped[3:].strip()
                is_c = fence_tag == "c" or fence_tag.startswith("c ")
                marked = is_c and bool(COMPILE_TAG_RE.match(last_non_blank))
            elif stripped:
                last_non_blank = stripped
        elif stripped.startswith("```") and len(stripped.rstrip("`")) == 0:
            in_fence = False
            last_non_blank = "```"
            if marked:
                sample_index += 1
                code = "\n".join(code_lines) + "\n"
                if not INCLUDE_RE.search(code):
                    errors.append(f"{path}:{fence_start_line}: marked sample missing #include")
                else:
                    samples.append(
                        Sample(
                            doc_path=path,
                            line=fence_start_line,
                            index=sample_index,
                            slug=slug,
                            code=code,
                        )
                    )
            marked = False
        else:
            code_lines.append(line)

    return samples, errors


def collect_samples(docs_dir: Path) -> tuple[list[Sample], list[str]]:
    """Scan all markdown files in docs_dir for compile-marked C blocks."""
    all_samples: list[Sample] = []
    all_errors: list[str] = []
    md_files = sorted(docs_dir.rglob("*.md"))
    for path in md_files:
        samples, errors = scan_file(path, docs_dir)
        all_samples.extend(samples)
        all_errors.extend(errors)
    return all_samples, all_errors


def extract_samples(samples: list[Sample], out_dir: Path) -> list[Path]:
    """Write extracted samples to the output directory."""
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for sample in samples:
        dest = out_dir / sample.filename
        dest.write_text(sample.code, encoding="utf-8")
        written.append(dest)
    return written


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--docs-dir",
        type=Path,
        default=None,
        help="Root directory of markdown documentation (default: docs/ in repo root)",
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--list",
        action="store_true",
        help="List documentation paths containing compile-marked samples",
    )
    group.add_argument(
        "--extract",
        type=Path,
        metavar="OUTDIR",
        help="Extract marked C code blocks to OUTDIR as <page-slug>_<n>.c",
    )
    return parser.parse_args(argv)


def find_default_docs_dir() -> Path:
    """Locate the default docs directory relative to this script or repo root."""
    candidate = Path(__file__).resolve().parents[2] / "docs"
    if candidate.is_dir():
        return candidate
    cwd_candidate = Path.cwd() / "docs"
    if cwd_candidate.is_dir():
        return cwd_candidate
    return candidate


def main(argv: list[str] | None = None) -> int:
    """Main entrypoint for checking or extracting doc samples."""
    args = parse_args(argv)
    docs_dir = args.docs_dir or find_default_docs_dir()
    if not docs_dir.is_dir():
        sys.stderr.write(f"check_api_doc_samples: docs directory not found: {docs_dir}\n")
        return 1

    samples, errors = collect_samples(docs_dir)
    if errors:
        for err in errors:
            sys.stderr.write(f"{err}\n")
        return 1

    if args.list:
        for sample in samples:
            print(sample.doc_path)
        return 0

    if args.extract:
        extract_samples(samples, args.extract)
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
