#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail when libvmaf.so exports anything but its public API (ADR-0379).

Every exported symbol must be either a ``vmaf_*`` function or object declared
in a public header under ``core/include/``, or belong to a C++ runtime whose
headers declare its namespace with default visibility: ``std`` (libstdc++) and,
in SYCL builds, ``sycl`` (DPC++). Template members of those namespaces that a TU
instantiates are exported whatever the compile flags, and they are the
runtime's definitions, not ours.

Anything else is an internal symbol leaking out of the shared object. It can be
interposed by a host application defining the same name, and consumers can
start depending on it. The library builds with ``-fvisibility=hidden``; this
test is what notices when a target stops getting that flag.

Usage: check_exported_symbols.py <libvmaf.so> <public include dir>
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

# Judged on the demangled name, because the mangling grammar has too many
# spellings for a std member to enumerate: cv- and ref-qualified members
# (_ZNKRSt8optional..., _ZNOSt10unexpected...), entities local to a std
# function (_ZZNSt7__cxx11...), typeinfo and vtables. Sanitizer and debug
# builds keep more of these out of line, so they export more of them.
RUNTIME_NAMESPACES = ("std::", "sycl::")
# "typeinfo for std::X", "guard variable for std::X::y", ... name the entity last.
SPECIAL_SYMBOL = re.compile(
    r"^(?:typeinfo name for |typeinfo for |vtable for |VTT for |guard variable for "
    r"|(?:non-)?virtual thunk to |covariant return thunk to |reference temporary #\d+ for )+"
)
# DPC++'s versioned namespace (sycl::_V1 mangles as 4sycl3_V1) also appears
# inside typeinfo for function types built from its classes, such as the
# async-handler type int(const sycl::device&), which no prefix test catches.
SYCL_RUNTIME = re.compile(r"^_Z.*4sycl3_V\d")


def exported_symbols(library: Path) -> list[str]:
    nm = shutil.which("nm")
    if nm is None:
        print("SKIP: nm not found")
        raise SystemExit(77)
    out = subprocess.run(
        [nm, "-D", "--defined-only", str(library)], check=True, capture_output=True, text=True
    ).stdout
    return sorted({line.split()[-1] for line in out.splitlines() if line.strip()})


def demangled(names: list[str]) -> dict[str, str]:
    cxxfilt = shutil.which("c++filt")
    if cxxfilt is None:
        print("SKIP: c++filt not found")
        raise SystemExit(77)
    out = subprocess.run(
        [cxxfilt], input="\n".join(names), check=True, capture_output=True, text=True
    ).stdout.splitlines()
    return dict(zip(names, out))


def qualified_name(plain: str) -> str:
    """The entity's qualified name: no argument list, no leading return type.

    A demangled template function carries its return type in front
    ("bool std::operator==<char, ...>(...)"), so the name is the last token
    before the argument list, counted outside template brackets. A misread
    here can only make the test stricter: the fallback is the whole string,
    which then fails the namespace test and is reported.
    """
    depth = 0
    head = plain
    for i, ch in enumerate(plain):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == "(" and depth == 0 and i > 0:
            head = plain[:i]
            break
    depth = 0
    start = 0
    for i, ch in enumerate(head):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == " " and depth == 0:
            start = i + 1
    return head[start:]


def runtime_owned(mangled: str, plain: str) -> bool:
    """True for a symbol that belongs to the C++ runtime, not to libvmaf."""
    if SYCL_RUNTIME.match(mangled):
        return True
    return qualified_name(SPECIAL_SYMBOL.sub("", plain)).startswith(RUNTIME_NAMESPACES)


def public_identifiers(include_dir: Path) -> set[str]:
    names: set[str] = set()
    for header in include_dir.rglob("*.h"):
        names.update(re.findall(r"\bvmaf_\w+", header.read_text(errors="replace")))
    return names


def main() -> int:
    library, include_dir = Path(sys.argv[1]), Path(sys.argv[2])
    public = public_identifiers(include_dir)
    candidates = [name for name in exported_symbols(library) if name not in public]
    plain = demangled(candidates)
    leaked = [name for name in candidates if not runtime_owned(name, plain[name])]
    if leaked:
        print(f"{library.name} exports {len(leaked)} symbol(s) outside its public API:")
        for name in leaked:
            print(f"  {name}")
        print("Build the defining target with vmaf_cflags_common / vmaf_cppflags_common,")
        print("or declare the symbol VMAF_EXPORT in a public header if it is API.")
        return 1
    print(f"{library.name}: every export is public API or a C++ runtime's own")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
