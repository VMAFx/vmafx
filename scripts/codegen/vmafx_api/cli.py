# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Command line of the VMAFx API generator (ADR-1852).

    vmafx-api.py --write                 regenerate every output
    vmafx-api.py --check                 fail when a committed output differs
    vmafx-api.py --abi-check --against-ref v1.0.0
    vmafx-api.py --abi-check --against-file old.toml

Exit status: 0 clean, 1 findings (drift or a breaking change), 2 the
definition or the command line is invalid.
"""

from __future__ import annotations

import argparse
import difflib
import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

import tomllib

from . import abi_check, emit_c, emit_compat, emit_docs, emit_layout_test, emit_python
from .model import Api, DefinitionError, load, parse

DEFINITION = Path("core/api/vmafx.toml")
Renderer = Callable[[Api], str]
OUTPUTS: tuple[tuple[str, Renderer], ...] = (
    ("core/include/vmafx/vmafx.h", lambda api: emit_c.header_text(api, api.headers[0])),
    ("core/include/vmafx/libvmaf_bridge.h", lambda api: emit_c.header_text(api, api.headers[1])),
    ("core/src/vmafx/status_gen.h", emit_c.status_header),
    ("core/src/vmafx/status_gen.c", emit_c.status_source),
    ("core/src/vmafx/compat_libvmaf_gen.c", emit_compat.compat_source),
    ("core/test/test_vmafx_abi_layout.c", emit_layout_test.layout_test_source),
    ("bindings/python/vmafx/_api.py", emit_python.module_text),
    ("docs/api/vmafx/reference.md", emit_docs.reference_text),
)


def render(api: Api) -> dict[str, str]:
    return {path: renderer(api) for path, renderer in OUTPUTS}


def write(root: Path, files: dict[str, str]) -> int:
    for path, text in files.items():
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_text(encoding="utf-8") != text:
            target.write_text(text, encoding="utf-8")
            print(f"wrote {path}")
    return 0


def check(root: Path, files: dict[str, str]) -> int:
    stale = []
    for path, text in files.items():
        target = root / path
        current = target.read_text(encoding="utf-8") if target.exists() else ""
        if current == text:
            continue
        stale.append(path)
        diff = difflib.unified_diff(
            current.splitlines(),
            text.splitlines(),
            f"committed/{path}",
            f"generated/{path}",
            lineterm="",
        )
        print("\n".join(list(diff)[:40]))
    if stale:
        print(f"{len(stale)} generated file(s) differ from {DEFINITION}: {', '.join(stale)}")
        print("Run: python3 scripts/codegen/vmafx-api.py --write (never edit generated files)")
        return 1
    print(f"{len(files)} generated files match {DEFINITION}")
    return 0


def _old_definition(root: Path, args: argparse.Namespace) -> Api:
    if args.against_file:
        return load(Path(args.against_file))
    git = shutil.which("git")
    if git is None:
        raise OSError("--against-ref needs git on PATH")
    text = subprocess.run(  # noqa: S603 -- resolved git, fixed argv
        [git, "-C", str(root), "show", f"{args.against_ref}:{DEFINITION.as_posix()}"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return parse(tomllib.loads(text))


def abi(root: Path, api: Api, args: argparse.Namespace) -> int:
    findings = abi_check.compare(_old_definition(root, args), api)
    for finding in findings:
        print(finding)
    if findings:
        print("A break needs a higher ABI major, `!` in the PR title and a `Migration:` footer.")
        return 1
    print("definition is an append-only successor")
    return 0


def _arguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--definition", type=Path, default=None)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--abi-check", action="store_true")
    parser.add_argument("--against-ref")
    parser.add_argument("--against-file")
    args = parser.parse_args(argv)
    if args.abi_check and not (args.against_ref or args.against_file):
        parser.error("--abi-check needs --against-ref or --against-file")
    return args


def main(argv: list[str] | None = None) -> int:
    args = _arguments(argv)
    root = args.root.resolve()
    try:
        api = load(args.definition or root / DEFINITION)
        if args.abi_check:
            return abi(root, api, args)
        files = render(api)
    except (
        DefinitionError,
        OSError,
        subprocess.CalledProcessError,
        tomllib.TOMLDecodeError,
    ) as err:
        print(f"vmafx-api: {err}", file=sys.stderr)
        return 2
    return write(root, files) if args.write else check(root, files)
