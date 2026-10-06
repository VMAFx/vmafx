#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixtures of the input format table made by FFmpeg (ADR-2145).

For every layout of the table that FFmpeg knows, a small planar frame of
pseudo-random samples is converted by FFmpeg to the layout, and the layout's
bytes are written to `core/test/vmafx_format_fixtures.h` next to the planar
samples the import must return. FFmpeg is the independent oracle: the C tests
(`test_vmafx_import_ffmpeg_oracle`) import the bytes and compare with the
planar frame sample for sample, so a layout read the way this code reads it and
not the way FFmpeg writes it fails. The header is committed;
`core/test/test_vmafx_format_fixtures_current.py` regenerates it with FFmpeg
(skipped without one) and fails when it differs.

    python3 scripts/dev/gen_format_fixtures.py --write
    python3 scripts/dev/gen_format_fixtures.py --check
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

WIDTH, HEIGHT = (
    14,
    5,
)  # 14 is not a multiple of 6 (V210's last group is partial); FFmpeg's v210 decoder fails below 4 rows
BYTE_DEPTH = 8
SPDX = "SPDX-" + "License-Identifier: EUPL-1.2"
OUTPUT = Path("core/test/vmafx_format_fixtures.h")

# (VmafxPixelFormat name, FFmpeg pixel format, planar FFmpeg format of the frame made, bits,
#  chroma width divisor, chroma height divisor, FFmpeg encoder or None)
SINGLE_PLANE = {"YUYV422", "UYVY422", "V210", "Y210", "Y212", "AYUV", "VUYX", "Y410", "XV36"}

FORMATS = [
    ("NV12", "nv12", "yuv420p", 8, 2, 2, None),
    ("P010", "p010le", "yuv420p10le", 10, 2, 2, None),
    ("P016", "p016le", "yuv420p16le", 16, 2, 2, None),
    ("NV16", "nv16", "yuv422p", 8, 2, 1, None),
    ("P210", "p210le", "yuv422p10le", 10, 2, 1, None),
    ("P216", "p216le", "yuv422p16le", 16, 2, 1, None),
    ("NV24", "nv24", "yuv444p", 8, 1, 1, None),
    ("P410", "p410le", "yuv444p10le", 10, 1, 1, None),
    ("P416", "p416le", "yuv444p16le", 16, 1, 1, None),
    ("YUYV422", "yuyv422", "yuv422p", 8, 2, 1, None),
    ("UYVY422", "uyvy422", "yuv422p", 8, 2, 1, None),
    ("V210", "yuv422p10le", "yuv422p10le", 10, 2, 1, "v210"),
    ("Y210", "y210le", "yuv422p10le", 10, 2, 1, None),
    ("Y212", "y212le", "yuv422p12le", 12, 2, 1, None),
    ("AYUV", "ayuv", "yuv444p", 8, 1, 1, None),
    ("VUYX", "vuyx", "yuv444p", 8, 1, 1, None),
    ("Y410", "xv30le", "yuv444p10le", 10, 1, 1, None),
    ("XV36", "xv36le", "yuv444p12le", 12, 1, 1, None),
    ("YUV444P_MSB", "yuv444p10msble", "yuv444p10le", 10, 1, 1, None),
    ("YUV444P_MSB", "yuv444p12msble", "yuv444p12le", 12, 1, 1, None),
]


def bytes_per_sample(bpc: int) -> int:
    return 1 if bpc == BYTE_DEPTH else 2


def planar_frame(
    bpc: int, cw: int, ch: int, seed: int, lo: int = 0, hi: int | None = None
) -> bytes:
    """Pseudo-random planar samples in [lo, hi], little-endian words above 8 bits."""
    hi = (1 << bpc) - 1 if hi is None else hi
    state = 0x9E3779B97F4A7C15 ^ seed
    out = bytearray()
    for plane_w, plane_h in ((WIDTH, HEIGHT), (cw, ch), (cw, ch)):
        for _ in range(plane_w * plane_h):
            state ^= (state << 13) & 0xFFFFFFFFFFFFFFFF
            state ^= state >> 7
            state ^= (state << 17) & 0xFFFFFFFFFFFFFFFF
            sample = lo + ((state >> 24) % (hi - lo + 1))
            out += sample.to_bytes(bytes_per_sample(bpc), "little")
    return bytes(out)


def ffmpeg(args: list[str], data: bytes) -> bytes:
    command = ["ffmpeg", "-v", "error", "-nostdin", *args]
    done = subprocess.run(  # noqa: S603 -- FFmpeg from PATH, argv built here
        command, input=data, capture_output=True, check=False
    )
    if done.returncode != 0:
        raise SystemExit(f"ffmpeg {' '.join(args)}: {done.stderr.decode(errors='replace')}")
    return done.stdout


def convert(row: tuple, planar: bytes) -> bytes:
    _, packed, planar_fmt, _, _, _, encoder = row
    base = ["-f", "rawvideo", "-pix_fmt", planar_fmt, "-s", f"{WIDTH}x{HEIGHT}", "-i", "-"]
    if encoder:
        return ffmpeg([*base, "-c:v", encoder, "-f", "rawvideo", "-"], planar)
    return ffmpeg([*base, "-pix_fmt", packed, "-f", "rawvideo", "-"], planar)


