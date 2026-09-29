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
* in every ocloc fat binary inside it (one ``ar`` archive per TU image, members
  named ``<bits>.<GFX IP version>``), a member for the IP version of every
  requested target, as ``ocloc ids <target>`` reports it. The build compresses
  each image with ``--offload-compress``; zstd frames are decoded first.

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

SECTION = "__CLANG_OFFLOAD_BUNDLE__sycl-spir64_gen"
AR_MAGIC = b"!<arch>\n"
AR_HEADER = 60
ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
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


def elf_section(data: bytes, wanted: str) -> bytes | None:
    """Return the contents of the named section of an ELF64 little-endian file."""
    if data[:4] != ELF_MAGIC or data[4] != ELFCLASS64 or data[5] != ELFDATA2LSB:
        raise CheckError("not an ELF64 little-endian object")
    (shoff,) = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    headers = [struct.unpack_from("<IIQQQQ", data, shoff + i * shentsize) for i in range(shnum)]
    names_offset = headers[shstrndx][4]
    for name_offset, _kind, _flags, _addr, offset, size in headers:
        start = names_offset + name_offset
        name = data[start : data.index(b"\0", start)].decode("ascii", "replace")
        if name == wanted:
            return data[offset : offset + size]
    return None


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
        position += position % 2
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


def expand(blob: bytes) -> bytes:
    """Return the section with any zstd-compressed images decoded in place."""
    if not blob.startswith(ZSTD_MAGIC):
        return blob
    images: list[bytes] = []
    position = 0
    while position < len(blob):
        if blob[position] == 0:
            position += 1
            continue
        if not blob.startswith(ZSTD_MAGIC, position):
            raise CheckError(f"unexpected bytes at offset {position} between zstd frames")
        end = zstd_frame_end(blob, position)
        images.append(zstd_decompress(blob[position:end]))
        position = end
    return b"".join(images)


def fat_binaries(blob: bytes) -> list[set[str]]:
    """Split the section into its ocloc fat binaries and list each one's members."""
    blob = expand(blob)
    archives: list[set[str]] = []
    start = blob.find(AR_MAGIC)
    while start >= 0:
        names, end = archive_members(blob, start)
        archives.append(names)
        start = blob.find(AR_MAGIC, max(end, start + len(AR_MAGIC)))
    return archives


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


def check(archives: list[set[str]], wanted: dict[str, str], partial: dict[str, set[str]]) -> str:
    """Validate the fat binaries against the per-target IP versions."""
    if not archives:
        raise CheckError(f"{SECTION} holds no ocloc fat binary. {REMEDY}")
    optional = {wanted[target] for targets in partial.values() for target in targets}
    member_ips = [{name.split(".", 1)[-1] for name in names} for names in archives]
    incomplete = []
    for index, ips in enumerate(member_ips):
        missing = set(wanted.values()) - ips
        if missing - optional:
            raise CheckError(
                f"fat binary {index} lacks IP versions {sorted(missing - optional)} "
                f"({', '.join(t for t, ip in wanted.items() if ip in missing)}). {REMEDY}"
            )
        if missing:
            incomplete.append(index)
    if len(incomplete) > len(partial):
        raise CheckError(
            f"{len(incomplete)} fat binaries omit targets, but only {len(partial)} TU(s) "
            f"are declared partial ({', '.join(sorted(partial))}). {REMEDY}"
        )
    return (
        f"{len(archives)} spir64_gen fat binaries; {len(set(wanted.values()))} IP versions "
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
        summary = check(fat_binaries(section), wanted, parse_partial(args.partial, targets))
    except (CheckError, OSError, ValueError, struct.error, subprocess.SubprocessError) as error:
        print(f"check_aot_image: {args.library}: {error}", file=sys.stderr)
        return 1
    args.stamp.write_text(summary + "\n", encoding="utf-8")
    print(f"check_aot_image: {args.library.name}: {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
