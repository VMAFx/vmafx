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

# Mangled names inside namespace std: nested names (_ZNSt, _ZNKSt), typeinfo,
# typeinfo names and vtables (_ZTISt, _ZTSSt, _ZTVSt) and free functions (_ZSt).
STD_NAMESPACE = re.compile(r"^_Z(?:N|NK)?St|^_ZT[ISV]St")
# Anything whose mangled name involves DPC++'s versioned sycl namespace
# (sycl::_V1 mangles as 4sycl3_V1): its members, and typeinfo for types built
# from them, such as the async-handler function type.
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


def public_identifiers(include_dir: Path) -> set[str]:
    names: set[str] = set()
    for header in include_dir.rglob("*.h"):
        names.update(re.findall(r"\bvmaf_\w+", header.read_text(errors="replace")))
    return names


def main() -> int:
    library, include_dir = Path(sys.argv[1]), Path(sys.argv[2])
    public = public_identifiers(include_dir)
    leaked = [
        name
        for name in exported_symbols(library)
        if not (name in public or STD_NAMESPACE.match(name) or SYCL_RUNTIME.match(name))
    ]
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
