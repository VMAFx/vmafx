#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check what every program of the Windows tester zip loads (ADR-1515).

    check-windows-bundle-imports.py <bundle-dir> --machine x64|arm64
        [--runtime mt|md] [--loaded-at-run-time NAME[,NAME...]]

Reads the import and delay-import tables of every `.exe`, `.dll` and `.pyd` under the
unpacked bundle (no tool needed: the PE headers are parsed here) and fails when

- a file is not a PE image of the bundle's architecture;
- with `--runtime mt` (the CPU and CUDA zips): a VMAFx program (`build/`, `tests/`)
  imports a C or C++ runtime DLL (`vcruntime*`, `msvcp*`, `ucrtbase`,
  `api-ms-win-crt-*`, ...): the zip links the runtime statically (`/MT`, ADR-1503
  rule 7) and ships none for them, or any DLL that is not part of Windows;
- with `--runtime md` (the SYCL zip, ADR-1566: `-fsycl` requires `/MD`): a file under
  `build/` or `tests/` imports a DLL that is neither part of Windows (the Universal CRT
  included) nor present in its own directory, the one directory Windows searches
  before System32; or a DLL there is imported by nothing in its directory and is not
  one of the `--loaded-at-run-time` names (ADR-1503 rule 1: ship only what runs);
- the interpreter (`runtime/`) imports a DLL that is neither part of Windows (the
  Universal CRT included) nor present in `runtime/`.

The Windows counterpart of scripts/ci/check-macos-bundle-links.sh. Standard library only.
Exit 0 clean, 1 a finding, 2 usage error.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import Any

MACHINES = {"x64": 0x8664, "arm64": 0xAA64}
DOS_HEADER_SIZE = 0x40
PE32_MAGIC, PE32_PLUS_MAGIC = 0x10B, 0x20B
MAX_SECTIONS = 96
MAX_DESCRIPTORS = 4096
MAX_NAME = 260
SUFFIXES = (".exe", ".dll", ".pyd")
# C and C++ runtime DLLs a /MT build must not import.
RUNTIME_PREFIXES = ("vcruntime", "msvcp", "ucrtbase", "api-ms-win-crt-", "concrt", "vcomp",
                    "vccorlib", "msvcr")  # fmt: skip
# DLLs of Windows itself (System32), which every Windows 10 and later carries. API sets
# (api-ms-win-*, ext-ms-win-*) resolve inside Windows and are accepted by prefix.
# cfgmgr32.dll (the Configuration Manager, CM_Get_Device_IDW and others) is System32
# since Windows 2000: Microsoft Learn, "CM_Get_Device_IDW function (cfgmgr32.h)",
# Requirements: Library Cfgmgr32.lib, DLL CfgMgr32.dll. The Level Zero loader of the
# SYCL zip imports it (run 37594655522).
SYSTEM_DLLS = frozenset(
    {
        "advapi32.dll", "bcrypt.dll", "bcryptprimitives.dll", "cfgmgr32.dll", "comctl32.dll",
        "comdlg32.dll", "crypt32.dll", "dbghelp.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll",
        "kernel32.dll", "kernelbase.dll", "mpr.dll", "msvcrt.dll", "mswsock.dll",
        "ncrypt.dll", "netapi32.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll", "powrprof.dll",
        "propsys.dll", "psapi.dll", "rpcrt4.dll", "secur32.dll", "setupapi.dll",
        "shell32.dll", "shlwapi.dll", "user32.dll", "userenv.dll", "uxtheme.dll",
        "version.dll", "winmm.dll", "ws2_32.dll", "wsock32.dll",
    }
)  # fmt: skip
SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-win-")
# The Universal CRT is part of Windows 10 and later; a /MD program imports it directly.
UNIVERSAL_CRT = "ucrtbase.dll"


Sections = list[tuple[int, int, int]]  # (virtual address, size, file offset)


class PeError(ValueError):
    """The file is not a PE image this checker can read."""


def u16(data: bytes, offset: int) -> int:
    value: int = struct.unpack_from("<H", data, offset)[0]
    return value


def u32(data: bytes, offset: int) -> int:
    value: int = struct.unpack_from("<I", data, offset)[0]
    return value


def pe_header(data: bytes) -> int:
    """Offset of the PE signature."""
    if len(data) < DOS_HEADER_SIZE or data[:2] != b"MZ":
        raise PeError("no MZ header")
    offset = u32(data, 0x3C)
    if data[offset : offset + 4] != b"PE\0\0":
        raise PeError("no PE signature")
    return offset


def sections(data: bytes, pe: int) -> Sections:
    """(virtual address, size, file offset) of every section."""
    count, optional_size = u16(data, pe + 6), u16(data, pe + 20)
    if count > MAX_SECTIONS:
        raise PeError(f"{count} sections")
    table = pe + 24 + optional_size
    found = []
    for index in range(count):
        base = table + 40 * index
        virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", data, base + 8)
        found.append((address, max(virtual_size, raw_size), raw))
    return found


def data_directory(data: bytes, pe: int, index: int) -> tuple[int, int]:
    """(RVA, size) of one data directory of the optional header."""
    optional = pe + 24
    magic = u16(data, optional)
    if magic not in (PE32_MAGIC, PE32_PLUS_MAGIC):
        raise PeError(f"optional header magic {magic:#x}")
    count_at, first = (92, 96) if magic == PE32_MAGIC else (108, 112)
    if index >= u32(data, optional + count_at):
        return 0, 0
    return u32(data, optional + first + 8 * index), u32(data, optional + first + 8 * index + 4)


def file_offset(table: Sections, rva: int) -> int:
    for address, size, raw in table:
        if address <= rva < address + size:
            return raw + rva - address
    raise PeError(f"RVA {rva:#x} is in no section")


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset, offset + MAX_NAME)
    if end < 0:
        raise PeError("unterminated name")
    return data[offset:end].decode("ascii", errors="replace")


