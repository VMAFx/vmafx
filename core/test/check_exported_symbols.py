#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail when a library exports anything but its public API (ADR-0379, ADR-1852).

The engine library libvmafx.so.1 (ADR-1852 decision D3, RC4 WP6) exports the
``vmafx_*`` functions of the generated symbol list (``core/src/vmafx_symbols.txt``),
each in the linker version node the list names, plus the libvmaf functions a
built backend still keeps in the engine (declared exceptions of
``core/api/vmafx.toml``, version node ``VMAF_LEGACY_<BACKEND>``). The compat
library libvmaf.so.3 exports exactly the libvmaf functions the generated
compat list (``core/src/libvmaf_symbols.txt``) gives it for this build:
unversioned, and no ``vmafx_`` symbol. Both lists are exact in both
directions: an export missing from the list, a listed function the library
does not export, and an export in the wrong version node all fail.

Symbols of a C++ runtime whose headers declare its namespace with default
visibility are allowed: ``std`` (libstdc++) and, in SYCL builds, ``sycl``
(DPC++). Template members of those namespaces that a TU instantiates are
exported whatever the compile flags, and they are the runtime's definitions,
not ours. Anything else is an internal symbol leaking out of the shared
object: a host application defining the same name can interpose it, and
consumers can start depending on it. The libraries build with
``-fvisibility=hidden`` and libvmafx with a version script whose last word is
``local: *;``; this test is what notices when that stops holding.

The version-node comparison needs ``nm`` to print symbol versions; any
versioned symbol, import or export, shows that it does, and the check says so
when it cannot compare.

Usage:
  check_exported_symbols.py --library vmafx|vmaf --vmafx-symbols <list>
      --compat-symbols <list> [--backends cuda,sycl,hip,metal] [--features mcp] <library>
"""

import argparse
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
        # Any `name@NODE` shows nm prints versions: an import's or an export's.
        # A toolchain may link every import unversioned (`w __cxa_finalize`)
        # while nm prints the exports' nodes (T-VMAFX-SYMBOL-VERSIONS-UNSEEN-
        # UBUNTU-2026-10-06). A version-definition `A` symbol proves nothing:
        # an nm without version output lists it too.
        visible = visible or node is not None
        if kind in UNDEFINED_TYPES:
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


def on_platform(platform: str) -> bool:
    """True when this check runs on the operating system a compat row is limited to."""
    return platform == "windows" and sys.platform.startswith(("win", "cygwin", "msys"))


def compat_rows_from_text(text: str) -> dict[str, str]:
    """`<symbol> <library>[:<condition>]` rows of the generated compat list."""
    return listed_symbols_from_text(text)


def expected_exports(
    rows: dict[str, str], library: str, backends: set[str], features: set[str]
) -> dict[str, str | None]:
    """The libvmaf functions `library` exports in this build, with their version node.

    `compat`: libvmaf.so.3, unversioned. `compat:!B`: libvmaf.so.3 unless backend
    B is built, else libvmafx.so.1 in node VMAF_LEGACY_B. `compat:F`: libvmaf.so.3
    in builds with feature F. `engine:B`: libvmafx.so.1 in builds with backend B.
    A row ending in `@<os>` exists only in builds for that operating system.
    """
    out: dict[str, str | None] = {}
    for name, row in rows.items():
        where, _, platform = row.partition("@")
        if platform and not on_platform(platform):
            continue
        owner, _, condition = where.partition(":")
        backend = condition.removeprefix("!")
        if owner == "engine" or (condition.startswith("!") and backend in backends):
            if library == "vmafx" and backend in backends:
                out[name] = f"VMAF_LEGACY_{backend.upper()}"
            continue
        if library == "vmaf" and (
            not condition or condition.startswith("!") or condition in features
        ):
            out[name] = None
    return out


def libvmaf_findings(
    symbols: DynamicSymbols, expected: dict[str, str | None], library: str
) -> list[str]:
    """Differences between the library's vmaf_ exports and the compat list for this build."""
    exported = {
        COMPILER_VARIANT_SUFFIX.sub("", n): v
        for n, v in symbols.exports.items()
        if n.startswith("vmaf_")
    }
    where = "libvmaf.so.3" if library == "vmaf" else "libvmafx.so.1"
    out = [
        f"{n}: exported, but the compat list gives it to no {where} of this build"
        for n in sorted(exported.keys() - expected.keys())
    ]
    out += [
        f"{n}: the compat list gives it to {where}, not exported"
        for n in sorted(expected.keys() - exported.keys())
    ]
    if symbols.versions_visible:
        for name in sorted(exported.keys() & expected.keys()):
            if exported[name] != expected[name]:
                got = exported[name] or "no version node"
                want = expected[name] or "no version node"
                out.append(f"{name}: exported in {got}, expected {want}")
    return out


