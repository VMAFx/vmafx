#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The device-free scratch check of the sycl-aot suite reads what IGC writes (ADR-1395).

``sycl_aot_scratch`` reads each kernel's ``spill_size`` and ``private_size``
from the ``.ze_info`` section of the native images ocloc writes per target, and
``test_sycl_aot_default_targets.py`` fails on a kernel that uses scratch memory
on any target of the default list. This test needs no compiler and no device:
it builds images in the format IGC 2.41.5 writes (an ar archive of ELF zebins
named by GFX IP version, embedded in an object) and plants each defect the
check must refuse.
"""

from __future__ import annotations

import contextlib
import io
import struct
import unittest

import sycl_aot_scratch as scratch
import test_sycl_aot_default_targets as aot_build

SLOT = "_ZTSN12_GLOBAL__N_113Ss2SlotKernelE"
HORI_0_32 = "_ZTSN12_GLOBAL__N_120IntegerVifHoriKernelILi0ELi32EEE"
HORI_1_32 = "_ZTSN12_GLOBAL__N_120IntegerVifHoriKernelILi1ELi32EEE"
PROBE = "_ZTS25VmafSyclScratchProbeSpill"
XE_LP_IP, XE_LPG_IP, XE_HPG_IP, XE2_IP = "12.2.0", "12.70.4", "12.56.5", "20.1.0"
IP_FAMILIES = {
    XE_LP_IP: {"adl", "rpl"},
    XE_LPG_IP: {"mtl", "arl"},
    XE_HPG_IP: {"dg2", "acm"},
    XE2_IP: {"bmg"},
}


def kernel_entry(name: str, spill: int = 0, private: int = 0, buffer: bool = False) -> str:
    """One `kernels:` entry as IGC 2.41.5 writes it (abridged)."""
    sizes = (f"      spill_size:      {spill}\n" if spill else "") + (
        f"      private_size:    {private}\n" if private else ""
    )
    scratch_buffer = (
        "    per_thread_memory_buffers:\n"
        "      - type:            scratch\n"
        "        usage:           single_space\n"
        "        size:            1024\n"
        if buffer
        else ""
    )
    return (
        f"  - name:            {name}\n"
        "    user_attributes:\n"
        "      intel_reqd_sub_group_size: 16\n"
        "    execution_env:\n"
        "      grf_count:       128\n"
        "      simd_size:       16\n"
        f"{sizes}"
        "    payload_arguments:\n"
        "      - arg_type:        global_id_offset\n"
        "        offset:          0\n"
        "        size:            12\n"
        f"{scratch_buffer}"
    )


def ze_info(*entries: str, misc: str = "") -> str:
    """A .ze_info document; `misc` names a kernel in kernels_misc_info only."""
    text = "---\nversion:         '1.73'\nkernels:\n" + "".join(entries)
    if misc:
        text += f"kernels_misc_info:\n  - name:            {misc}\n    args_info: []\n"
    return text + "...\n"


def zebin(info: str) -> bytes:
    """A minimal ELF64 with the sections .ze_info and .shstrtab."""
    names = b"\0.ze_info\0.shstrtab\0"
    body = info.encode()
    data_offset = 64
    names_offset = data_offset + len(body)
    shoff = names_offset + len(names)
    header = bytearray(64)
    header[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<Q", header, 0x28, shoff)
    struct.pack_into("<HHH", header, 0x3A, 64, 3, 2)
    sections = bytes(64)
    sections += struct.pack("<IIQQQQ", 1, 1, 0, 0, data_offset, len(body)) + bytes(24)
    sections += struct.pack("<IIQQQQ", 10, 3, 0, 0, names_offset, len(names)) + bytes(24)
    return bytes(header) + body + names + sections


def ar_member(name: str, data: bytes) -> bytes:
    header = f"{name + '/':<16}{0:<12}{0:<6}{0:<6}{644:<8}{len(data):<10}`\n".encode()
    return header + data + (b"\n" if len(data) & 1 else b"")


def fat_object(images: dict[str, str]) -> bytes:
    """An object holding ocloc's archive: GFX IP -> .ze_info text, plus generic IR."""
    archive = scratch.AR_MAGIC + ar_member("pad_0", b" " * 8)
    for ip, info in images.items():
        archive += ar_member(f"64.{ip}", zebin(info))
    archive += ar_member("generic_ir", zebin(ze_info()))
    return b"\x7fELF host object bytes" + archive + b"\0\0wrapper tail"


def build_check(results: list[aot_build.UnitResult]) -> list[str]:
    """scratch_failures() with its report captured."""
    with contextlib.redirect_stdout(io.StringIO()):
        return aot_build.scratch_failures(results, IP_FAMILIES)


def judged(images: dict[str, str]) -> tuple[list[str], list[str]]:
    _ips, found = scratch.object_scratch(fat_object(images))
    return scratch.judge(found, IP_FAMILIES)