def check_roundtrip(row: tuple, packed: bytes, planar: bytes) -> None:
    """FFmpeg decodes its own bytes to the planar frame: the oracle is self-consistent."""
    _, name, planar_fmt, _, _, _, encoder = row
    size = f"{WIDTH}x{HEIGHT}"
    if encoder:
        # The raw v210 demuxer feeds the codec's decoder.
        back = ffmpeg(
            [
                "-f",
                "v210",
                "-video_size",
                size,
                "-i",
                "-",
                "-pix_fmt",
                planar_fmt,
                "-f",
                "rawvideo",
                "-",
            ],
            packed,
        )
    else:
        back = ffmpeg(
            [
                "-f",
                "rawvideo",
                "-pix_fmt",
                name,
                "-s",
                size,
                "-i",
                "-",
                "-pix_fmt",
                planar_fmt,
                "-f",
                "rawvideo",
                "-",
            ],
            packed,
        )
    if back != planar:
        raise SystemExit(f"{name}: FFmpeg's own decode differs from the source frame")


def c_bytes(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 16]) + ",")
    return "\n".join(lines)


def header_lines() -> list[str]:
    return [
        "/**",
        " *",
        " *  Copyright 2026 Lusoris",
        " *",
        f" * {SPDX}",
        " */",
        "",
        "/*",
        " * GENERATED by scripts/dev/gen_format_fixtures.py with FFmpeg (ADR-2145). Do not edit.",
        " *",
        " * One frame per input layout FFmpeg knows, written by FFmpeg from a pseudo-random",
        " * planar frame, and the planar samples the import must return (plane 0, 1, 2 in",
        " * order, tightly packed, little-endian words above 8 bits). FFmpeg is the oracle of",
        " * where each sample lives. A pitch of `pitch` bytes separates rows of `bytes`.",
        " */",
        "",
        "#ifndef VMAFX_FORMAT_FIXTURES_H",
        "#define VMAFX_FORMAT_FIXTURES_H",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "/* clang-format off */",
        f"#define VFX_WIDTH {WIDTH}u",
        f"#define VFX_HEIGHT {HEIGHT}u",
        "",
        "typedef struct VfxFixture {",
        "    const char *ffmpeg_name; /* FFmpeg's pixel format */",
        "    uint32_t pix_fmt;        /* VmafxPixelFormat */",
        "    uint32_t bpc;",
        "    size_t pitch;            /* bytes per row of the layout's plane 0 */",
        "    size_t n_bytes;",
        "    const uint8_t *bytes;    /* the layout's planes, one after another */",
        "    const uint8_t *planar;   /* the frame it makes */",
        "} VfxFixture;",
        "",
    ]


def render() -> str:
    out = header_lines()
    names = []
    for index, row in enumerate(FORMATS):
        enum, packed, _planar_fmt, bpc, dw, dh, encoder = row
        cw, ch = (WIDTH + dw - 1) // dw, (HEIGHT + dh - 1) // dh
        # V210's encoder clips to 4..1019: the codes 0 to 3 and 1020 to 1023 are reserved.
        planar = planar_frame(bpc, cw, ch, index, *((4, 1019) if encoder else (0, None)))
        data = convert(row, planar)
        check_roundtrip(row, data, planar)
        label = (encoder or packed).replace("-", "_")
        out += [f"static const uint8_t vfx_{label}_bytes[] = {{", c_bytes(data), "};", ""]
        out += [f"static const uint8_t vfx_{label}_planar[] = {{", c_bytes(planar), "};", ""]
        # Single-plane layouts pad their rows (V210); the others are tightly packed planes.
        pitch = len(data) // HEIGHT if enum in SINGLE_PLANE else WIDTH * bytes_per_sample(bpc)
        names.append((label, enum, packed, bpc, pitch, len(data)))
    out += ["static const VfxFixture vfx_fixtures[] = {"]
    for label, enum, packed, bpc, pitch, size in names:
        out += [
            f'    {{"{packed}", VMAFX_PIXEL_FORMAT_{enum}, {bpc}u, {pitch}u, {size}u, '
            f"vfx_{label}_bytes, vfx_{label}_planar}},"
        ]
    out += [
        "};",
        "",
        "#define VFX_N_FIXTURES (sizeof(vfx_fixtures) / sizeof(vfx_fixtures[0]))",
        "/* clang-format on */",
        "",
        "#endif /* VMAFX_FORMAT_FIXTURES_H */",
        "",
    ]
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    if shutil.which("ffmpeg") is None:
        print("SKIP: gen_format_fixtures: no ffmpeg on PATH", file=sys.stderr)
        return 77
    text = render()
    target = args.root / OUTPUT
    if args.write:
        target.write_text(text, encoding="utf-8")
        print(f"wrote {OUTPUT}")
        return 0
    if not target.exists() or target.read_text(encoding="utf-8") != text:
        print(f"{OUTPUT} differs from what FFmpeg produces; run with --write", file=sys.stderr)
        return 1
    print(f"{OUTPUT} matches FFmpeg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