def leaked_symbols(symbols: DynamicSymbols) -> list[str]:
    """Exports that are neither API (vmaf_ / vmafx_, judged by the lists) nor a runtime's own."""
    candidates = [
        name for name in sorted(symbols.exports) if not name.startswith(("vmaf_", "vmafx_"))
    ]
    plain = demangled(candidates)
    return [name for name in candidates if not runtime_owned(name, plain[name])]


def report(library: Path, leaked: list[str], findings: list[str], symbols: DynamicSymbols) -> int:
    if leaked:
        print(f"{library.name} exports {len(leaked)} symbol(s) outside its public API:")
        for name in leaked:
            print(f"  {name}")
        print("Build the defining target with vmaf_cflags_common / vmaf_cppflags_common,")
        print("or declare the function in core/api/vmafx.toml if it is API.")
    if findings:
        print(
            f"{library.name} disagrees with the generated symbol lists in {len(findings)} place(s):"
        )
        for finding in findings:
            print(f"  {finding}")
        print("Regenerate with `python3 scripts/codegen/vmafx-api.py --write` and relink;")
        print("an exported function exists only through core/api/vmafx.toml (ADR-1852).")
    if leaked or findings:
        return 1
    if not symbols.versions_visible:
        print("NOTE: nm prints no symbol versions here; version nodes were not compared")
    print(f"{library.name}: every export is public API or a runtime's own")
    return 0


def arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--library", choices=("vmafx", "vmaf"), required=True)
    parser.add_argument("--vmafx-symbols", type=Path, required=True)
    parser.add_argument("--compat-symbols", type=Path, required=True)
    parser.add_argument("--backends", default="")
    parser.add_argument("--features", default="")
    parser.add_argument("path", type=Path)
    return parser.parse_args(argv)


def findings_of(args: argparse.Namespace, symbols: DynamicSymbols) -> list[str]:
    backends = {b for b in args.backends.split(",") if b}
    features = {f for f in args.features.split(",") if f}
    rows = compat_rows_from_text(args.compat_symbols.read_text(encoding="utf-8"))
    expected = expected_exports(rows, args.library, backends, features)
    findings = libvmaf_findings(symbols, expected, args.library)
    if args.library == "vmafx":
        return vmafx_findings(symbols, listed_symbols(args.vmafx_symbols)) + findings
    stray = sorted(n for n in symbols.exports if n.startswith("vmafx_"))
    return [
        f"{n}: libvmaf.so.3 exports a vmafx_ symbol (it belongs to libvmafx.so.1)" for n in stray
    ] + findings


def main(argv: list[str] | None = None) -> int:
    args = arguments(sys.argv[1:] if argv is None else argv)
    symbols = exported_symbols(args.path)
    # Red-cap regression check: ADR-1337 prevents C++ placement new/delete from leaking.
    if "_ZnwmPv" in symbols.exports or "_ZdlPvS_" in symbols.exports:
        print("Regression: C++ placement new/delete (_ZnwmPv / _ZdlPvS_) leaked into public ABI.")
        return 1
    return report(args.path, leaked_symbols(symbols), findings_of(args, symbols), symbols)


if __name__ == "__main__":
    raise SystemExit(main())
