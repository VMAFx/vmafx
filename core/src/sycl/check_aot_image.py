#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail the build when libvmaf does not carry the SYCL AOT images it asked for.

core/src/meson.build compiles every SYCL TU with ``-fsycl-targets=spir64_gen``
for the ``sycl_icpx_aot_targets`` list. Before ADR-1360 the link quietly threw
those images away and shipped SPIR-V only, while configure still announced AOT.
This check reads the linked ELF shared object and requires what AOT promises:

* a ``__CLANG_OFFLOAD_BUNDLE__sycl-spir64_gen`` section, where icpx puts the
  native images;
* in every image inside it, the GFX IP version of every requested target, as
  ``ocloc ids <target>`` reports it. The build compresses each image with
  ``--offload-compress``; zstd frames are decoded first. ocloc writes an image
  in one of two forms, by the number of device acronyms it was given:

  - two or more, even when they share an IP version: a fat binary, an ``ar``
    archive with one member per acronym, named ``<bits>.<GFX IP version>``;
  - exactly one, as with ``-Dsycl_icpx_aot_targets=dg2-g11``: a bare zebin, an
    ELF file with no archive around it. Its IP version is the product-config
    note (IntelGT note type 6) of its ``.note.intelgt.compat`` section, a 32-bit
    word laid out as ``HardwareIpVersion`` (architecture in bits 31:22, release
    in 21:14, revision in 5:0) that ``ocloc ids`` prints as
    ``architecture.release.revision``. Layout source: intel/compute-runtime,
    shared/source/device_binary_format/zebin/zebin_elf.h (``IntelGTSectionType``)
    and shared/source/helpers/hw_ip_version.h.

A bare zebin counts as an image that carries one IP version, so both forms
obey the same rules. Anything else in the section is an error.

TUs that the build deliberately compiles without some targets because IGC
cannot compile them (``--partial``) may leave those targets out, and only those.

Usage: check_aot_image.py --ocloc PATH --targets a,b,c [--partial TU=t1,t2]...
                          --stamp OUT LIBRARY
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

SECTION = "__CLANG_OFFLOAD_BUNDLE__sycl-spir64_gen"
AR_MAGIC = b"!<arch>\n"
AR_HEADER = 60
ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
ELF_HEADER = 64
SHT_NOTE = 7
SHT_NOBITS = 8
# zebin_elf.h: SectionNames::noteIntelGT, intelGTNoteOwnerName, IntelGTSectionType::productConfig.
NOTE_SECTION = ".note.intelgt.compat"
NOTE_OWNER = b"IntelGT\0"
NOTE_HEADER = 12
NOTE_PRODUCT_CONFIG = 6
PRODUCT_CONFIG_SIZE = 4
MAX_NOTES = 64
# hw_ip_version.h: HardwareIpVersion { revision : 6; reserved : 8; release : 8; architecture : 10 }.
IP_ARCHITECTURE_SHIFT = 22
IP_RELEASE_SHIFT = 14
IP_RELEASE_MASK = 0xFF
IP_REVISION_MASK = 0x3F
FAT = "fat binary"
NATIVE = "native image"
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
ZSTD_RLE_BLOCK = 1
ZSTD_RESERVED_BLOCK = 3
MAX_FRAME_BLOCKS = 1 << 20
OCLOC_TIMEOUT_SECONDS = 60
MAX_ARCHIVE_MEMBERS = 4096
REMEDY = (
    "The build lost ahead-of-time images: every SYCL TU must be compiled with "
    "-fno-sycl-rdc and -fsycl-targets=spir64_gen,spir64 plus the device list "
    "(sycl_toolchain_args in core/src/meson.build), and ocloc must be on PATH. "
    "See ADR-1360."
)


class CheckError(Exception):
    """A violated AOT expectation, reported verbatim."""


# A section of an ELF file: (name, type, file offset, size).
Section = tuple[str, int, int, int]


class Image(NamedTuple):
    """One spir64_gen image: its form (FAT or NATIVE) and the GFX IP versions it carries."""

    kind: str
    ips: frozenset[str]


