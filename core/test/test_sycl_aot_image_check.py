#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for the SYCL AOT image check (ADR-1360).

core/src/sycl/check_aot_image.py fails the build when libvmaf.so lacks the
spir64_gen images sycl_icpx_aot_targets asked for. These fixtures are
synthetic ELF64 files, so the contract runs on any host, with or without
oneAPI; `ocloc ids` is replaced by a fixed acronym-to-IP table.
"""

from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

CHECKER = Path(__file__).resolve().parents[1] / "src" / "sycl" / "check_aot_image.py"
SPEC = importlib.util.spec_from_file_location("check_aot_image", CHECKER)
assert SPEC is not None and SPEC.loader is not None
check_aot_image = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check_aot_image
SPEC.loader.exec_module(check_aot_image)

# Real `ocloc ids` answers (NEO 26.35): dg2-g10 and acm-g10 are one IP.
IP = {"dg2-g10": "12.55.8", "acm-g10": "12.55.8", "adl-s": "12.2.0", "bmg-g21": "20.1.0"}
TARGETS = "dg2-g10,acm-g10,adl-s,bmg-g21"


def fat_binary(ips: list[str]) -> bytes:
    """An ocloc multi-device fat binary: an ar archive, one member per IP."""
    blob = bytearray(b"!<arch>\n")
    members = [(f"pad_{i}/", b"\0" * 8) for i in range(len(ips))]
    members += [(f"64.{ip}/", b"ISA" * 7) for ip in ips] + [("generic_ir/", b"SPIRV")]
    for name, body in members:
        header = f"{name:<16}{0:<12}{0:<6}{0:<6}{644:<8}{len(body):<10}`\n"
        blob += header.encode("ascii") + body + (b"\n" if len(body) % 2 else b"")
    return bytes(blob)


def elf64(sections: dict[str, bytes]) -> bytes:
    """A minimal ELF64 little-endian file holding the named sections."""
    names = b"\0.shstrtab\0" + b"".join(name.encode() + b"\0" for name in sections)
    payloads = [(".shstrtab", names), *sections.items()]
    body = bytearray(b"\0" * 64)
    placed = []
    for name, payload in payloads:
        placed.append((names.index(name.encode() + b"\0"), len(body), len(payload)))
        body += payload
    shoff = len(body)
    body += b"\0" * 64  # SHN_UNDEF
    for name_offset, offset, size in placed:
        body += struct.pack("<IIQQQQIIQQ", name_offset, 1, 0, 0, offset, size, 0, 0, 1, 0)
    header = b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9
    header += struct.pack(
        "<HHIQQQIHHHHHH", 3, 62, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(placed) + 1, 1
    )
    body[:64] = header
    return bytes(body)


class AotImageCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        patcher = mock.patch.object(check_aot_image, "ocloc_ip_version", self.fake_ocloc)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    @staticmethod
    def fake_ocloc(_ocloc: str, target: str) -> str:
        if target not in IP:
            raise check_aot_image.CheckError(f"unknown acronym {target}")
        return IP[target]

    def run_check(self, library: bytes, *extra: str, targets: str = TARGETS) -> int:
        path = self.root / "libvmaf.so"
        path.write_bytes(library)
        argv = [
            "--ocloc",
            "ocloc",
            "--targets",
            targets,
            *extra,
            "--stamp",
            str(self.root / "stamp"),
            str(path),
        ]
        return int(check_aot_image.main(argv))

    def gen_section(self, *archives: bytes) -> bytes:
        return elf64({check_aot_image.SECTION: b"".join(archives)})

    def test_every_image_covers_every_target(self) -> None:
        full = fat_binary(["12.55.8", "12.55.8", "12.2.0", "20.1.0"])
        self.assertEqual(self.run_check(self.gen_section(full, full, full)), 0)
        self.assertIn("3 spir64_gen fat binaries", (self.root / "stamp").read_text())

    def test_spirv_only_library_fails(self) -> None:
        library = elf64({"__CLANG_OFFLOAD_BUNDLE__sycl-spir64": b"SPIRV"})
        self.assertEqual(self.run_check(library), 1)
        self.assertFalse((self.root / "stamp").exists())

    def test_empty_gen_section_fails(self) -> None:
        self.assertEqual(self.run_check(self.gen_section(b"")), 1)

    def test_image_missing_a_target_fails(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        short = fat_binary(["12.55.8", "20.1.0"])
        self.assertEqual(self.run_check(self.gen_section(full, short)), 1)

    def test_declared_partial_unit_may_omit_only_its_targets(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        no_xe2 = fat_binary(["12.55.8", "12.2.0"])
        partial = ("--partial", "integer_psnr_hvs_sycl=bmg-g21")
        self.assertEqual(self.run_check(self.gen_section(full, no_xe2), *partial), 0)
        no_adl = fat_binary(["12.55.8", "20.1.0"])
        self.assertEqual(self.run_check(self.gen_section(full, no_adl), *partial), 1)

    def test_more_partial_images_than_declared_units_fails(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        no_xe2 = fat_binary(["12.55.8", "12.2.0"])
        partial = ("--partial", "integer_psnr_hvs_sycl=bmg-g21")
        self.assertEqual(self.run_check(self.gen_section(full, no_xe2, no_xe2), *partial), 1)

    def test_partial_entry_for_unrequested_target_is_ignored(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0"])
        partial = ("--partial", "integer_psnr_hvs_sycl=bmg-g21")
        self.assertEqual(
            self.run_check(self.gen_section(full, full), *partial, targets="dg2-g10,adl-s"), 0
        )

    def test_single_target_boundary(self) -> None:
        one = fat_binary(["20.1.0"])
        self.assertEqual(self.run_check(self.gen_section(one), targets="bmg-g21"), 0)
        self.assertEqual(self.run_check(self.gen_section(one), targets="adl-s"), 1)

    def test_unknown_target_fails(self) -> None:
        full = fat_binary(["12.55.8"])
        self.assertEqual(self.run_check(self.gen_section(full), targets="dg2-g10,xyz-q1"), 1)

    def test_malformed_partial_fails(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        self.assertEqual(self.run_check(self.gen_section(full), "--partial", "no-equals"), 1)

    def test_non_elf_input_fails(self) -> None:
        self.assertEqual(self.run_check(b"MZ\x90\x00 not an ELF file"), 1)

    def compressed(self, *archives: bytes) -> bytes:
        """zstd frames zero-padded to 16 bytes, the layout --offload-compress links."""
        try:
            from compression import zstd  # noqa: PLC0415 - Python >= 3.14 only
        except ImportError:
            self.skipTest("compression.zstd needs Python >= 3.14")
        blob = b""
        for archive in archives:
            blob += zstd.compress(archive)
            blob += b"\0" * (-len(blob) % 16)
        return blob

    def test_compressed_images_are_decoded(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        self.assertEqual(self.run_check(self.gen_section(self.compressed(full, full, full))), 0)
        self.assertIn("3 spir64_gen fat binaries", (self.root / "stamp").read_text())

    def test_compressed_image_missing_a_target_fails(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        short = fat_binary(["12.55.8", "12.2.0"])
        self.assertEqual(self.run_check(self.gen_section(self.compressed(full, short))), 1)

    def test_garbage_between_frames_fails(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        blob = self.compressed(full) + b"junk" + self.compressed(full)
        self.assertEqual(self.run_check(self.gen_section(blob)), 1)

    def test_truncated_frame_fails(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        blob = self.compressed(full).rstrip(b"\0")
        self.assertEqual(self.run_check(self.gen_section(blob[: len(blob) // 2])), 1)


if __name__ == "__main__":
    unittest.main()