class ImageReading(unittest.TestCase):
    """The reader finds every image and every size IGC records."""

    def test_every_target_image_is_read(self) -> None:
        clean = ze_info(kernel_entry(SLOT))
        ips, found = scratch.object_scratch(fat_object({XE_LP_IP: clean, XE2_IP: clean}))
        self.assertEqual(ips, [XE_LP_IP, XE2_IP])  # generic_ir is not a target image
        self.assertEqual(found, [])

    def test_spill_and_private_sizes_are_read(self) -> None:
        info = ze_info(kernel_entry(SLOT, spill=384), kernel_entry(HORI_1_32, private=4096))
        _ips, found = scratch.object_scratch(fat_object({XE_LP_IP: info}))
        self.assertEqual(
            found,
            [
                scratch.KernelScratch(SLOT, XE_LP_IP, 384, 0),
                scratch.KernelScratch(HORI_1_32, XE_LP_IP, 0, 4096),
            ],
        )

    def test_misc_info_list_is_not_a_kernel(self) -> None:
        info = ze_info(kernel_entry(SLOT), misc=HORI_1_32)
        self.assertEqual(scratch.kernel_scratch(info, XE_LP_IP), [])


class ScratchJudgement(unittest.TestCase):
    """Planted defects the check refuses, and what it lets through."""

    def test_spill_on_xe_lp_is_refused(self) -> None:
        # The UHD 770 reports of 2026-10-05: 384 bytes at SIMD-16 without 256 GRF.
        failures, _notes = judged({XE_LP_IP: ze_info(kernel_entry(SLOT, spill=384))})
        self.assertEqual(len(failures), 1)
        self.assertIn(SLOT, failures[0])
        self.assertIn("adl/rpl", failures[0])
        self.assertIn("spill 384 B", failures[0])

    def test_private_memory_is_refused(self) -> None:
        failures, _notes = judged({XE_HPG_IP: ze_info(kernel_entry(SLOT, private=896))})
        self.assertEqual(len(failures), 1)
        self.assertIn("private 896 B", failures[0])

    def test_scratch_buffer_without_sizes_is_refused(self) -> None:
        failures, _notes = judged({XE2_IP: ze_info(kernel_entry(SLOT, buffer=True))})
        self.assertEqual(len(failures), 1)

    def test_unknown_gfx_ip_is_refused(self) -> None:
        _ips, found = scratch.object_scratch(
            fat_object({"30.0.0": ze_info(kernel_entry(HORI_1_32, spill=64))})
        )
        failures, _notes = scratch.judge(found, IP_FAMILIES)
        self.assertEqual(len(failures), 1)
        self.assertIn("unknown IP 30.0.0", failures[0])

    def test_known_kernel_passes_only_on_its_families(self) -> None:
        on_xe_lp = ze_info(kernel_entry(HORI_1_32, spill=10048))
        self.assertEqual(judged({XE_LP_IP: on_xe_lp})[0], [])
        for ip in (XE_LPG_IP, XE_HPG_IP, XE2_IP):
            failures, _notes = judged({ip: ze_info(kernel_entry(HORI_1_32, spill=96))})
            self.assertEqual(len(failures), 1, ip)

    def test_known_xe_lpg_entry_passes_there(self) -> None:
        info = ze_info(kernel_entry(HORI_0_32, spill=96))
        self.assertEqual(judged({XE_LPG_IP: info})[0], [])

    def test_probes_are_left_out(self) -> None:
        info = ze_info(kernel_entry(PROBE, spill=4096))
        self.assertEqual(judged({XE_HPG_IP: info})[0], [])

    def test_known_entry_that_no_longer_spills_is_noted(self) -> None:
        clean = ze_info(kernel_entry(HORI_1_32))
        failures, notes = judged({XE_LP_IP: clean})
        self.assertEqual(failures, [])
        self.assertTrue(any(HORI_1_32 in note and "adl" in note for note in notes), notes)

    def test_known_list_names_only_the_vif_simd32_instances(self) -> None:
        # The list only shrinks (ADR-1395): the four kernels fixed for Xe-LP
        # must not come back through it.
        for kernel in scratch.KNOWN_SCRATCH:
            self.assertRegex(kernel, r"IntegerVif(Hori|Fused)KernelILi\dELi32EEE$")


class BuildIntegration(unittest.TestCase):
    """The sycl-aot test compiles uncompressed and refuses a missing target image."""

    def test_rewritten_compile_is_uncompressed(self) -> None:
        argv = [
            "icpx",
            "-c",
            "--offload-compress",
            "--offload-compression-level=22",
            "-fsycl",
            "-fsycl-targets=spir64_gen,spir64",
            "-Xsycl-target-backend=spir64_gen",
            "-device dg2-g11",
            "x.cpp",
            "-o",
            "x.o",
        ]
        rewritten = aot_build.for_targets(argv, ["adl-s", "bmg-g21"], "/t/0.o")
        self.assertFalse([a for a in rewritten if a.startswith(aot_build.COMPRESSION)])
        self.assertIn("-device adl-s,bmg-g21", rewritten)

    def test_missing_target_image_is_refused(self) -> None:
        result = aot_build.UnitResult("x.cpp", [], [XE_LP_IP, XE_HPG_IP], [])
        failures = build_check([result])
        self.assertEqual(len(failures), 1)
        self.assertIn("default list has", failures[0])

    def test_scratch_kernel_fails_the_build_check(self) -> None:
        found = [scratch.KernelScratch(SLOT, XE_LP_IP, 384, 0)]
        result = aot_build.UnitResult("x.cpp", [], list(IP_FAMILIES), found)
        failures = build_check([result])
        self.assertEqual(len(failures), 1)
        self.assertIn(SLOT, failures[0])


if __name__ == "__main__":
    unittest.main()
