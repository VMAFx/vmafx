#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""gen-sycl-compile-commands.py — Augment compile_commands.json with SYCL TU entries.

meson generates CUSTOM_COMMAND rules for icpx-compiled SYCL translation units,
which means those TUs do NOT appear in compile_commands.json and are invisible
to clang-tidy. This script parses build.ninja, extracts those CUSTOM_COMMAND
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
"""

import json
import re
import sys
from pathlib import Path

# ninja names the rule CUSTOM_COMMAND, or CUSTOM_COMMAND_DEP when the meson
# target has a depfile (the SYCL feature TUs since PR #1764); the DEPFILE
# lines then stand between the build statement and COMMAND.
SYCL_COMMAND_PATTERN = re.compile(
    r"^build\s+\S+:\s+CUSTOM_COMMAND(?:_DEP)?\s+(\S+\.cpp)\s+\|.*icpx\s*\n"
    r"(?:[ \t]+\S[^\n]*\n)*?"
    r"[ \t]+COMMAND\s*=\s*(.+?)(?:\n|$)",
    re.MULTILINE,
)

# Every build statement that compiles a .cpp with icpx, whatever its rule is
# called. One that SYCL_COMMAND_PATTERN does not parse would leave its TU out
# of the database, and a lane that measures no SYCL TU still reports clean.
SYCL_BUILD_STATEMENT = re.compile(
    r"^build\s+\S+:\s+\S+\s+\S+\.cpp\s+\|.*icpx\s*$",
    re.MULTILINE,
)


class UnparsedSyclCommandError(RuntimeError):
    """build.ninja compiles a SYCL TU through a statement this script cannot parse."""


EXPECTED_ARGUMENT_COUNT = 2


# Replace the icpx binary with clang++ and drop SYCL-specific flags
# that stock clang-tidy/clang++ cannot parse.
#
# Flags removed:
#   -fsycl-targets  — SYCL device targets; unsupported by clang++
#   -fno-sycl-rdc   — SYCL device-link policy; irrelevant to analysis
#   --offload-compress, --offload-compression-level=<n>
#                   — device-image compression (ADR-1590); nothing to analyse
#   -fsycl          — SYCL device-compilation; unsupported by clang++
#   -Xsycl-target-backend[=<target>] <arg>
#                   — icpx AOT backend argument, scoped or unscoped
#   -Xs ...         — legacy icpx AOT device compilation flags
#
# Flags translated rather than dropped:
#   -fp-model=...   — rewritten to clang's -ffp-model spelling, so the
#                     analyzer sees the same floating-point contract the
#                     real build uses instead of clang's default.
#
# -pedantic is deliberately NOT stripped: it is the flag the build is
# held to, and the wrapper's -Wno-unknown-* pair below already silences
# the SYCL-header noise that stripping it used to avoid.
#
# The wrapper (clang-tidy-sycl.sh) injects:
#   -isystem<sycl-include>  — resolves <sycl/sycl.hpp>
#   -extra-arg-before=-std=c++20
#   -Wno-unknown-warning-option / -Wno-unknown-pragmas
#
# We still keep the -I include paths and -D defines from the original
# icpx command so clang-tidy can resolve project headers.
# Replace the output argument too; clang-tidy ignores compilation output.
def clang_tidy_command(raw_command: str) -> str:
    """Translate one icpx command into the stock-clang analyzer profile.

    `-foffload-fp32-prec-div` / `-foffload-fp32-prec-sqrt` (ADR-1367) only set
    how the device rounds fp32 `/` and sqrt; stock clang rejects them as
    unknown arguments, and the host analysis does not depend on them.

    `-MD -MF <file>` (PR #1764) writes the build's depfile; the analyzer's
    command drops it so a tidy run leaves the build's dependency record alone.
    """
    command = re.sub(
        r"(?:/opt/intel/oneapi/compiler/[^/]+/bin/)?icpx\b",
        "clang++",
        raw_command,
    )
    command = re.sub(r"\s+-fsycl-targets=\S+", "", command)
    command = re.sub(r"\s+-fno-sycl-rdc\b", "", command)
    command = re.sub(r"\s+--offload-compression-level=\S+", "", command)
    command = re.sub(r"\s+--offload-compress\b", "", command)
    command = re.sub(r"\s+-fsycl\b", "", command)
    command = re.sub(
        r"\s+-Xsycl-target-backend(?:=\S+)?\s+(?:'[^']*'|\"[^\"]*\"|\S+)",
        "",
        command,
    )
    command = re.sub(r"\s+-Xs\s+'[^']*'", "", command)
    command = re.sub(r"\s+-Xs\s+\S+", "", command)
    command = re.sub(r"\s+-foffload-fp32-prec-(?:div|sqrt)\b", "", command)
    command = re.sub(r"(\s+)-fp-model=", r"\1-ffp-model=", command)
    # The depfile is the build's: an analyzer run must not overwrite it.
    command = re.sub(r"\s+-MF\s+\S+", "", command)
    command = re.sub(r"\s+-MM?D\b", "", command)
    return re.sub(r"\s+-o\s+\S+", "", command)


def require_every_statement_parsed(build_ninja_path: Path, content: str, parsed: int) -> None:
    """Raise when build.ninja compiles more .cpp files with icpx than were parsed."""
    statements = len(SYCL_BUILD_STATEMENT.findall(content))
    if statements != parsed:
        raise UnparsedSyclCommandError(
            f"{build_ninja_path}: {statements} build statements compile a .cpp with icpx, "
            f"{parsed} were parsed; the rule name or layout changed"
        )


def parse_ninja_sycl_commands(build_ninja_path: Path) -> list[dict]:
    """Extract CUSTOM_COMMAND entries for SYCL .cpp files from build.ninja.

    Returns a list of dicts matching the compile_commands.json schema:
    {"directory": ..., "command": ..., "file": ...}

    Raises UnparsedSyclCommandError when build.ninja compiles more .cpp files
    with icpx than this function extracted.
    """
    build_dir = build_ninja_path.resolve().parent

    content = build_ninja_path.read_text(encoding="utf-8")

    entries = []
    for m in SYCL_COMMAND_PATTERN.finditer(content):
        src_relative = m.group(1)  # e.g. ../src/./sycl/picture_sycl.cpp
        raw_command = m.group(2).strip()

        # Resolve the source path relative to the build directory.
        src_abs = (build_dir / src_relative).resolve()

        # The analyzer's form of the command: see clang_tidy_command().
        cmd = clang_tidy_command(raw_command)

        entries.append(
            {
                "directory": str(build_dir),
                "command": cmd,
                "file": str(src_abs),
            }
        )

    require_every_statement_parsed(build_ninja_path, content, len(entries))
    return entries


def main(argv: list[str]) -> int:
    if len(argv) != EXPECTED_ARGUMENT_COUNT:
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

    try:
        new_entries = parse_ninja_sycl_commands(ninja_path)
    except UnparsedSyclCommandError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
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
