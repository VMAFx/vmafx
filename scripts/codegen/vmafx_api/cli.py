# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Command line of the VMAFx API generator (ADR-1852).

    vmafx-api.py --write                 regenerate every output
    vmafx-api.py --check                 fail when a committed output differs
    vmafx-api.py --abi-check --against-ref origin/rc4/api-generation-prototype
    vmafx-api.py --abi-check --against-merge-base origin/master
    vmafx-api.py --abi-check --against-file old.toml
    vmafx-api.py --changelog origin/master

Exit status: 0 clean, 1 findings (drift or a breaking change), 2 the
definition or the command line is invalid, 77 the comparison could not run
(no git, unknown ref, shallow clone, no definition at the base); the reason is
printed.
"""

from __future__ import annotations

import argparse
import difflib
import sys
from collections.abc import Callable
from pathlib import Path

import tomllib

from . import (
    abi_check,
    changelog,
    emit_build,
    emit_c,
    emit_compat,
    emit_docs,
    emit_layout_test,
    emit_python,
    emit_symbols,
    gitref,
)
from .headers import plan
from .loader import load
from .model import Api, DefinitionError

DEFINITION = Path("core/api/vmafx.toml")
SKIP = 77  # Meson's "test skipped"
Renderer = Callable[[Api], str]
FIXED_OUTPUTS: tuple[tuple[str, Renderer], ...] = (
    ("core/include/vmafx/meson.build", emit_build.header_install),
    ("core/src/vmafx/status_gen.h", emit_c.status_header),
    ("core/src/vmafx/status_gen.c", emit_c.status_source),
    ("core/src/vmafx/compat_libvmaf_gen.c", emit_compat.compat_source),
    ("core/src/vmafx.map", emit_symbols.version_script),
    ("core/src/vmafx.def", emit_symbols.def_file),
    ("core/src/vmafx_symbols.txt", emit_symbols.symbol_list),
    ("core/test/test_vmafx_abi_layout.c", emit_layout_test.layout_test_source),
    ("bindings/python/vmafx/_api.py", emit_python.module_text),
)


def render(api: Api) -> dict[str, str]:
    """Every generated file: path -> text."""
    files = {f"core/include/{p.header.path}": emit_c.header_text(api, p) for p in plan(api)}
    files.update({path: renderer(api) for path, renderer in FIXED_OUTPUTS})
    files.update(dict(emit_docs.pages(api)))
    return files


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


def _old_definition(root: Path, args: argparse.Namespace) -> tuple[Api, str]:
    if args.against_file:
        return load(Path(args.against_file)), args.against_file
    ref = args.against_ref or args.changelog
    if args.against_merge_base:
        ref = gitref.merge_base(root, args.against_merge_base)
    return gitref.definition_at(root, ref, DEFINITION), ref


def abi(root: Path, api: Api, args: argparse.Namespace) -> int:
    old, ref = _old_definition(root, args)
    result = abi_check.report(old, api)
    for finding in result.findings:
        print(finding)
    if result.findings:
        print(
            "A break needs a higher ABI major (minor within 0.x), `!` in the PR title and a `Migration:` footer."
        )
        return 1
    for item in result.accepted:
        print(f"accepted break (ABI {old.abi_version} -> {api.abi_version}): {item}")
    print(f"definition is an append-only successor of {ref} ({len(result.added)} additions)")
    return 0


def _arguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--definition", type=Path, default=None)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--abi-check", action="store_true")
    mode.add_argument("--changelog", metavar="BASE_REF")
    against = parser.add_mutually_exclusive_group()
    against.add_argument("--against-ref")
    against.add_argument("--against-merge-base", metavar="REF")
    against.add_argument("--against-file")
    args = parser.parse_args(argv)
    if args.abi_check and not (args.against_ref or args.against_file or args.against_merge_base):
        parser.error("--abi-check needs --against-ref, --against-merge-base or --against-file")
    return args


def _run(root: Path, args: argparse.Namespace) -> int:
    api = load(args.definition or root / DEFINITION)
    if args.abi_check:
        return abi(root, api, args)
    if args.changelog:
        print(changelog.draft(_old_definition(root, args)[0], api), end="")
        return 0
    files = render(api)
    return write(root, files) if args.write else check(root, files)


def main(argv: list[str] | None = None) -> int:
    args = _arguments(argv)
    root = args.root.resolve()
    try:
        return _run(root, args)
    except gitref.Unavailable as err:
        print(f"SKIP: vmafx-api: {err}")
        return SKIP
    except (DefinitionError, OSError, tomllib.TOMLDecodeError) as err:
        print(f"vmafx-api: {err}", file=sys.stderr)
        return 2
