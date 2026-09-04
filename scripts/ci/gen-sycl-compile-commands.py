#!/usr/bin/env python3
"""gen-sycl-compile-commands.py — Augment compile_commands.json with SYCL TU entries.

meson generates CUSTOM_COMMAND rules for icpx-compiled SYCL translation units,
which means those TUs do NOT appear in compile_commands.json and are invisible
to clang-tidy.  This script parses build.ninja, extracts those CUSTOM_COMMAND
entries, translates them to clang-tidy-compatible compiler invocations (replacing
icpx with clang++, dropping -fsycl), and appends the resulting entries to
compile_commands.json so the changed-file SYCL lint gate can cover them.

Usage:
    python3 scripts/ci/gen-sycl-compile-commands.py <build-dir>

Arguments:
    build-dir   Path to the meson SYCL build directory that already contains
                build.ninja and compile_commands.json.

The script writes the augmented compile_commands.json in-place.  Existing entries
are preserved unchanged; duplicates (same file already present) are skipped.

Copyright 2026 Lusoris
Licensed under the BSD+Patent License (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    https://opensource.org/licenses/BSDplusPatent
"""

import json
import re
import sys
from pathlib import Path


def parse_ninja_sycl_commands(build_ninja_path: Path) -> list[dict]:
    """Extract CUSTOM_COMMAND entries for SYCL .cpp files from build.ninja.

    Returns a list of dicts matching the compile_commands.json schema:
    {"directory": ..., "command": ..., "file": ...}
    """
    build_dir = build_ninja_path.resolve().parent

    content = build_ninja_path.read_text(encoding="utf-8")

    entries = []
    # Match lines of the form:
    #   build <out>: CUSTOM_COMMAND <src.cpp> | <compiler>
    #    COMMAND = <icpx flags ...> <src.cpp> -o <out>
    pattern = re.compile(
        r"^build\s+\S+:\s+CUSTOM_COMMAND\s+(\S+\.cpp)\s+\|.*icpx\s*\n"
        r"(?:[ \t]+\S[^\n]*\n)*?"
        r"[ \t]+COMMAND\s*=\s*(.+?)(?:\n|$)",
        re.MULTILINE,
    )

    for m in pattern.finditer(content):
        src_relative = m.group(1)  # e.g. ../src/./sycl/picture_sycl.cpp
        raw_command = m.group(2).strip()

        # Resolve the source path relative to the build directory.
        src_abs = (build_dir / src_relative).resolve()

        # Replace the icpx binary with clang++ and drop SYCL-specific flags
        # that stock clang-tidy/clang++ cannot parse.
        #
        # Flags removed:
        #   -fsycl-targets  — SYCL device targets; unsupported by clang++
        #   -fsycl          — SYCL device-compilation; unsupported by clang++
        #   -Xs ...         — icpx AOT device compilation flags
        #   -fp-model=...   — icpx floating point model
        #   -pedantic       — harmless but generates noise from SYCL headers
        #
        # The wrapper (clang-tidy-sycl.sh) injects:
        #   -isystem<sycl-include>  — resolves <sycl/sycl.hpp>
        #   -extra-arg-before=-std=c++20
        #   -Wno-unknown-warning-option / -Wno-unknown-pragmas
        #
        # We still keep the -I include paths and -D defines from the original
        # icpx command so clang-tidy can resolve project headers.
        cmd = re.sub(
            r"(?:/opt/intel/oneapi/compiler/[^/]+/bin/)?icpx\b",
            "clang++",
            raw_command,
        )
        cmd = re.sub(r"\s+-fsycl-targets=\S+", "", cmd)
        cmd = re.sub(r"\s+-fsycl\b", "", cmd)
        cmd = re.sub(r"\s+-Xs\s+'[^']*'", "", cmd)
        cmd = re.sub(r"\s+-Xs\s+\S+", "", cmd)
        cmd = re.sub(r"\s+-fp-model=\S+", "", cmd)
        cmd = re.sub(r"\s+-pedantic\b", "", cmd)
        # Replace the output argument -o <obj> with nothing (clang-tidy
        # ignores compilation output).
        cmd = re.sub(r"\s+-o\s+\S+", "", cmd)

        entries.append(
            {
                "directory": str(build_dir),
                "command": cmd,
                "file": str(src_abs),
            }
        )

    return entries


def main(argv: list[str]) -> int:
    if len(argv) != 2:  # noqa: PLR2004 — 2 args is the spec
        print(
            f"usage: {argv[0]} <build-dir>",
            file=sys.stderr,
        )
        return 1

    build_dir = Path(argv[1])
    ninja_path = build_dir / "build.ninja"
    cc_path = build_dir / "compile_commands.json"

    if not ninja_path.is_file():
        print(f"error: {ninja_path}: no such file", file=sys.stderr)
        return 1
    if not cc_path.is_file():
        print(f"error: {cc_path}: no such file", file=sys.stderr)
        return 1

    existing = json.loads(cc_path.read_text(encoding="utf-8"))

    new_entries = parse_ninja_sycl_commands(ninja_path)
    new_files = {e["file"] for e in new_entries}

    # Replace any existing entries for these files (so updated flags take effect)
    filtered_existing = [e for e in existing if e.get("file") not in new_files]
    added = len(new_entries)
    filtered_existing.extend(new_entries)

    cc_path.write_text(json.dumps(filtered_existing, indent=2) + "\n", encoding="utf-8")

    print(
        f"gen-sycl-compile-commands: added/updated {added} SYCL TU entries in {cc_path}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
