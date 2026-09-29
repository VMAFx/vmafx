#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for the SYCL device-link anchor (ADR-1364).

core/src/sycl/coff_add_anchor.py appends an external symbol to the COFF object
``icpx -fsycl -fsycl-link`` produces on Windows, so link.exe pulls it out of
vmaf.lib. The fixtures are synthetic COFF objects, so the contract runs on any
host, with or without a Windows toolchain.
"""

from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

PATCHER = Path(__file__).resolve().parents[1] / "src" / "sycl" / "coff_add_anchor.py"
SPEC = importlib.util.spec_from_file_location("coff_add_anchor", PATCHER)
assert SPEC is not None and SPEC.loader is not None
coff_add_anchor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = coff_add_anchor
SPEC.loader.exec_module(coff_add_anchor)

ANCHOR = "vmaf_sycl_device_images"
STATIC = 3


def coff(symbols: list[tuple[str, int, int, int, int]], *, bigobj: bool = False) -> bytes:
    """A COFF object with one section and ``(name, value, section, class, aux)`` symbols."""
    record = 20 if bigobj else 18
    header_size = 56 if bigobj else 20
    raw = b"\xcc" * 16
    strings = bytearray()
    table = bytearray()
    for name, value, section, storage, aux in symbols:
        encoded = name.encode()
        if len(encoded) <= coff_add_anchor.SHORT_NAME_BYTES:
            field = encoded.ljust(8, b"\0")
        else:
            field = b"\0\0\0\0" + struct.pack("<I", 4 + len(strings))
            strings += encoded + b"\0"
        packed_section = struct.pack("<i" if bigobj else "<h", section)
        table += field + struct.pack("<I", value) + packed_section
        table += struct.pack("<HBB", 0x20, storage, aux)
        table += b"\0" * (record * aux)
    nsyms = len(table) // record
    section_header = b".rdata\0\0" + struct.pack(
        "<IIIIIIHHI", 0, 0, 16, header_size + 40, 0, 0, 0, 0, 0x40000040
    )
    symtab = header_size + len(section_header) + len(raw)
    if bigobj:
        header = struct.pack("<HHHHI", 0, 0xFFFF, 2, 0x8664, 0) + coff_add_anchor.BIGOBJ_CLASS_ID
        header += struct.pack("<IIIIIII", 0, 0, 0, 0, 1, symtab, nsyms)
    else:
        header = struct.pack("<HHIIIHH", 0x8664, 1, 0, symtab, nsyms, 0, 0)
    return (
        header
        + section_header
        + raw
        + bytes(table)
        + struct.pack("<I", 4 + len(strings))
        + bytes(strings)
    )


def symbols_of(data: bytes) -> list[tuple[bytes, int, int, int]]:
    """Parse ``(name, value, section, class)`` for every primary symbol record."""
    layout = coff_add_anchor._Layout(data)
    parsed = []
    index = 0
    while index < layout.nsyms:
        offset = layout.symtab + index * layout.record
        name = coff_add_anchor._symbol_name(data, layout, offset)
        (value,) = struct.unpack_from("<I", data, offset + 8)
        (section,) = struct.unpack_from(layout.section_format, data, offset + 12)
        storage = data[offset + layout.record - 2]
        parsed.append((name, value, section, storage))
        index += 1 + data[offset + layout.record - 1]
    return parsed


WRAPPER = [
    (".text", 0, 1, STATIC, 1),
    ("sycl.descriptor_reg", 0, 1, STATIC, 0),
    (".sycl_offloading.descriptor", 0x1680, 1, STATIC, 0),
    ("prop", 0x10, 1, STATIC, 0),
]


class CoffAnchorTest(unittest.TestCase):
    def test_anchor_is_external_on_the_descriptor(self) -> None:
        for bigobj in (False, True):
            with self.subTest(bigobj=bigobj):
                original = coff(WRAPPER, bigobj=bigobj)
                patched = coff_add_anchor.add_anchor(original, ANCHOR)
                parsed = symbols_of(patched)
                self.assertEqual(parsed[:-1], symbols_of(original))
                self.assertEqual(parsed[-1], (ANCHOR.encode(), 0x1680, 1, 2))

    def test_existing_records_and_names_keep_their_bytes(self) -> None:
        original = coff(WRAPPER)
        patched = coff_add_anchor.add_anchor(original, ANCHOR)
        layout = coff_add_anchor._Layout(original)
        # Only the symbol count changes before the old string table offset.
        self.assertEqual(patched[:12], original[:12])
        self.assertEqual(patched[16 : layout.strtab], original[16 : layout.strtab])
        old_strings = original[layout.strtab + 4 :]
        new_layout = coff_add_anchor._Layout(patched)
        self.assertTrue(patched[new_layout.strtab + 4 :].startswith(old_strings))

    def test_rejects_an_object_without_a_sycl_descriptor(self) -> None:
        with self.assertRaisesRegex(coff_add_anchor.CoffError, "not a SYCL device-link object"):
            coff_add_anchor.add_anchor(coff(WRAPPER[:2]), ANCHOR)

    def test_rejects_a_second_anchor(self) -> None:
        once = coff_add_anchor.add_anchor(coff(WRAPPER), ANCHOR)
        with self.assertRaisesRegex(coff_add_anchor.CoffError, "already defined"):
            coff_add_anchor.add_anchor(once, ANCHOR)

    def test_rejects_trailing_data_after_the_string_table(self) -> None:
        with self.assertRaisesRegex(coff_add_anchor.CoffError, "does not end the file"):
            coff_add_anchor.add_anchor(coff(WRAPPER) + b"\0", ANCHOR)

    def test_rejects_short_or_empty_names_and_truncated_input(self) -> None:
        for name in ("", "anchor"):
            with self.assertRaisesRegex(coff_add_anchor.CoffError, "9 or more"):
                coff_add_anchor.add_anchor(coff(WRAPPER), name)
        with self.assertRaisesRegex(coff_add_anchor.CoffError, "too short"):
            coff_add_anchor.add_anchor(b"\0" * 10, ANCHOR)

    def test_aux_records_are_skipped_not_parsed_as_symbols(self) -> None:
        # An aux record whose bytes spell the descriptor name must not match.
        decoy = coff([(".text", 0, 1, STATIC, 1)])
        layout = coff_add_anchor._Layout(decoy)
        aux = layout.symtab + layout.record
        forged = bytearray(decoy)
        forged[aux : aux + 8] = b".sycl_of"
        with self.assertRaisesRegex(coff_add_anchor.CoffError, "not a SYCL device-link object"):
            coff_add_anchor.add_anchor(bytes(forged), ANCHOR)

    def test_command_line_writes_output_or_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "raw.obj"
            target = Path(directory) / "anchored.obj"
            source.write_bytes(coff(WRAPPER))
            self.assertEqual(
                coff_add_anchor.main(["--symbol", ANCHOR, str(source), str(target)]), 0
            )
            self.assertEqual(symbols_of(target.read_bytes())[-1][0], ANCHOR.encode())
            source.write_bytes(coff(WRAPPER[:2]))
            target.unlink()
            self.assertEqual(
                coff_add_anchor.main(["--symbol", ANCHOR, str(source), str(target)]), 1
            )
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
