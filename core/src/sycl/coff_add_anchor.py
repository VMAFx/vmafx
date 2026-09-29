#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Give the MSVC SYCL device-link object an external symbol a program can pull.

On Windows, Meson links every target with link.exe directly, never through the
icx/icpx driver, so nothing wraps and registers the SYCL device images at link
time (see ADR-1364). core/src/meson.build therefore runs the driver's explicit
device link (``icpx -fsycl -fsycl-link``) over every SYCL object of libvmaf.
Its output is one COFF object holding the device images and a CRT initializer
that registers them with the SYCL runtime, and nothing else: every symbol in it
is static. link.exe pulls an object out of a static library only to resolve an
external symbol, so inside ``vmaf.lib`` that object would never be linked.

This script appends one external symbol to the object, placed on the
``.sycl_offloading.descriptor`` the wrapper emits. ``sycl/common.cpp`` asks for
it with ``#pragma comment(linker, "/include:<symbol>")``, so any program that
uses the SYCL backend also links the registration. Relocations index symbols by
position and appending leaves every existing index unchanged.

Usage: coff_add_anchor.py --symbol NAME INPUT OUTPUT
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

DESCRIPTOR = b".sycl_offloading.descriptor"
IMAGE_SYM_CLASS_EXTERNAL = 2
BIGOBJ_CLASS_ID = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")
MAX_OBJECT_BYTES = 1 << 31
# PE/COFF layout (Microsoft PE format spec, "COFF File Header" and "COFF
# Symbol Table"; the /bigobj anonymous header is clang's and MSVC's).
COFF_HEADER_BYTES = 20
BIGOBJ_HEADER_BYTES = 56
BIGOBJ_SIG2 = 0xFFFF
BIGOBJ_MIN_VERSION = 2
STRTAB_SIZE_BYTES = 4
SHORT_NAME_BYTES = 8


class CoffError(ValueError):
    """The input is not a COFF object this script can extend safely."""


class _Layout:
    """Offsets of the fields this script reads and rewrites."""

    def __init__(self, data: bytes) -> None:
        if len(data) < COFF_HEADER_BYTES:
            raise CoffError("file is too short for a COFF header")
        sig1, sig2, version = struct.unpack_from("<HHH", data, 0)
        self.bigobj = sig1 == 0 and sig2 == BIGOBJ_SIG2 and version >= BIGOBJ_MIN_VERSION
        if self.bigobj:
            if len(data) < BIGOBJ_HEADER_BYTES or data[12:28] != BIGOBJ_CLASS_ID:
                raise CoffError("unrecognised anonymous object header")
            self.count_offset = 52
            self.symtab, self.nsyms = struct.unpack_from("<II", data, 48)
            self.record = 20
            self.section_format = "<i"
        else:
            self.count_offset = 12
            self.symtab, self.nsyms = struct.unpack_from("<II", data, 8)
            self.record = 18
            self.section_format = "<h"
        self.strtab = self.symtab + self.nsyms * self.record
        if self.symtab == 0 or self.strtab + STRTAB_SIZE_BYTES > len(data):
            raise CoffError("object has no symbol table")
        (self.strtab_size,) = struct.unpack_from("<I", data, self.strtab)
        if self.strtab_size < STRTAB_SIZE_BYTES or self.strtab + self.strtab_size != len(data):
            raise CoffError("string table does not end the file")


def _symbol_name(data: bytes, layout: _Layout, offset: int) -> bytes:
    raw = data[offset : offset + 8]
    if raw[:4] == b"\0\0\0\0":
        (string_offset,) = struct.unpack_from("<I", raw, 4)
        start = layout.strtab + string_offset
        return data[start : data.index(b"\0", start)]
    return raw.rstrip(b"\0")


def _find_symbols(data: bytes, layout: _Layout, anchor: bytes) -> tuple[bytes, int]:
    """Return the descriptor's (section number bytes, value), rejecting a second anchor."""
    placement = None
    index = 0
    while index < layout.nsyms:
        offset = layout.symtab + index * layout.record
        name = _symbol_name(data, layout, offset)
        if name == anchor:
            raise CoffError(f"{anchor.decode()} is already defined")
        if name == DESCRIPTOR and placement is None:
            (value,) = struct.unpack_from("<I", data, offset + 8)
            section = data[offset + 12 : offset + layout.record - 4]
            placement = (section, value)
        index += 1 + data[offset + layout.record - 1]
    if placement is None:
        raise CoffError(f"no {DESCRIPTOR.decode()} symbol: not a SYCL device-link object")
    return placement


def add_anchor(data: bytes, symbol: str) -> bytes:
    """Return ``data`` with an external ``symbol`` on the SYCL image descriptor."""
    anchor = symbol.encode("ascii")
    if not anchor or b"\0" in anchor or len(anchor) <= SHORT_NAME_BYTES:
        # Short names live inline; the fixed layout below only writes long ones.
        raise CoffError("anchor name must be 9 or more ASCII characters")
    layout = _Layout(data)
    section, value = _find_symbols(data, layout, anchor)
    record = b"\0\0\0\0" + struct.pack("<II", layout.strtab_size, value) + section
    record += struct.pack("<HBB", 0, IMAGE_SYM_CLASS_EXTERNAL, 0)
    strings = data[layout.strtab + 4 :] + anchor + b"\0"
    header = bytearray(data[: layout.strtab])
    struct.pack_into("<I", header, layout.count_offset, layout.nsyms + 1)
    return bytes(header) + record + struct.pack("<I", 4 + len(strings)) + strings


def main(argv: list[str] | None = None) -> int:
    """Command-line entry point used by core/src/meson.build."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--symbol", required=True)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.input.stat().st_size > MAX_OBJECT_BYTES:
            raise CoffError("object is larger than 2 GiB")
        patched = add_anchor(args.input.read_bytes(), args.symbol)
    except (OSError, CoffError, struct.error) as exc:
        print(f"coff_add_anchor.py: {args.input}: {exc}", file=sys.stderr)
        return 1
    args.output.write_bytes(patched)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