def elf_sections(data: bytes, base: int = 0) -> list[Section]:
    """List the sections of the ELF64 little-endian file at ``base``; offsets are file-relative."""
    ident = data[base : base + ELF_HEADER]
    if (
        len(ident) < ELF_HEADER
        or ident[:4] != ELF_MAGIC
        or ident[4] != ELFCLASS64
        or ident[5] != ELFDATA2LSB
    ):
        raise CheckError("not an ELF64 little-endian object")
    (shoff,) = struct.unpack_from("<Q", data, base + 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, base + 0x3A)
    if shstrndx >= shnum:
        raise CheckError("the ELF section name table index is out of range")
    headers = [
        struct.unpack_from("<IIQQQQ", data, base + shoff + i * shentsize) for i in range(shnum)
    ]
    names_offset = base + headers[shstrndx][4]
    sections = []
    for name_offset, kind, _flags, _addr, offset, size in headers:
        start = names_offset + name_offset
        name = data[start : data.index(b"\0", start)].decode("ascii", "replace")
        sections.append((name, kind, offset, size))
    return sections


def elf_section(data: bytes, wanted: str) -> bytes | None:
    """Return the contents of the named section of an ELF64 little-endian file."""
    for name, _kind, offset, size in elf_sections(data):
        if name == wanted:
            return data[offset : offset + size]
    return None


def elf_end(data: bytes, base: int, sections: list[Section]) -> int:
    """Return where the ELF image at ``base`` ends: past its section table and section data."""
    (shoff,) = struct.unpack_from("<Q", data, base + 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, base + 0x3A)
    ends: list[int] = [ELF_HEADER, shoff + shnum * shentsize]
    ends += [offset + size for _name, kind, offset, size in sections if kind != SHT_NOBITS]
    return base + max(ends)


def intelgt_note(notes: bytes, wanted: int) -> bytes | None:
    """Return the descriptor of the first IntelGT note of type ``wanted``, or None.

    Notes are a 12-byte header, then the owner name and the descriptor, each padded to 4 bytes.
    Notes of other owners are skipped.
    """
    position = 0
    for _ in range(MAX_NOTES):
        if position == len(notes):
            return None
        if position + NOTE_HEADER > len(notes):
            raise CheckError(f"truncated note header in {NOTE_SECTION}")
        name_size, desc_size, kind = struct.unpack_from("<III", notes, position)
        name_at = position + NOTE_HEADER
        desc_at = name_at + ((name_size + 3) & ~3)
        position = desc_at + ((desc_size + 3) & ~3)
        if position > len(notes):
            raise CheckError(f"a note overruns {NOTE_SECTION}")
        if kind == wanted and notes[name_at : name_at + name_size] == NOTE_OWNER:
            return notes[desc_at : desc_at + desc_size]
    raise CheckError(f"more than {MAX_NOTES} notes in {NOTE_SECTION}")


def native_ip_version(data: bytes, base: int, sections: list[Section]) -> str:
    """Return the GFX IP version of the bare zebin at ``base``, spelled as ``ocloc ids`` does."""
    notes = [
        data[base + offset : base + offset + size]
        for name, kind, offset, size in sections
        if name == NOTE_SECTION and kind == SHT_NOTE
    ]
    if len(notes) != 1:
        raise CheckError(f"a native image has {len(notes)} {NOTE_SECTION} note sections, not one")
    config = intelgt_note(notes[0], NOTE_PRODUCT_CONFIG)
    if config is None or len(config) != PRODUCT_CONFIG_SIZE:
        raise CheckError(
            f"a native image has no 4-byte IntelGT product-config note (type "
            f"{NOTE_PRODUCT_CONFIG}) in {NOTE_SECTION}"
        )
    (value,) = struct.unpack("<I", config)
    release = (value >> IP_RELEASE_SHIFT) & IP_RELEASE_MASK
    return f"{value >> IP_ARCHITECTURE_SHIFT}.{release}.{value & IP_REVISION_MASK}"


def archive_members(blob: bytes, start: int) -> tuple[set[str], int]:
    """Return the member names of the ar archive at ``start`` and where it ends."""
    names: set[str] = set()
    position = start + len(AR_MAGIC)
    for _ in range(MAX_ARCHIVE_MEMBERS):
        header = blob[position : position + AR_HEADER]
        if len(header) < AR_HEADER or header[58:60] != b"`\n":
            break
        names.add(header[:16].decode("ascii", "replace").strip().rstrip("/"))
        position += AR_HEADER + int(header[48:58].decode("ascii").strip())
        position += (position - start) % 2
    return names, position


