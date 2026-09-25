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
# The same test on the mangled form, for names c++filt cannot demangle: the
# binutils on older runners returns C++20 constraint manglings (Q... requires
# clauses, Tk concept parameters) unchanged. Matches an outermost std:: name --
# free (_ZSt), nested with cv/ref qualifiers (_ZNKRSt, _ZNOSt), local to a std
# function (_ZZNSt), and typeinfo, vtables and guard variables for them.
STD_MANGLED = re.compile(r"^_Z(?:T[ISV]|GV)?Z?(?:N[rVK]*[RO]?)?St")
# DPC++'s versioned namespace (sycl::_V1 mangles as 4sycl3_V1) also appears
# inside typeinfo for function types built from its classes, such as the
# async-handler type int(const sycl::device&), which no prefix test catches.
SYCL_RUNTIME = re.compile(r"^_Z.*4sycl3_V\d")
# The linker defines __start_<section> / __stop_<section> for every section
# named like a C identifier. GNU ld exports them from a shared library; lld
# hides them. Sanitizer builds emit such sections for their own metadata
# (ASan's global descriptors, SanitizerCoverage's guards and counters), so a
# GNU-ld sanitizer build of libvmaf exports these bounds without leaking API.
SANITIZER_SECTION_BOUND = re.compile(
    r"^__(?:start|stop)_(?:asan_globals|hwasan_globals|__sancov_\w+)$"
)
# icpx emits a per-variant entry point beside the real function when it clones
# one for offload, spelling it `<name>|_._.<n>._.<m>`. The pipe cannot occur in
# a C identifier, so the suffix is unambiguous. These are not new API: they are
# the same public function, and the parent is what the header declares. Strip
# the suffix before the public-name lookup so a variant is accepted exactly
# when its parent is. Observed on the SYCL build as
# `vmaf_dnn_session_run|_._.1._.1`, whose parent carries VMAF_EXPORT in
# core/include/libvmaf/dnn.h.
COMPILER_VARIANT_SUFFIX = re.compile(r"\|_\._\.\d+\._\.\d+$")


def exported_symbols(library: Path) -> list[str]:
    nm = shutil.which("nm")
    if nm is None:
        print("SKIP: nm not found")
        raise SystemExit(77)
    out = subprocess.run(  # noqa: S603 -- resolved nm path and the library path, no shell
        [nm, "-D", "--defined-only", str(library)], check=True, capture_output=True, text=True
    ).stdout
    return sorted({line.split()[-1] for line in out.splitlines() if line.strip()})


def demangled(names: list[str]) -> dict[str, str]:
    cxxfilt = shutil.which("c++filt")
    if cxxfilt is None:
        print("SKIP: c++filt not found")
        raise SystemExit(77)
    out = subprocess.run(  # noqa: S603 -- resolved c++filt path, names on stdin, no shell
        [cxxfilt], input="\n".join(names), check=True, capture_output=True, text=True
    ).stdout.splitlines()
    # c++filt answers one line per input line; a mismatch is a tool failure.
    return dict(zip(names, out, strict=True))


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
    """True for a symbol that belongs to a runtime (C++, SYCL, a sanitizer), not to libvmaf."""
    if SYCL_RUNTIME.match(mangled) or STD_MANGLED.match(mangled):
        return True
    if SANITIZER_SECTION_BOUND.match(mangled):
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
    candidates = [
        name
        for name in exported_symbols(library)
        if COMPILER_VARIANT_SUFFIX.sub("", name) not in public
    ]
    plain = demangled(candidates)
    leaked = [name for name in candidates if not runtime_owned(name, plain[name])]
    # Red-cap regression check: ADR-xxxx prevents C++ placement new/delete from leaking.
    if '_ZnwmPv' in candidates or '_ZdlPvS_' in candidates:
        print('Regression: C++ placement new/delete (_ZnwmPv / _ZdlPvS_) leaked into public ABI.')
        return 1
    if leaked:
        print(f"{library.name} exports {len(leaked)} symbol(s) outside its public API:")
        for name in leaked:
            print(f"  {name}")
        print("Build the defining target with vmaf_cflags_common / vmaf_cppflags_common,")
        print("or declare the symbol VMAF_EXPORT in a public header if it is API.")
        return 1
    print(f"{library.name}: every export is public API or a runtime's own")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
