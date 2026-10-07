# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Checks of the input format table (`[[pixel_formats]]`, ADR-2145).

The table and the `VmafxPixelFormat` enum must say the same thing: every enum
value but UNKNOWN has exactly one row, every row names an enum value, and the
fields of a row agree with each other (a layout fixes the planes, a chroma
format the planar frame it makes, a packing the element positions). A row that
breaks one stops generation naming the row.
"""

from __future__ import annotations

from collections.abc import Callable
from fractions import Fraction

from .model import Api, DefinitionError, PixelFormat

LAYOUTS = ("planar", "semi_planar", "packed", "rgb")
CHROMA = {"400": "YUV400P", "420": "YUV420P", "422": "YUV422P", "444": "YUV444P"}
SITINGS = ("none", "left")
PACKINGS = ("none", "yuyv", "uyv4", "xvyu2101010", "v210", "rgb")
DEVICES = ("cpu", "cuda", "sycl", "hip", "metal")
ENUM = "VmafxPixelFormat"
SEMI_PLANAR_PLANES = 2  # luma and one plane of interleaved Cb / Cr
BPC_MIN, BPC_MAX = 8, 16
PIXEL_FORMAT_PREFIX = "VMAFX_PIXEL_FORMAT_"


def _planes_ok(row: PixelFormat) -> bool:
    if row.layout == "planar":
        return row.planes == (1 if row.chroma == "400" else 3)
    if row.layout == "semi_planar":
        return row.planes == SEMI_PLANAR_PLANES and row.interleaved
    return row.planes == 1


def _check_layout(row: PixelFormat, where: str) -> None:
    if row.layout not in LAYOUTS or row.siting not in SITINGS or row.chroma not in CHROMA:
        raise DefinitionError(f"{where}: layout, siting or chroma is not a known value")
    if not _planes_ok(row):
        raise DefinitionError(f"{where}: {row.planes} planes do not fit a {row.layout} layout")
    if (row.packing != "none") != (row.layout in ("packed", "rgb")):
        raise DefinitionError(f"{where}: `packing` belongs to the packed and rgb layouts only")
    if row.packing not in PACKINGS or (row.layout == "rgb") != (row.packing == "rgb"):
        raise DefinitionError(f"{where}: packing {row.packing!r} does not fit layout {row.layout}")
    if row.needs_statement != (row.layout == "rgb"):
        raise DefinitionError(f"{where}: `needs_statement` is true for rgb rows and only those")
    if row.layout == "rgb" and (row.rgb_elems not in (3, 4) or sorted(row.elem) != [0, 1, 2]):
        raise DefinitionError(f"{where}: an rgb row has 3 or 4 elements and R, G, B in 0 to 2")
    if row.layout == "rgb" and row.chroma != "444":
        raise DefinitionError(f"{where}: an rgb frame is converted to 4:4:4")
    if row.packing in ("yuyv", "uyv4") and sorted(row.elem) != sorted(set(row.elem)):
        raise DefinitionError(f"{where}: `elem` positions repeat")


def _check_depths(row: PixelFormat, where: str) -> None:
    low, high = row.bpc
    if not BPC_MIN <= low <= high <= BPC_MAX:
        raise DefinitionError(f"{where}: bpc {list(row.bpc)} is not inside 8 to 16")
    for surface, names in (("ffmpeg", row.ffmpeg), ("gstreamer", row.gstreamer)):
        for depth in names:
            if not low <= depth <= high:
                raise DefinitionError(
                    f"{where}: {surface} names bpc {depth}, outside {list(row.bpc)}"
                )
    for device in row.devices:
        if device not in DEVICES:
            raise DefinitionError(f"{where}: device {device!r} is not one of {DEVICES}")
    if "cpu" not in row.devices:
        raise DefinitionError(f"{where}: every format is read on the CPU device")


def _enum_values(api: Api) -> dict[str, int]:
    for enum in api.enums:
        if enum.name == ENUM:
            return {v.name: v.value for v in enum.values}
    raise DefinitionError(f"enum {ENUM} is missing")


def _check_unique(api: Api) -> None:
    pickers: tuple[tuple[str, Callable[[PixelFormat], list[str]]], ...] = (
        ("enum", lambda r: [r.enum]),
        ("name", lambda r: [r.name]),
        ("ffmpeg name", lambda r: list(r.ffmpeg.values())),
        ("gstreamer name", lambda r: list(r.gstreamer.values())),
    )
    for what, pick in pickers:
        seen: set[str] = set()
        for row in api.pixel_formats:
            for item in pick(row):
                if item in seen:
                    raise DefinitionError(f"pixel_formats: duplicate {what} {item}")
                seen.add(item)


def check_matrices(api: Api) -> None:
    """Every `[[color_matrices]]` row names a VmafxColorMatrix value and has luma weights in (0, 1)."""
    values = {v.name for enum in api.enums if enum.name == "VmafxColorMatrix" for v in enum.values}
    seen: set[str] = set()
    for row in api.color_matrices:
        where = f"color_matrices[{row.enum}]"
        if row.enum not in values or row.enum in seen:
            raise DefinitionError(f"{where}: not a VmafxColorMatrix value, or listed twice")
        seen.add(row.enum)
        try:
            kr, kb = Fraction(row.kr), Fraction(row.kb)
        except ValueError as err:
            raise DefinitionError(f"{where}: kr and kb are exact decimals") from err
        if not (0 < kr < 1 and 0 < kb < 1 and kr + kb < 1):
            raise DefinitionError(f"{where}: kr and kb must be positive and sum below 1")


def check(api: Api) -> None:
    """Refuse a table that disagrees with the enum or with itself."""
    check_matrices(api)
    if not api.pixel_formats:
        return
    values = _enum_values(api)
    named = {name for name in values if name != PIXEL_FORMAT_PREFIX + "UNKNOWN"}
    rows = {row.enum for row in api.pixel_formats}
    for missing in sorted(named - rows):
        raise DefinitionError(f"pixel_formats: no row for {missing}")
    for stray in sorted(rows - named):
        raise DefinitionError(f"pixel_formats: {stray} is not a VmafxPixelFormat value")
    _check_unique(api)
    for row in api.pixel_formats:
        where = f"pixel_formats[{row.enum}]"
        _check_layout(row, where)
        _check_depths(row, where)
        planar = PIXEL_FORMAT_PREFIX + CHROMA[row.chroma]
        if row.planar != planar or row.planar not in values:
            raise DefinitionError(f"{where}: a {row.chroma} frame is {planar}, not {row.planar}")