def zstd_frame_end(blob: bytes, start: int) -> int:
    """Return where the zstd frame at ``start`` ends, walking its block headers.

    ``--offload-compress`` stores each image as one zstd frame and the linker
    pads frames to their alignment with zero bytes, so frames must be split
    before decoding (RFC 8878 section 3.1.1).
    """
    descriptor = blob[start + 4]
    single_segment = bool(descriptor & 0x20)
    content_size_bytes = (1 if single_segment else 0, 2, 4, 8)[descriptor >> 6]
    position = start + 5 + (0 if single_segment else 1)
    position += (0, 1, 2, 4)[descriptor & 0x3] + content_size_bytes
    for _ in range(MAX_FRAME_BLOCKS):
        if position + 3 > len(blob):
            raise CheckError("truncated zstd frame in the spir64_gen section")
        header = int.from_bytes(blob[position : position + 3], "little")
        kind, size = (header >> 1) & 0x3, header >> 3
        if kind == ZSTD_RESERVED_BLOCK:
            raise CheckError("corrupt zstd block in the spir64_gen section")
        position += 3 + (1 if kind == ZSTD_RLE_BLOCK else size)
        end = position + (4 if descriptor & 0x4 else 0)
        if end > len(blob):
            raise CheckError("truncated zstd frame in the spir64_gen section")
        if header & 1:
            return end
    raise CheckError("zstd frame in the spir64_gen section has too many blocks")


def zstd_decompress(frame: bytes) -> bytes:
    """Decode one frame with Python 3.14's compression.zstd, else the zstd CLI."""
    try:
        from compression import zstd  # noqa: PLC0415 - Python >= 3.14 only
    except ImportError:
        zstd_cli = shutil.which("zstd")
        if zstd_cli is None:
            raise CheckError(
                "the AOT images are zstd-compressed (--offload-compress); decoding them "
                "needs Python >= 3.14 or the zstd command-line tool"
            ) from None
        result = subprocess.run(  # noqa: S603 -- resolved zstd path, data on stdin, no shell
            [zstd_cli, "-d", "-c", "-q"],
            input=frame,
            capture_output=True,
            check=False,
            timeout=OCLOC_TIMEOUT_SECONDS,
        )
        if result.returncode != 0:
            raise CheckError(f"zstd could not decode an AOT image: {result.stderr!r}") from None
        return result.stdout
    try:
        return zstd.decompress(frame)
    except zstd.ZstdError as error:
        raise CheckError(f"zstd could not decode an AOT image: {error}") from None


def expand(blob: bytes) -> list[bytes]:
    """Return the section's payloads: each zstd frame decoded, else the raw section as one."""
    if not blob.startswith(ZSTD_MAGIC):
        return [blob]
    payloads: list[bytes] = []
    position = 0
    while position < len(blob):
        if blob[position] == 0:
            position += 1
            continue
        if not blob.startswith(ZSTD_MAGIC, position):
            raise CheckError(f"unexpected bytes at offset {position} between zstd frames")
        end = zstd_frame_end(blob, position)
        payloads.append(zstd_decompress(blob[position:end]))
        position = end
    return payloads


def fat_image(data: bytes, base: int) -> tuple[Image, int]:
    """Parse the ocloc fat binary at ``base``; return it and where it ends."""
    names, end = archive_members(data, base)
    return Image(FAT, frozenset(name.split(".", 1)[-1] for name in names)), end


def native_image(data: bytes, base: int) -> tuple[Image, int]:
    """Parse the bare zebin at ``base``; return it, with its one IP version, and where it ends."""
    sections = elf_sections(data, base)
    end = elf_end(data, base, sections)
    if end > len(data):
        raise CheckError("a native image in the spir64_gen section is truncated")
    return Image(NATIVE, frozenset({native_ip_version(data, base, sections)})), end


def parse_images(payload: bytes) -> list[Image]:
    """Split a payload into its images, fat binaries and bare zebins in any mix.

    Zero bytes between images are alignment padding. Bytes that start neither an ``ar``
    archive nor an ELF file are an error, so a stray or unknown image cannot pass unseen.
    """
    images: list[Image] = []
    position = 0
    while position < len(payload):
        if payload[position] == 0:
            position += 1
            continue
        if payload.startswith(AR_MAGIC, position):
            image, position = fat_image(payload, position)
        elif payload.startswith(ELF_MAGIC, position):
            image, position = native_image(payload, position)
        else:
            raise CheckError(
                f"unexpected bytes at offset {position}: "
                "neither an ocloc fat binary nor a native ELF image"
            )
        images.append(image)
    return images


