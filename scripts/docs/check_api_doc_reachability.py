#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check reachability of all exported VMAFx symbols in public documentation.

Every symbol in core/src/vmafx_symbols.txt must appear as a whole word in
docs/api/, docs/getting-started/, or docs/usage/. Exits 1 naming each missing symbol.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

WORD_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def load_symbols(symbols_file: Path) -> list[str]:
    """Load symbol names from a vmafx_symbols.txt file."""
    symbols: list[str] = []
    lines = symbols_file.read_text(encoding="utf-8").splitlines()
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split()
        if parts:
            symbols.append(parts[0])
    return symbols


def load_doc_words(doc_dirs: list[Path]) -> set[str]:
    """Collect all whole words appearing in markdown files across doc_dirs."""
    words: set[str] = set()
    for doc_dir in doc_dirs:
        if not doc_dir.is_dir():
            continue
        md_files = sorted(doc_dir.rglob("*.md"))
        for path in md_files:
            text = path.read_text(encoding="utf-8")
            words.update(WORD_RE.findall(text))
    return words


def find_missing_symbols(symbols: list[str], doc_words: set[str]) -> list[str]:
    """Return symbols that do not appear as whole words in documentation."""
    return sorted(sym for sym in symbols if sym not in doc_words)


def default_paths() -> tuple[Path, list[Path]]:
    """Determine default symbols file and docs directories relative to repo root."""
    repo_root = Path(__file__).resolve().parents[2]
    symbols_file = repo_root / "core" / "src" / "vmafx_symbols.txt"
    doc_dirs = [
        repo_root / "docs" / "api",
        repo_root / "docs" / "getting-started",
        repo_root / "docs" / "usage",
    ]
    return symbols_file, doc_dirs


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments."""
    def_symbols, def_doc_dirs = default_paths()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--symbols",
        type=Path,
        default=def_symbols,
        help="Path to symbols list (default: core/src/vmafx_symbols.txt)",
    )
    parser.add_argument(
        "--docs-dirs",
        type=Path,
        nargs="+",
        default=def_doc_dirs,
        help="Directories of markdown documentation to scan",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Check that every symbol is present in the specified documentation dirs."""
    args = parse_args(argv)
    if not args.symbols.is_file():
        sys.stderr.write(f"check_api_doc_reachability: symbols file not found: {args.symbols}\n")
        return 1

    symbols = load_symbols(args.symbols)
    doc_words = load_doc_words(args.docs_dirs)
    missing = find_missing_symbols(symbols, doc_words)

    if missing:
        for sym in missing:
            sys.stderr.write(f"missing symbol from api docs: {sym}\n")
        return 1

    print(f"check_api_doc_reachability: all {len(symbols)} symbols reached in docs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
