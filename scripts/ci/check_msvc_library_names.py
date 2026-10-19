#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check the library files a static MSVC-like build installed (Q-299).

A static MSVC, clang-cl or icx-cl build installs `lib/vmaf.lib` and
`lib/vmafx.lib`, the files the MSVC linker opens for `-lvmaf` / `-lvmafx`, and
not Meson's classic `libvmaf.a` / `libvmafx.a`. The pkg-config files name them
the same way: `libvmaf.pc` has `-lvmaf` and requires `libvmafx`, `libvmafx.pc`
has `-lvmafx`. Netflix/vmaf 3b4dd350e; core/src/meson.build
(`vmaf_static_name_kwargs`).

With `--meson-log`, the configure log must also be free of the pkg-config
module's warning that a library target with `name_prefix` / `name_suffix` may
not be found from its `-l` flag: core/src/meson.build hands the MSVC names to
the module as flags so that the warning does not appear (ADR-2828).

Usage: check_msvc_library_names.py --prefix DIR [--libdir lib] [--meson-log FILE]
Exit 0 when everything is in place, 1 with one line per problem otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LIBS = ("vmaf", "vmafx")
NAME_WARNING = re.compile(r"WARNING: Library target '[^']+' has 'name_(?:prefix|suffix)' set")


def problems(prefix: Path, libdir: str = "lib") -> list[str]:
    lib = prefix / libdir
    found: list[str] = []
    for name in LIBS:
        if not (lib / f"{name}.lib").is_file():
            found.append(f"{lib / (name + '.lib')}: missing")
        classic = lib / f"lib{name}.a"
        if classic.exists():
            found.append(
                f"{classic}: Meson's classic name, which -l{name} does not open with link.exe"
            )
    pc = lib / "pkgconfig"
    expect = {
        "libvmaf.pc": (r"^Libs:.*\s-lvmaf(\s|$)", r"^Requires:.*\blibvmafx\b"),
        "libvmafx.pc": (r"^Libs:.*\s-lvmafx(\s|$)",),
    }
    for file, patterns in expect.items():
        path = pc / file
        if not path.is_file():
            found.append(f"{path}: missing")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in patterns:
            if not re.search(pattern, text, re.M):
                found.append(f"{path}: no line matching {pattern!r}")
    return found


def log_problems(meson_log: Path) -> list[str]:
    """The pkg-config name warnings of a Meson configure log, one problem each."""
    if not meson_log.is_file():
        return [f"{meson_log}: missing"]
    text = meson_log.read_text(encoding="utf-8", errors="replace")
    return [f"{meson_log}: {m.group(0)}" for m in NAME_WARNING.finditer(text)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--libdir", default="lib")
    parser.add_argument("--meson-log", type=Path)
    args = parser.parse_args(argv)
    found = problems(args.prefix, args.libdir)
    if args.meson_log is not None:
        found += log_problems(args.meson_log)
    for line in found:
        print(f"check_msvc_library_names: {line}")
    if not found:
        print(
            f"check_msvc_library_names: {args.prefix / args.libdir}: vmaf.lib, vmafx.lib and pkg-config files in place"
        )
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