def section_images(section: bytes) -> list[Image]:
    """Return every image of the spir64_gen section, whichever form ocloc wrote it in."""
    return [image for payload in expand(section) for image in parse_images(payload)]


def ocloc_ip_version(ocloc: str, target: str) -> str:
    """Ask ocloc for a device acronym's GFX IP version, e.g. bmg-g21 -> 20.1.0."""
    result = subprocess.run(  # noqa: S603 -- Meson passes the resolved ocloc, no shell
        [ocloc, "ids", target],
        capture_output=True,
        text=True,
        check=False,
        timeout=OCLOC_TIMEOUT_SECONDS,
    )
    lines = result.stdout.split()
    if result.returncode != 0 or "ids:" not in lines or lines[-1] == "ids:":
        raise CheckError(f"`{ocloc} ids {target}` did not name an IP version: {result.stdout!r}")
    return lines[-1]


def count_images(images: list[Image]) -> str:
    """Name what the section holds, e.g. ``31 spir64_gen fat binaries``."""
    fat = sum(image.kind == FAT for image in images)
    parts = [f"{fat} spir64_gen fat binaries"] if fat else []
    if len(images) > fat:
        parts.append(f"{len(images) - fat} spir64_gen native images")
    return ", ".join(parts)


def check(images: list[Image], wanted: dict[str, str], partial: dict[str, set[str]]) -> str:
    """Validate the images against the per-target IP versions."""
    if not images:
        raise CheckError(f"{SECTION} holds no ocloc fat binary or native image. {REMEDY}")
    requested = set(wanted.values())
    optional = {wanted[target] for targets in partial.values() for target in targets}
    incomplete = []
    for index, image in enumerate(images):
        stray = image.ips - requested if image.kind == NATIVE else set()
        if stray:
            raise CheckError(
                f"{image.kind} {index} is built for IP version {sorted(stray)}, which no "
                f"requested target uses ({', '.join(f'{t}={ip}' for t, ip in wanted.items())}). "
                f"{REMEDY}"
            )
        missing = requested - image.ips
        if missing - optional:
            raise CheckError(
                f"{image.kind} {index} lacks IP versions {sorted(missing - optional)} "
                f"({', '.join(t for t, ip in wanted.items() if ip in missing)}). {REMEDY}"
            )
        if missing:
            incomplete.append(index)
    if len(incomplete) > len(partial):
        raise CheckError(
            f"{len(incomplete)} images omit targets, but only {len(partial)} TU(s) "
            f"are declared partial ({', '.join(sorted(partial))}). {REMEDY}"
        )
    return (
        f"{count_images(images)}; {len(requested)} IP versions "
        f"for {len(wanted)} targets; {len(incomplete)} partial by declaration"
    )


def parse_partial(values: list[str], targets: list[str]) -> dict[str, set[str]]:
    partial: dict[str, set[str]] = {}
    for value in values:
        unit, _, skipped = value.partition("=")
        names = {name for name in skipped.split(",") if name}
        unknown = names - set(targets)
        if not unit or not names:
            raise CheckError(f"--partial expects TU=target[,target], got {value!r}")
        partial[unit] = names - unknown
    return {unit: names for unit, names in partial.items() if names}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ocloc", required=True)
    parser.add_argument("--targets", required=True)
    parser.add_argument("--partial", action="append", default=[])
    parser.add_argument("--stamp", type=Path, required=True)
    parser.add_argument("library", type=Path)
    args = parser.parse_args(argv)
    targets = [target for target in args.targets.split(",") if target]
    try:
        section = elf_section(args.library.read_bytes(), SECTION)
        if section is None:
            raise CheckError(f"{args.library.name} has no {SECTION} section. {REMEDY}")
        wanted = {target: ocloc_ip_version(args.ocloc, target) for target in targets}
        summary = check(section_images(section), wanted, parse_partial(args.partial, targets))
    except (CheckError, OSError, ValueError, struct.error, subprocess.SubprocessError) as error:
        print(f"check_aot_image: {args.library}: {error}", file=sys.stderr)
        return 1
    args.stamp.write_text(summary + "\n", encoding="utf-8")
    print(f"check_aot_image: {args.library.name}: {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
