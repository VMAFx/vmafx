#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail the build when GPU device code is stored uncompressed (ADR-1590).

``compress_device_code`` (default on) asks every device compiler for its
strongest compression. A flag that a toolchain upgrade stops honouring, or a
new kernel target that misses the policy list, would silently store raw device
code again; this check reads what the build produced and refuses it:

* a CUDA fatbin (``--fatbin FILE``) must start with the fatbin magic and hold
  no entry stored raw: no cubin ELF header (ELF64, ``e_machine`` EM_CUDA) and
  no PTX text (``.target sm_``). nvcc's default compresses the PTX only.
* a HIP code object bundle (``--hsaco FILE``) must be a compressed offload
  bundle (magic ``CCOB``) and hold no AMDGPU ELF header (``e_machine``
  EM_AMDGPU) outside the compressed stream.
* a linked ELF binary with SYCL device images (``--sycl-elf FILE``) must store
  every ``__CLANG_OFFLOAD_BUNDLE__sycl-*`` section as zstd frames, with only
  zero padding between them; SPIR-V and the spir64_gen images alike. The ELF
  and zstd frame readers are check_aot_image.py's (ADR-1360).

Usage: check_device_compression.py --stamp OUT [--fatbin F...] [--hsaco F...]
                                   [--sycl-elf F...]
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import struct
import sys
from pathlib import Path

FATBIN_MAGIC = 0xBA55ED50
MAGIC_SIZE = 4
CCOB_MAGIC = b"CCOB"
ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
E_MACHINE_OFFSET = 18
EM_CUDA = 190
EM_AMDGPU = 224
PTX_TARGET = b".target sm_"
SYCL_SECTION_PREFIX = "__CLANG_OFFLOAD_BUNDLE__sycl-"
REMEDY = (
    "Device code is stored uncompressed although compress_device_code is on. "
    "Every device compile must take its backend's compression list in "
    "core/src/meson.build (cuda_compress_args, hip_compress_args, "
    "sycl_compress_args); see ADR-1590."
)


def load_aot_checker():
    """Import sycl/check_aot_image.py, whose ELF and zstd readers this check reuses."""
    path = Path(__file__).resolve().parent / "sycl" / "check_aot_image.py"
    spec = importlib.util.spec_from_file_location("check_aot_image", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


AOT = load_aot_checker()


class CompressionError(Exception):
    """Raw device code where compressed code was asked for, reported verbatim."""


def raw_elf_headers(data: bytes, machine: int) -> int:
    """Count ELF64 little-endian headers for ``machine`` stored in the clear in ``data``."""
    count = 0
    for match in re.finditer(re.escape(ELF_MAGIC), data):
        start = match.start()
        if start + E_MACHINE_OFFSET + 2 > len(data):
            continue
        if data[start + 4] != ELFCLASS64 or data[start + 5] != ELFDATA2LSB:
            continue
        (found,) = struct.unpack_from("<H", data, start + E_MACHINE_OFFSET)
        count += int(found == machine)
    return count


def check_fatbin(name: str, data: bytes) -> None:
    """A fatbin holds every cubin and PTX entry compressed."""
    if len(data) < MAGIC_SIZE or struct.unpack_from("<I", data, 0)[0] != FATBIN_MAGIC:
        raise CompressionError(f"{name}: not a CUDA fatbin (magic 0x{FATBIN_MAGIC:08X})")
    cubins = raw_elf_headers(data, EM_CUDA)
    ptx = data.count(PTX_TARGET)
    if cubins or ptx:
        raise CompressionError(f"{name}: {cubins} cubin and {ptx} PTX entries stored raw")


def check_hsaco(name: str, data: bytes) -> None:
    """A HIP code object bundle is a compressed offload bundle."""
    if not data.startswith(CCOB_MAGIC):
        raise CompressionError(f"{name}: not a compressed offload bundle (no {CCOB_MAGIC!r} magic)")
    objects = raw_elf_headers(data, EM_AMDGPU)
    if objects:
        raise CompressionError(f"{name}: {objects} AMDGPU code objects stored raw")


def zstd_frames_only(section: str, blob: bytes) -> None:
    """``blob`` is one or more zstd frames with zero padding between them."""
    if not blob.startswith(AOT.ZSTD_MAGIC):
        raise CompressionError(f"{section}: the device image is not zstd-compressed")
    position = 0
    for _ in range(len(blob)):
        if position >= len(blob):
            return
        if blob[position] == 0:
            position += 1
            continue
        if not blob.startswith(AOT.ZSTD_MAGIC, position):
            raise CompressionError(f"{section}: raw bytes at offset {position} between zstd frames")
        try:
            position = AOT.zstd_frame_end(blob, position)
        except AOT.CheckError as error:
            raise CompressionError(f"{section}: {error}") from None


def check_sycl_elf(name: str, data: bytes) -> None:
    """Every SYCL device image section of a linked binary is zstd frames."""
    try:
        sections = AOT.elf_sections(data)
    except AOT.CheckError as error:
        raise CompressionError(f"{name}: {error}") from None
    found = [entry for entry in sections if entry[0].startswith(SYCL_SECTION_PREFIX)]
    if not found:
        raise CompressionError(f"{name}: no {SYCL_SECTION_PREFIX}* section to check")
    for section, _kind, offset, size in found:
        zstd_frames_only(f"{name}: {section}", data[offset : offset + size])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stamp", required=True, type=Path)
    for flag in ("--fatbin", "--hsaco", "--sycl-elf"):
        parser.add_argument(flag, action="extend", nargs="+", default=[], type=Path)
    args = parser.parse_args(argv)
    checks = [(check_fatbin, args.fatbin), (check_hsaco, args.hsaco)]
    checks.append((check_sycl_elf, args.sycl_elf))
    if not any(paths for _check, paths in checks):
        parser.error("nothing to check: pass --fatbin, --hsaco or --sycl-elf")
    try:
        for check, paths in checks:
            for path in paths:
                check(path.name, path.read_bytes())
    except CompressionError as error:
        print(f"check_device_compression: {error}\n{REMEDY}", file=sys.stderr)
        return 1
    count = sum(len(paths) for _check, paths in checks)
    args.stamp.write_text(f"{count} device code files compressed\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
