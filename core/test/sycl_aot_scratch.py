#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Scratch memory of the ahead-of-time kernel images, read without a device (ADR-1395).

A kernel uses scratch memory when IGC spills registers or keeps a private array
in memory. On Arc A-series GPUs under the Linux xe driver such a kernel returns
wrong values, so no libvmaf SYCL kernel may use any. ``test_sycl_kernel_scratch``
measures that on the GPU it runs on; this module measures it for every target of
the default AOT list from the native images ocloc writes.

Each image is an Intel GPU ELF (a zebin) whose ``.ze_info`` section records, per
kernel, ``spill_size`` and ``private_size`` and lists any per-thread memory
buffer. A multi-target compile stores the images in an ar archive whose member
names carry the target's GFX IP version (``64.12.2.0`` for ``adl-s``); without
``--offload-compress`` the archive sits uncompressed in the object icpx writes.

Spills differ per target: Xe-LP (``tgllp``, ``adl-*``, ``rpl-*``) has no
256-entry register file, so a kernel that asks for it through
``VmafSyclKernelShape<SG, 256>`` gets 128 registers there (UHD 770 reports of
2026-10-05, ``T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02``).
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass

AR_MAGIC = b"!<arch>\n"
AR_HEADER = 60
ELF_MAGIC = b"\x7fELF"
ELF64_HEADER = 64
# ar member names of a target's image: an optional pointer-size prefix and the
# GFX IP version.
_IMAGE_MEMBER = re.compile(r"^(?:\d+\.)?(\d+\.\d+\.\d+)$")
_KERNEL = re.compile(r"^  - name:\s+(\S+)\s*$", re.M)
_SIZE = re.compile(r"^\s+(spill_size|private_size):\s+(\d+)\s*$", re.M)
_TOP_LEVEL = re.compile(r"^\S", re.M)

# The scratch probes of core/src/sycl/scratch_check.cpp use scratch memory on
# purpose; the device audit leaves them out too.
PROBE_NAMES = ("_ZTS25VmafSyclScratchProbeSpill", "_ZTS27VmafSyclScratchProbePrivate")

# Target families by the name up to the first hyphen (sycl_aot_targets.family).
XE_LP = frozenset({"tgllp", "adl", "rpl"})
XE_LPG = frozenset({"mtl", "arl"})

# Kernels that still use scratch memory on some families, measured with ocloc
# 26.35 / IGC 2.41.5. They are the SIMD-32 instances of integer_vif_sycl, which
# run only when VMAF_SYCL_VIF_SUBGROUP_SIZE=32 forces them; the maintainer
# decision on them is open in T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02.
# The list only shrinks: never add an entry to make the check pass.
KNOWN_SCRATCH: dict[str, frozenset[str]] = {
    "_ZTSN12_GLOBAL__N_120IntegerVifHoriKernelILi0ELi32EEE": XE_LP | XE_LPG,
    "_ZTSN12_GLOBAL__N_120IntegerVifHoriKernelILi1ELi32EEE": XE_LP,
    "_ZTSN12_GLOBAL__N_120IntegerVifHoriKernelILi2ELi32EEE": XE_LP,
    "_ZTSN12_GLOBAL__N_120IntegerVifHoriKernelILi3ELi32EEE": XE_LP,
    "_ZTSN12_GLOBAL__N_121IntegerVifFusedKernelILi0ELi32EEE": XE_LP,
    "_ZTSN12_GLOBAL__N_121IntegerVifFusedKernelILi1ELi32EEE": XE_LP,
    "_ZTSN12_GLOBAL__N_121IntegerVifFusedKernelILi2ELi32EEE": XE_LP,
    "_ZTSN12_GLOBAL__N_121IntegerVifFusedKernelILi3ELi32EEE": XE_LP,
}


@dataclass(frozen=True)
class KernelScratch:
    """One kernel of one target's image that uses scratch memory."""

    kernel: str
    ip: str
    spill: int
    private: int


