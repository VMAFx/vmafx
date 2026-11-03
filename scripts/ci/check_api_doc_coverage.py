#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that every public VMAFx symbol in headers is documented in docs/api/."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

WORD_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def strip_c_comments(text: str) -> str:
    """Remove block and line comments from C source."""
    return re.sub(r"(/\*.*?\*/)|(//[^\n]*)", " ", text, flags=re.DOTALL)


def is_vmafx_name(name: str) -> bool:
    """Return True if name starts with vmafx_, VMAFX_, or Vmafx."""
    return bool(re.match(r"^(?:vmafx_|VMAFX_|Vmafx)", name))


def collect_include_guards(clean_text: str) -> set[str]:
    """Find macros defined as header include guards."""
    return set(re.findall(r"#\s*ifndef\s+([A-Za-z0-9_]+)\s+#\s*define\s+\1\b", clean_text))


def collect_macros(clean_text: str, guards: set[str]) -> set[str]:
    """Collect macro names declared with #define, skipping include guards."""
    macros: set[str] = set()
    for match in re.finditer(r"#\s*define\s+([A-Za-z0-9_]+)", clean_text):
        name = match.group(1)
        if name not in guards and is_vmafx_name(name):
            macros.add(name)
    return macros


def collect_types(clean_text: str) -> set[str]:
    """Collect type names declared in typedefs, structs, enums, and unions."""
    types: set[str] = set()
    for m in re.finditer(r"typedef\s+.*?\(\s*\*\s*([A-Za-z0-9_]+)\s*\)\s*\(", clean_text):
        if is_vmafx_name(m.group(1)):
            types.add(m.group(1))
    for m in re.finditer(
        r"typedef\s+(?:enum\s+|struct\s+|union\s+)?(?:[A-Za-z0-9_]+\s+|\*\s*)+([A-Za-z0-9_]+)\s*;",
        clean_text,
    ):
        if is_vmafx_name(m.group(1)):
            types.add(m.group(1))
    for m in re.finditer(r"\b(?:struct|enum|union)\s+([A-Za-z0-9_]+)\b", clean_text):
        if is_vmafx_name(m.group(1)):
            types.add(m.group(1))
    return types


def collect_enum_values(clean_text: str) -> set[str]:
    """Collect enum constant names declared inside enum blocks."""
    values: set[str] = set()
    for m in re.finditer(r"\benum(?:\s+[A-Za-z0-9_]+)?\s*\{([^}]+)\}", clean_text):
        for item in m.group(1).split(","):
            raw_item = item.strip()
            if not raw_item:
                continue
            item_name = raw_item.split("=")[0].strip()
            match = re.search(r"\b([A-Za-z0-9_]+)\s*$", item_name)
            if match and is_vmafx_name(match.group(1)):
                values.add(match.group(1))
    return values


def collect_functions(clean_text: str) -> set[str]:
    """Collect function names declared in header prototypes."""
    funcs: set[str] = set()
    non_preproc = re.sub(r"^[ \t]*#.*$", "", clean_text, flags=re.MULTILINE)
    non_preproc = re.sub(r"typedef\b[^;]+;", "", non_preproc)
    for m in re.finditer(r"\b(vmafx_[A-Za-z0-9_]+)\s*\(", non_preproc):
        funcs.add(m.group(1))
    return funcs


def collect_header_symbols(header_path: Path) -> set[str]:
    """Collect all public vmafx_* symbols declared in a single C header."""
    raw = header_path.read_text(encoding="utf-8")
    clean = strip_c_comments(raw)
    guards = collect_include_guards(clean)

    symbols: set[str] = set()
    symbols.update(collect_macros(clean, guards))
    symbols.update(collect_types(clean))
    symbols.update(collect_enum_values(clean))
    symbols.update(collect_functions(clean))
    return symbols


def collect_all_symbols(headers_dir: Path) -> set[str]:
    """Collect declared symbols across all public headers in headers_dir."""
    all_symbols: set[str] = set()
    for header in sorted(headers_dir.glob("*.h")):
        syms = collect_header_symbols(header)
        if header.name == "libvmaf_bridge.h" and not any(
            s.startswith(("vmafx_", "VMAFX_")) for s in syms
        ):
            continue
        all_symbols.update(syms)
    return all_symbols


def load_doc_words(docs_dir: Path) -> set[str]:
    """Collect all words appearing in markdown files under docs_dir."""
    words: set[str] = set()
    if not docs_dir.is_dir():
        return words
    for md_file in sorted(docs_dir.rglob("*.md")):
        text = md_file.read_text(encoding="utf-8")
        words.update(WORD_RE.findall(text))
    return words


def load_allowlist(allow_path: Path | None) -> set[str]:
    """Load allowed symbols from an optional file with <symbol> <reason> columns."""
    if allow_path is None or not allow_path.is_file():
        return set()
    allowed: set[str] = set()
    for line in allow_path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split()
        if parts:
            allowed.add(parts[0])
    return allowed


def find_missing_symbols(
    symbols: set[str], doc_words: set[str], allowed: set[str] | None = None
) -> list[str]:
    """Return sorted symbols that are not present in doc_words and not allowed."""
    allow_set = allowed if allowed is not None else set()
    return sorted(sym for sym in symbols if sym not in doc_words and sym not in allow_set)


def default_paths() -> tuple[Path, Path]:
    """Return default (headers_dir, docs_dir) relative to repository root."""
    repo_root = Path(__file__).resolve().parents[2]
    headers_dir = repo_root / "core" / "include" / "vmafx"
    docs_dir = repo_root / "docs" / "api"
    return headers_dir, docs_dir


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse CLI arguments."""
    def_headers, def_docs = default_paths()
    parser = argparse.ArgumentParser(
        description="Verify public VMAFx API symbol documentation coverage."
    )
    parser.add_argument(
        "--headers-dir",
        type=Path,
        default=def_headers,
        help="Directory containing public VMAFx headers (default: core/include/vmafx)",
    )
    parser.add_argument(
        "--docs-dir",
        type=Path,
        default=def_docs,
        help="Directory containing API markdown documentation (default: docs/api)",
    )
    parser.add_argument(
        "--allow",
        type=Path,
        default=None,
        help="Optional file listing allowed undocumented symbols with a reason column",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Run coverage check; print missing symbols sorted and return exit code."""
    args = parse_args(argv)
    if not args.headers_dir.is_dir():
        print(f"error: headers directory not found: {args.headers_dir}", file=sys.stderr)
        return 1

    symbols = collect_all_symbols(args.headers_dir)
    doc_words = load_doc_words(args.docs_dir)
    allowed = load_allowlist(args.allow)
    missing = find_missing_symbols(symbols, doc_words, allowed)

    if missing:
        for sym in missing:
            print(f"missing symbol from api docs: {sym}", file=sys.stderr)
        return 1

    print(f"check_api_doc_coverage: all {len(symbols)} public symbols documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
