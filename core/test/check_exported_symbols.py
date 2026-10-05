#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail when libvmaf.so exports anything but its public API (ADR-0379, ADR-1852).

Every exported symbol must be a ``vmaf_*`` function or object declared in a
public header under ``core/include/``, a ``vmafx_*`` function of the symbol list
generated from ``core/api/vmafx.toml`` (``core/src/vmafx_symbols.txt``), or
belong to a C++ runtime whose headers declare its namespace with default
visibility: ``std`` (libstdc++) and, in SYCL builds, ``sycl`` (DPC++). Template
members of those namespaces that a TU instantiates are exported whatever the
compile flags, and they are the runtime's definitions, not ours.

Anything else is an internal symbol leaking out of the shared object. It can be
interposed by a host application defining the same name, and consumers can
start depending on it. The library builds with ``-fvisibility=hidden``; this
test is what notices when a target stops getting that flag.

The VMAFx symbol list is exact in both directions (ADR-1852): a ``vmafx_``
export missing from it, a listed symbol the library does not export, and an
export in another linker version node than the list names (the version script
``core/src/vmafx.map`` assigns them) all fail. The version-node comparison
needs ``nm`` to print symbol versions; the C library's own versioned imports
show whether it does, and the check says so when it cannot compare.

Usage: check_exported_symbols.py <libvmaf.so> <public include dir> <vmafx symbol list>
"""

import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
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


UNDEFINED_TYPES = {"U", "w", "v"}
NM_TIMEOUT = 120  # seconds; HISS-02 bounds every external call


@dataclass(frozen=True)
class DynamicSymbols:
    """Defined exports (name -> version node or None) and whether nm prints versions."""

    exports: dict[str, str | None]
    versions_visible: bool


def split_version(token: str) -> tuple[str, str | None]:
    """`name@@NODE` / `name@NODE` -> (name, NODE); a plain name -> (name, None)."""
    name, _, node = token.replace("@@", "@", 1).partition("@")
    return name, node or None


def parse_nm(text: str) -> DynamicSymbols:
    """Read `nm -D` output. Version-definition symbols (`A VMAFX_0.1`) are not exports."""
    exports: dict[str, str | None] = {}
    absolute: set[str] = set()
    visible = False
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2:  # noqa: PLR2004 -- "<type> <name>" at least
            continue
        kind, (name, node) = parts[-2], split_version(parts[-1])
        if kind in UNDEFINED_TYPES:
            visible = visible or node is not None
            continue
        if kind == "A":
            absolute.add(name)
        exports[name] = node
    nodes = {node for node in exports.values() if node is not None}
    for name in absolute & nodes:
        del exports[name]
    return DynamicSymbols(exports=exports, versions_visible=visible)


def exported_symbols(library: Path) -> DynamicSymbols:
    nm = shutil.which("nm")
    if nm is None:
        print("SKIP: nm not found")
        raise SystemExit(77)
    out = subprocess.run(  # noqa: S603 -- resolved nm path and the library path, no shell
        [nm, "-D", str(library)], check=True, capture_output=True, text=True, timeout=NM_TIMEOUT
    ).stdout
    return parse_nm(out)


def listed_symbols_from_text(text: str) -> dict[str, str]:
    """`<symbol> <node>` rows of the generated VMAFx symbol list; `#` starts a comment."""
    rows: dict[str, str] = {}
    for line in text.splitlines():
        fields = line.split("#", 1)[0].split()
        if fields:
            name, node = fields
            rows[name] = node
    return rows


def listed_symbols(path: Path) -> dict[str, str]:
    return listed_symbols_from_text(path.read_text(encoding="utf-8"))


def vmafx_findings(symbols: DynamicSymbols, listed: dict[str, str]) -> list[str]:
    """Differences between the library's vmafx_ exports and the generated list."""
    exported = {n: v for n, v in symbols.exports.items() if n.startswith("vmafx_")}
    out = [
        f"{n}: exported, missing from the VMAFx symbol list"
        for n in sorted(exported.keys() - listed.keys())
    ]
    out += [
        f"{n}: listed ({listed[n]}), not exported" for n in sorted(listed.keys() - exported.keys())
    ]
    if symbols.versions_visible:
        for name in sorted(exported.keys() & listed.keys()):
            if exported[name] != listed[name]:
                node = exported[name] or "no version node"
                out.append(f"{name}: exported in {node}, the list names {listed[name]}")
    return out


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
    """vmaf_ names the public headers mention; vmafx_ names come from the symbol list."""
    names: set[str] = set()
    for header in include_dir.rglob("*.h"):
        names.update(re.findall(r"\bvmaf_\w+", header.read_text(errors="replace")))
    return names


def leaked_symbols(symbols: DynamicSymbols, public: set[str]) -> list[str]:
    """Exports that are neither public API nor a runtime's own."""
    candidates = [
        name
        for name in sorted(symbols.exports)
        if not name.startswith("vmafx_") and COMPILER_VARIANT_SUFFIX.sub("", name) not in public
    ]
    plain = demangled(candidates)
    return [name for name in candidates if not runtime_owned(name, plain[name])]


def report(library: Path, leaked: list[str], vmafx: list[str], symbols: DynamicSymbols) -> int:
    if leaked:
        print(f"{library.name} exports {len(leaked)} symbol(s) outside its public API:")
        for name in leaked:
            print(f"  {name}")
        print("Build the defining target with vmaf_cflags_common / vmaf_cppflags_common,")
        print("or declare the symbol VMAF_EXPORT in a public header if it is API.")
    if vmafx:
        print(f"{library.name} disagrees with the VMAFx symbol list in {len(vmafx)} place(s):")
        for finding in vmafx:
            print(f"  {finding}")
        print("Regenerate with `python3 scripts/codegen/vmafx-api.py --write` and relink;")
        print("a vmafx_ function exists only through core/api/vmafx.toml (ADR-1852).")
    if leaked or vmafx:
        return 1
    if not symbols.versions_visible:
        print("NOTE: nm prints no symbol versions here; version nodes were not compared")
    print(f"{library.name}: every export is public API or a runtime's own")
    return 0


def main() -> int:
    library, include_dir, symbol_list = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    symbols = exported_symbols(library)
    # Red-cap regression check: ADR-1337 prevents C++ placement new/delete from leaking.
    if "_ZnwmPv" in symbols.exports or "_ZdlPvS_" in symbols.exports:
        print("Regression: C++ placement new/delete (_ZnwmPv / _ZdlPvS_) leaked into public ABI.")
        return 1
    leaked = leaked_symbols(symbols, public_identifiers(include_dir))
    vmafx = vmafx_findings(symbols, listed_symbols(symbol_list))
    return report(library, leaked, vmafx, symbols)


if __name__ == "__main__":
    raise SystemExit(main())
