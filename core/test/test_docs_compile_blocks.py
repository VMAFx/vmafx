#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Extract and compile fenced C code blocks in documentation (#2436).

Finds fenced C blocks marked with <!-- compile --> across documentation files
and verifies that they compile cleanly against the library headers using
-std=c11 -Wall -Wextra -Werror -fsyntax-only.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

MAX_FILES = 100_000
MAX_LINES_PER_FILE = 100_000
MAX_INCLUDES = 1_000
MAX_BLOCKS = 10_000


@dataclass(frozen=True)
class CodeBlock:
    """A fenced C code block extracted from documentation."""

    path: Path
    line_number: int
    code: str


def check_compiler_compatibility(cc_cmd: list[str]) -> None:
    """Exit 77 if the compiler is MSVC/cl-style, as it does not accept GCC flags."""
    if not cc_cmd:
        print("Error: VMAF_DOCS_CC command is empty", file=sys.stderr)
        sys.exit(1)
    raw = cc_cmd[0].lower()
    argv0 = Path(cc_cmd[0]).name.lower()
    if "cl.exe" in raw or "msvc" in raw or "clang-cl" in raw or argv0 == "cl":
        print(
            f"Skipping: compiler '{cc_cmd[0]}' is not GCC/Clang-style (exit 77)",
            file=sys.stderr,
        )
        sys.exit(77)


def extract_file_blocks(doc_path: Path) -> tuple[list[CodeBlock], list[str]]:
    """Scan a single markdown file for <!-- compile --> fenced C blocks."""
    try:
        content = doc_path.read_text(encoding="utf-8")
    except OSError as err:
        return [], [f"{doc_path}:1: error: failed to read file: {err}"]

    lines = content.splitlines()
    blocks: list[CodeBlock] = []
    errors: list[str] = []
    i = 0
    n = min(len(lines), MAX_LINES_PER_FILE)
    while i < n:
        line = lines[i]
        if line.strip() == "<!-- compile -->":
            marker_line = i + 1
            j = i + 1
            while j < n and lines[j].strip() == "":
                j += 1
            if j >= n or lines[j].strip() != "```c":
                errors.append(
                    f"{doc_path}:{marker_line}: error: "
                    "<!-- compile --> marker not followed by '```c' fence"
                )
                i = j + 1
                continue
            fence_line = j + 1
            k = j + 1
            body: list[str] = []
            found_closing = False
            while k < n:
                if lines[k].strip() == "```":
                    found_closing = True
                    break
                body.append(lines[k])
                k += 1
            if not found_closing:
                errors.append(f"{doc_path}:{fence_line}: error: unterminated code fence")
                break
            blocks.append(
                CodeBlock(
                    path=doc_path,
                    line_number=marker_line,
                    code="\n".join(body) + "\n",
                )
            )
            i = k + 1
        else:
            i += 1
    return blocks, errors


def extract_blocks(docs_dir: Path) -> tuple[list[CodeBlock], list[str]]:
    """Scan all markdown files under docs_dir (sorted) for compile blocks."""
    if not docs_dir.is_dir():
        return [], [f"Error: docs directory '{docs_dir}' does not exist"]
    all_blocks: list[CodeBlock] = []
    all_errors: list[str] = []
    md_paths = sorted(docs_dir.rglob("*.md"))
    for doc_path in md_paths[:MAX_FILES]:
        if not doc_path.is_file():
            continue
        blocks, errors = extract_file_blocks(doc_path)
        all_blocks.extend(blocks)
        all_errors.extend(errors)
    return all_blocks, all_errors