def descriptor_names(
    data: bytes, start: int, layout: tuple[int, int], table: Sections
) -> list[str]:
    """DLL names of a zero-terminated descriptor array (`layout`: entry size, offset of
    the name RVA in an entry)."""
    size, name_at = layout
    names: list[str] = []
    for index in range(MAX_DESCRIPTORS):
        entry = data[start + size * index : start + size * (index + 1)]
        if len(entry) < size or not any(entry):
            return names
        names.append(c_string(data, file_offset(table, u32(entry, name_at))))
    raise PeError("descriptor table without terminator")


def imports(data: bytes) -> tuple[int, list[str]]:
    """(machine, imported and delay-loaded DLL names in lower case) of a PE image."""
    pe = pe_header(data)
    table = sections(data, pe)
    names: list[str] = []
    for index, layout in ((1, (20, 12)), (13, (32, 4))):  # imports, delay imports
        rva, size = data_directory(data, pe, index)
        if rva and size:
            names += descriptor_names(data, file_offset(table, rva), layout, table)
    return u16(data, pe + 4), sorted({name.lower() for name in names})


def is_system(name: str) -> bool:
    return name in SYSTEM_DLLS or name.startswith(SYSTEM_PREFIXES)


def is_runtime(name: str) -> bool:
    return name.startswith(RUNTIME_PREFIXES)


def md_problems(rel: str, names: list[str], beside: set[str]) -> list[str]:
    """Findings for a file of a /MD bundle: every DLL it imports is part of Windows or
    lies in its own directory."""
    return [
        f"{rel}: imports {name}, neither part of Windows nor in its directory"
        for name in names
        if not (is_system(name) or name == UNIVERSAL_CRT or name in beside)
    ]


def problems_of(rel: str, machine: int, names: list[str], context: dict[str, Any]) -> list[str]:
    """Findings for one PE file of the bundle."""
    found = []
    if machine != context["machine"]:
        found.append(f"{rel}: machine {machine:#06x}, expected {context['machine']:#06x}")
    interpreter = rel.startswith("runtime/")
    if not interpreter and context["runtime"] == "md":
        directory = rel.rsplit("/", 1)[0]
        return found + md_problems(rel, names, context["by_directory"].get(directory, set()))
    for name in names:
        if not interpreter and is_runtime(name):
            found.append(f"{rel}: imports the runtime DLL {name} (the zip links /MT)")
        elif not interpreter and not is_system(name):
            found.append(f"{rel}: imports {name}, which is not part of Windows")
        elif interpreter and not (is_system(name) or name in context["shipped"]):
            found.append(f"{rel}: imports {name}, neither part of Windows nor in runtime/")
    return found


def unused_dlls(imported: dict[str, set[str]], files: list[str], loaded: set[str]) -> list[str]:
    """DLLs under build/ or tests/ that nothing in their directory imports and that are
    not loaded at run time by name (the SYCL runtime's loader and adapters)."""
    found = []
    for rel in files:
        directory, name = rel.rsplit("/", 1)
        lower = name.lower()
        if lower.endswith(".dll") and lower not in imported.get(directory, set()) | loaded:
            found.append(
                f"{rel}: no file in its directory imports it, and it is not loaded at run time"
            )
    return found


def check(
    bundle: Path, machine: str, runtime: str = "mt", loaded: tuple[str, ...] = ()
) -> list[str]:
    files = sorted(p for p in bundle.rglob("*") if p.is_file() and p.suffix.lower() in SUFFIXES)
    shipped = {p.name.lower() for p in files if p.relative_to(bundle).parts[0] == "runtime"}
    by_directory: dict[str, set[str]] = {}
    for path in files:
        rel = path.relative_to(bundle).as_posix()
        if "/" in rel:
            by_directory.setdefault(rel.rsplit("/", 1)[0], set()).add(path.name.lower())
    context = {"machine": MACHINES[machine], "shipped": shipped, "runtime": runtime,
               "by_directory": by_directory}  # fmt: skip
    problems: list[str] = []
    programs = 0
    imported: dict[str, set[str]] = {}
    beside: list[str] = []
    for path in files:
        rel = path.relative_to(bundle).as_posix()
        try:
            found_machine, names = imports(path.read_bytes())
        except (PeError, struct.error) as error:
            problems.append(f"{rel}: not a readable PE image ({error})")
            continue
        vmafx = rel.startswith(("build/", "tests/"))
        programs += vmafx and rel.lower().endswith(".exe")
        if vmafx:
            imported.setdefault(rel.rsplit("/", 1)[0], set()).update(names)
            beside.append(rel)
        problems += problems_of(rel, found_machine, names, context)
    if runtime == "md":
        problems += unused_dlls(imported, beside, {name.lower() for name in loaded})
    if programs == 0:
        problems.append("no VMAFx program under build/ or tests/")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--machine", required=True, choices=sorted(MACHINES))
    parser.add_argument("--runtime", default="mt", choices=("md", "mt"))
    parser.add_argument("--loaded-at-run-time", default="", metavar="NAMES")
    args = parser.parse_args(argv)
    if not args.bundle.is_dir():
        print(f"check-windows-bundle-imports: {args.bundle} is not a directory", file=sys.stderr)
        return 2
    loaded = tuple(name for name in args.loaded_at_run_time.split(",") if name)
    problems = check(args.bundle, args.machine, args.runtime, loaded)
    for problem in problems:
        print(f"check-windows-bundle-imports: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("check-windows-bundle-imports: every program loads Windows DLLs or the bundle's own")
    return 0


if __name__ == "__main__":
    sys.exit(main())
