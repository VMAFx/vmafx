#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for the SYCL AOT image check (ADR-1360).

core/src/sycl/check_aot_image.py fails the build when libvmaf.so lacks the
spir64_gen images sycl_icpx_aot_targets asked for. These fixtures are
synthetic ELF64 files, so the contract runs on any host, with or without
oneAPI; `ocloc ids` is replaced by a fixed acronym-to-IP table.

ocloc writes one image per TU in one of two forms: an `ar` fat binary when it
is given two or more device acronyms, a bare zebin (an ELF file) when it is
given one, as with `-Dsycl_icpx_aot_targets=dg2-g11`. The check reads the IP
version of a bare zebin from the IntelGT product-config note (type 6) of its
.note.intelgt.compat section; the note layout here is the one ocloc 26.35
wrote for a dg2-g11 kernel.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
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
IP = {
    "dg2-g10": "12.55.8",
    "acm-g10": "12.55.8",
    "dg2-g11": "12.56.5",
    "adl-s": "12.2.0",
    "bmg-g21": "20.1.0",
}
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


def elf64(
    sections: dict[str, bytes] | list[tuple[str, bytes]],
    kinds: dict[str, int] | None = None,
    e_type: int = 3,
    machine: int = 62,
) -> bytes:
    """A minimal ELF64 little-endian file holding the named sections (PROGBITS unless in kinds).

    Sections are a dict, or a list of pairs when a name repeats.
    """
    kinds = kinds or {}
    pairs = list(sections.items()) if isinstance(sections, dict) else sections
    names = b"\0.shstrtab\0" + b"".join(name.encode() + b"\0" for name, _ in pairs)
    payloads = [(".shstrtab", names), *pairs]
    body = bytearray(b"\0" * 64)
    placed = []
    for name, payload in payloads:
        name_offset = names.index(name.encode() + b"\0")
        placed.append((name_offset, len(body), len(payload), kinds.get(name, 1)))
        body += payload
    shoff = len(body)
    body += b"\0" * 64  # SHN_UNDEF
    for name_offset, offset, size, kind in placed:
        body += struct.pack("<IIQQQQIIQQ", name_offset, kind, 0, 0, offset, size, 0, 0, 1, 0)
    header = b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9
    header += struct.pack(
        "<HHIQQQIHHHHHH", e_type, machine, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(placed) + 1, 1
    )
    body[:64] = header
    return bytes(body)


def note(kind: int, desc: bytes, owner: bytes = b"IntelGT\0") -> bytes:
    """One ELF note: a 12-byte header, then owner and descriptor padded to four bytes."""

    def pad(blob: bytes) -> bytes:
        return blob + b"\0" * (-len(blob) % 4)

    return struct.pack("<III", len(owner), len(desc), kind) + pad(owner) + pad(desc)


def ip_word(ip: str) -> int:
    """Pack "architecture.release.revision" like HardwareIpVersion: bits 31:22, 21:14 and 5:0."""
    architecture, release, revision = (int(part) for part in ip.split("."))
    return architecture << 22 | release << 14 | revision


def intelgt_notes(product_config: bytes) -> bytes:
    """The notes ocloc 26.35 wrote into a dg2-g11 zebin, with the product config replaced."""
    return b"".join(
        [
            note(1, struct.pack("<I", 0x4F6)),  # product family
            note(2, struct.pack("<I", 0xC07)),  # gfx core
            note(3, struct.pack("<I", 0x250500)),  # target metadata flags
            note(4, b"1.73\0"),  # zebin version
            note(6, product_config),  # product config: the GFX IP version
            note(7, struct.pack("<I", 10)),  # indirect access detection version
            note(8, struct.pack("<I", 1)),  # indirect access buffer major version
        ]
    )


def zebin(ip: str = "12.56.5", notes: bytes | None = None, text: bytes = b"\0" * 16) -> bytes:
    """A bare native image, as ocloc writes it for one device acronym: a relocatable ELF."""
    if notes is None:
        notes = intelgt_notes(struct.pack("<I", ip_word(ip)))
    sections = {".note.intelgt.compat": notes, ".text": text}
    return elf64(sections, kinds={".note.intelgt.compat": 7}, e_type=1, machine=205)


class AotImageCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        patcher = mock.patch.object(check_aot_image, "ocloc_ip_version", self.fake_ocloc)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.stderr = ""

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
        with contextlib.redirect_stderr(io.StringIO()) as captured:
            code = int(check_aot_image.main(argv))
        self.stderr = captured.getvalue()
        return code

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

    def stamp(self) -> str:
        return (self.root / "stamp").read_text()

    def test_bare_zebins_cover_a_single_target(self) -> None:
        # -Dsycl_icpx_aot_targets=dg2-g11: ocloc writes one bare zebin per TU, no ar archive.
        section = self.gen_section(zebin("12.56.5"), zebin("12.56.5"), zebin("12.56.5"))
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 0)
        self.assertIn(
            "3 spir64_gen native images; 1 IP versions for 1 targets; 0 partial by declaration",
            self.stamp(),
        )

    def test_bare_zebin_for_another_ip_fails(self) -> None:
        section = self.gen_section(zebin("12.55.8"))
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 1)
        self.assertIn("native image 0 is built for IP version ['12.55.8']", self.stderr)
        self.assertIn("no requested target uses (dg2-g11=12.56.5)", self.stderr)
        self.assertFalse((self.root / "stamp").exists())

    def test_one_stray_zebin_among_good_ones_fails(self) -> None:
        section = self.gen_section(zebin("12.56.5"), zebin("20.1.0"), zebin("12.56.5"))
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 1)
        self.assertIn("native image 1 is built for", self.stderr)

    def test_aliases_sharing_one_ip_are_covered_by_one_zebin(self) -> None:
        section = self.gen_section(zebin("12.55.8"))
        self.assertEqual(self.run_check(section, targets="dg2-g10,acm-g10"), 0)

    def test_bare_zebin_in_a_multi_target_build_fails(self) -> None:
        full = fat_binary(["12.55.8", "12.2.0", "20.1.0"])
        self.assertEqual(self.run_check(self.gen_section(full, zebin("12.55.8"))), 1)
        self.assertIn("native image 1 lacks IP versions ['12.2.0', '20.1.0']", self.stderr)

    def test_bare_zebin_of_a_declared_partial_unit_needs_only_its_other_targets(self) -> None:
        full = fat_binary(["12.56.5", "20.1.0"])
        partial = ("--partial", "integer_psnr_hvs_sycl=bmg-g21")
        section = self.gen_section(full, full, zebin("12.56.5"))
        self.assertEqual(self.run_check(section, *partial, targets="dg2-g11,bmg-g21"), 0)
        self.assertIn(
            "2 spir64_gen fat binaries, 1 spir64_gen native images; "
            "2 IP versions for 2 targets; 1 partial by declaration",
            self.stamp(),
        )
        # Without the declaration the same library lost bmg-g21 in one TU.
        self.assertEqual(self.run_check(section, targets="dg2-g11,bmg-g21"), 1)
        # A zebin for only the skipped target leaves the unit without the target it must keep.
        wrong = self.gen_section(full, full, zebin("20.1.0"))
        self.assertEqual(self.run_check(wrong, *partial, targets="dg2-g11,bmg-g21"), 1)
        self.assertIn("native image 2 lacks IP versions ['12.56.5']", self.stderr)

    def test_ip_version_fields_decode(self) -> None:
        for ip in ("12.56.5", "20.1.0", "12.0.0", "0.0.0", "1023.255.63"):
            with self.subTest(ip=ip):
                (image,) = check_aot_image.parse_images(zebin(ip))
                self.assertEqual(image, (check_aot_image.NATIVE, frozenset({ip})))

    def test_measured_product_config_word_decodes(self) -> None:
        # The bytes ocloc 26.35 wrote into the dg2-g11 zebin; `ocloc ids dg2-g11` prints 12.56.5.
        notes = intelgt_notes(b"\x05\x00\x0e\x03")
        (image,) = check_aot_image.parse_images(zebin(notes=notes))
        self.assertEqual(image.ips, frozenset({"12.56.5"}))

    def test_reserved_ip_bits_do_not_reach_the_fields(self) -> None:
        word = ip_word("12.56.5") | 0xFF << 6  # HardwareIpVersion.reserved, bits 13:6
        (image,) = check_aot_image.parse_images(zebin(notes=intelgt_notes(struct.pack("<I", word))))
        self.assertEqual(image.ips, frozenset({"12.56.5"}))

    def test_elf_without_intelgt_notes_fails(self) -> None:
        plain = elf64({".text": b"\0" * 16}, e_type=1, machine=205)
        self.assertEqual(self.run_check(self.gen_section(plain), targets="dg2-g11"), 1)
        self.assertIn("has 0 .note.intelgt.compat note sections", self.stderr)

    def test_note_section_of_the_wrong_type_fails(self) -> None:
        notes = intelgt_notes(struct.pack("<I", ip_word("12.56.5")))
        progbits = elf64({".note.intelgt.compat": notes})  # sh_type PROGBITS, not SHT_NOTE
        self.assertEqual(self.run_check(self.gen_section(progbits), targets="dg2-g11"), 1)

    def test_zebin_without_a_product_config_note_fails(self) -> None:
        notes = note(1, struct.pack("<I", 0x4F6)) + note(4, b"1.73\0")
        section = self.gen_section(zebin(notes=notes))
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 1)
        self.assertIn("no 4-byte IntelGT product-config note", self.stderr)

    def test_product_config_note_of_the_wrong_size_fails(self) -> None:
        notes = intelgt_notes(struct.pack("<Q", ip_word("12.56.5")))
        self.assertEqual(self.run_check(self.gen_section(zebin(notes=notes)), targets="dg2-g11"), 1)
        self.assertIn("no 4-byte IntelGT product-config note", self.stderr)

    def test_product_config_note_of_another_owner_is_ignored(self) -> None:
        notes = note(6, struct.pack("<I", ip_word("12.56.5")), owner=b"GNU\0")
        self.assertEqual(self.run_check(self.gen_section(zebin(notes=notes)), targets="dg2-g11"), 1)
        self.assertIn("no 4-byte IntelGT product-config note", self.stderr)

    def test_truncated_notes_fail(self) -> None:
        notes = note(6, struct.pack("<I", ip_word("12.56.5")))
        for keep, message in ((len(notes) - 2, "a note overruns"), (5, "truncated note header")):
            with self.subTest(keep=keep):
                section = self.gen_section(zebin(notes=notes[:keep]))
                self.assertEqual(self.run_check(section, targets="dg2-g11"), 1)
                self.assertIn(message, self.stderr)

    def test_two_note_sections_fail(self) -> None:
        notes = intelgt_notes(struct.pack("<I", ip_word("12.56.5")))
        twice = [(".note.intelgt.compat", notes), (".note.intelgt.compat", notes)]
        image = elf64(twice, kinds={".note.intelgt.compat": 7}, e_type=1, machine=205)
        self.assertEqual(self.run_check(self.gen_section(image), targets="dg2-g11"), 1)
        self.assertIn("has 2 .note.intelgt.compat note sections", self.stderr)

    def test_zebin_with_a_bad_section_name_table_index_fails(self) -> None:
        image = bytearray(zebin("12.56.5"))
        struct.pack_into("<H", image, 0x3E, 99)  # e_shstrndx past e_shnum
        self.assertEqual(self.run_check(self.gen_section(bytes(image)), targets="dg2-g11"), 1)
        self.assertIn("section name table index is out of range", self.stderr)

    def test_elf32_image_fails(self) -> None:
        image = b"\x7fELF" + bytes([1, 1, 1]) + b"\0" * 57
        self.assertEqual(self.run_check(self.gen_section(image), targets="dg2-g11"), 1)
        self.assertIn("not an ELF64 little-endian object", self.stderr)

    def test_image_that_is_neither_form_fails(self) -> None:
        section = self.gen_section(zebin("12.56.5") + b"SPIRV")
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 1)
        self.assertIn("neither an ocloc fat binary nor a native ELF image", self.stderr)

    def test_zero_padding_between_images_is_skipped(self) -> None:
        blob = zebin("12.56.5") + b"\0" * 5 + fat_binary(["12.56.5"]) + b"\0" * 3
        self.assertEqual(self.run_check(self.gen_section(blob), targets="dg2-g11"), 0)
        self.assertIn("1 spir64_gen fat binaries, 1 spir64_gen native images", self.stamp())

    def test_truncated_bare_zebin_fails(self) -> None:
        image = zebin("12.56.5")
        self.assertEqual(self.run_check(self.gen_section(image[:-10]), targets="dg2-g11"), 1)
        self.assertIn("truncated", self.stderr)
        self.assertEqual(self.run_check(self.gen_section(image[:-200]), targets="dg2-g11"), 1)

    def test_archive_after_an_odd_length_zebin_keeps_its_alignment(self) -> None:
        odd = zebin("12.56.5", text=b"\0" * 17)
        self.assertEqual(len(odd) % 2, 1)
        section = self.gen_section(odd + fat_binary(["12.56.5"]) + odd)
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 0)
        self.assertIn("1 spir64_gen fat binaries, 2 spir64_gen native images", self.stamp())

    def test_compressed_bare_zebins_are_decoded(self) -> None:
        section = self.gen_section(self.compressed(zebin("12.56.5"), zebin("12.56.5")))
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 0)
        self.assertIn("2 spir64_gen native images", self.stamp())

    def test_compressed_frames_may_mix_both_forms(self) -> None:
        section = self.gen_section(self.compressed(fat_binary(["12.56.5"]), zebin("12.56.5")))
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 0)
        self.assertIn("1 spir64_gen fat binaries, 1 spir64_gen native images", self.stamp())

    def test_compressed_bare_zebin_for_another_ip_fails(self) -> None:
        section = self.gen_section(self.compressed(zebin("12.56.5"), zebin("20.1.0")))
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 1)
        self.assertIn("native image 1 is built for", self.stderr)

    def test_garbage_between_zebin_frames_fails(self) -> None:
        frame = self.compressed(zebin("12.56.5"))
        self.assertEqual(self.run_check(self.gen_section(frame + b"junk" + frame)), 1)

    def test_compressed_frame_holding_neither_form_fails(self) -> None:
        section = self.gen_section(self.compressed(zebin("12.56.5"), b"SPIRV"))
        self.assertEqual(self.run_check(section, targets="dg2-g11"), 1)
        self.assertIn("neither an ocloc fat binary nor a native ELF image", self.stderr)


if __name__ == "__main__":
    unittest.main()