def compile_block(
    block: CodeBlock,
    cc_cmd: list[str],
    include_dirs: list[Path],
    temp_dir: Path,
) -> tuple[bool, str]:
    """Compile a single extracted C block with -fsyntax-only."""
    c_file = temp_dir / f"snippet_{block.path.stem}_{block.line_number}.c"
    try:
        c_file.write_text(block.code, encoding="utf-8")
    except OSError as err:
        return (
            False,
            f"{block.path}:{block.line_number}: error writing temp C file: {err}",
        )

    cmd = (
        list(cc_cmd)
        + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only"]
        + [f"-I{inc}" for inc in include_dirs[:MAX_INCLUDES]]
        + [str(c_file)]
    )
    try:
        proc = subprocess.run(  # noqa: S603
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as err:
        return (
            False,
            f"{block.path}:{block.line_number}: error running compiler: {err}",
        )

    if proc.returncode != 0:
        err_msg = proc.stderr.strip() or proc.stdout.strip() or "compilation failed"
        return (
            False,
            f"{block.path}:{block.line_number}: error:\n{err_msg}",
        )
    return True, ""


def run_doc_compile(
    docs_dir: Path,
    include_dirs: list[Path],
    cc_cmd: list[str],
) -> tuple[bool, int, list[str]]:
    """Scan docs_dir and compile every extracted block."""
    blocks, parse_errors = extract_blocks(docs_dir)
    if parse_errors:
        return False, len(blocks), parse_errors
    if not blocks:
        return (
            False,
            0,
            ["Error: 0 blocks compiled; a scan that finds nothing is broken"],
        )

    compile_errors: list[str] = []
    with tempfile.TemporaryDirectory() as tmp_str:
        tmp_dir = Path(tmp_str)
        for block in blocks[:MAX_BLOCKS]:
            ok, msg = compile_block(block, cc_cmd, include_dirs, tmp_dir)
            if not ok:
                compile_errors.append(msg)

    if compile_errors:
        return False, len(blocks), compile_errors
    return True, len(blocks), []


def run_self_test(cc_cmd: list[str], include_dirs: list[Path]) -> int:
    """Run self-tests verifying extraction and failure detection behavior."""
    results: list[tuple[str, bool]] = []
    with tempfile.TemporaryDirectory() as tmp_str:
        tmp_path = Path(tmp_str)

        # Case (a): one good block -> must pass
        dir_a = tmp_path / "case_a"
        dir_a.mkdir()
        (dir_a / "good.md").write_text(
            "# Good\n\n<!-- compile -->\n```c\nint main(void) {\n    return 0;\n}\n```\n",
            encoding="utf-8",
        )
        ok_a, _, _ = run_doc_compile(dir_a, include_dirs, cc_cmd)
        results.append(("case (a) one good block", ok_a))

        # Case (b): one block with an undeclared identifier -> must fail
        dir_b = tmp_path / "case_b"
        dir_b.mkdir()
        (dir_b / "bad_id.md").write_text(
            "# Bad ID\n\n<!-- compile -->\n```c\nint main(void) {\n"
            "    return undeclared_identifier_xyz;\n}\n```\n",
            encoding="utf-8",
        )
        ok_b, _, _ = run_doc_compile(dir_b, include_dirs, cc_cmd)
        results.append(("case (b) undeclared identifier", not ok_b))

        # Case (c): a marker with no ```c fence after it -> must fail
        dir_c = tmp_path / "case_c"
        dir_c.mkdir()
        (dir_c / "bad_fence.md").write_text(
            "# Bad Fence\n\n<!-- compile -->\nint not_a_fence = 0;\n",
            encoding="utf-8",
        )
        ok_c, _, _ = run_doc_compile(dir_c, include_dirs, cc_cmd)
        results.append(("case (c) marker with no ```c fence", not ok_c))

        # Case (d): no markers -> must fail
        dir_d = tmp_path / "case_d"
        dir_d.mkdir()
        (dir_d / "no_markers.md").write_text(
            "# No Markers\n\nJust markdown text.\n",
            encoding="utf-8",
        )
        ok_d, _, _ = run_doc_compile(dir_d, include_dirs, cc_cmd)
        results.append(("case (d) no markers", not ok_d))

    all_passed = True
    for label, passed in results:
        status = "PASS" if passed else "FAIL"
        print(f"{label}: {status}")
        if not passed:
            all_passed = False

    return 0 if all_passed else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Compile C code blocks marked with <!-- compile --> in docs (#2436)"
    )
    default_docs = Path(__file__).resolve().parents[2] / "docs"
    parser.add_argument(
        "--docs",
        type=Path,
        default=default_docs,
        help="Directory containing markdown files (default: repo docs/)",
    )
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        type=Path,
        help="Include directory for C compilation (repeatable)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run self-test suite and exit",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Main entrypoint for test_docs_compile_blocks."""
    args = parse_args(argv)
    cc_env = os.environ.get("VMAF_DOCS_CC", "cc")
    cc_cmd = shlex.split(cc_env)
    check_compiler_compatibility(cc_cmd)

    if args.self_test:
        return run_self_test(cc_cmd, args.include)

    ok, count, errors = run_doc_compile(args.docs, args.include, cc_cmd)
    if not ok:
        for err in errors:
            print(err, file=sys.stderr)
        return 1

    print(f"{count} blocks compiled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