def ar_members(data: bytes, start: int = 0) -> list[tuple[str, bytes]]:
    """The members of the ar archive at `start` (ocloc's fat binary)."""
    if data[start : start + len(AR_MAGIC)] != AR_MAGIC:
        raise ValueError(f"no ar archive at offset {start}")
    members: list[tuple[str, bytes]] = []
    position = start + len(AR_MAGIC)
    while position + AR_HEADER <= len(data):
        header = data[position : position + AR_HEADER]
        if header[58:60] != b"`\n":
            break  # the bytes after the archive
        size = int(header[48:58].decode("ascii").strip())
        name = header[:16].decode("ascii", errors="replace").strip().rstrip("/")
        members.append((name, data[position + AR_HEADER : position + AR_HEADER + size]))
        position += AR_HEADER + size + (size & 1)
    return members


def elf_section(blob: bytes, wanted: str) -> bytes | None:
    """The bytes of the ELF64 section named `wanted`, or None."""
    if blob[:4] != ELF_MAGIC or len(blob) < ELF64_HEADER:
        return None
    shoff = struct.unpack_from("<Q", blob, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x3A)
    headers = [struct.unpack_from("<IIQQQQ", blob, shoff + i * shentsize) for i in range(shnum)]
    names = headers[shstrndx][4]
    for name, _kind, _flags, _addr, offset, size in headers:
        end = blob.index(b"\0", names + name)
        if blob[names + name : end].decode("ascii", errors="replace") == wanted:
            return blob[offset : offset + size]
    return None


def kernels_section(ze_info: str) -> str:
    """The top-level `kernels:` list of a .ze_info document."""
    start = ze_info.find("\nkernels:")
    if start < 0:
        return ""
    following = _TOP_LEVEL.search(ze_info, start + len("\nkernels:") + 1)
    return ze_info[start : following.start() if following else len(ze_info)]


def kernel_scratch(ze_info: str, ip: str) -> list[KernelScratch]:
    """Every kernel of one image whose spill, private memory or buffers are not empty."""
    section = kernels_section(ze_info)
    starts = list(_KERNEL.finditer(section))
    found = []
    for index, match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(section)
        body = section[match.end() : end]
        sizes = {key: int(value) for key, value in _SIZE.findall(body)}
        spill, private = sizes.get("spill_size", 0), sizes.get("private_size", 0)
        if spill or private or "per_thread_memory_buffers:" in body:
            found.append(KernelScratch(match.group(1), ip, spill, private))
    return found


def images_in(data: bytes) -> list[tuple[str, bytes]]:
    """(GFX IP, zebin) of every target image in the ar archives inside `data`."""
    images = []
    position = data.find(AR_MAGIC)
    while position >= 0:
        for name, blob in ar_members(data, position):
            match = _IMAGE_MEMBER.match(name)
            if match and blob[:4] == ELF_MAGIC:
                images.append((match.group(1), blob))
        position = data.find(AR_MAGIC, position + len(AR_MAGIC))
    return images


def object_scratch(data: bytes) -> tuple[list[str], list[KernelScratch]]:
    """The GFX IP of every target image in an object, and its kernels in scratch memory."""
    images = images_in(data)
    found = []
    for ip, blob in images:
        ze_info = elf_section(blob, ".ze_info")
        if ze_info is None:
            raise ValueError(f"image for {ip} has no .ze_info section")
        found += kernel_scratch(ze_info.decode("utf-8", errors="replace"), ip)
    return [ip for ip, _blob in images], [e for e in found if e.kernel not in PROBE_NAMES]


def judge(
    found: list[KernelScratch], ip_families: dict[str, set[str]]
) -> tuple[list[str], list[str]]:
    """(failures, notes): scratch outside KNOWN_SCRATCH, and entries that no longer spill."""
    failures, seen = [], set()
    for entry in dict.fromkeys(found):  # ocloc builds one image per target name
        families = ip_families.get(entry.ip, {f"unknown IP {entry.ip}"})
        allowed = KNOWN_SCRATCH.get(entry.kernel, frozenset())
        if families <= allowed:
            seen |= {(entry.kernel, family) for family in families}
            continue
        failures.append(
            f"{entry.kernel} uses scratch memory on {'/'.join(sorted(families))} "
            f"(GFX IP {entry.ip}): spill {entry.spill} B, private {entry.private} B"
        )
    notes = [
        f"{kernel} no longer uses scratch memory on {family}: remove it from KNOWN_SCRATCH"
        for kernel, families in sorted(KNOWN_SCRATCH.items())
        for family in sorted(families)
        if (kernel, family) not in seen and any(family in f for f in ip_families.values())
    ]
    return failures, notes
