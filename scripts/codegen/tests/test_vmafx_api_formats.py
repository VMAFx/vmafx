#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The input format table and the RGB conversion model (ADR-2145, ADR-2146).

Positive: the table agrees with the enum, the FFmpeg and GStreamer lists are
the rows that qualify, the format names are names the tools know (FFmpeg's
`-pix_fmts`, GStreamer's format list, when installed), the committed fixtures
are what FFmpeg and the exact-rational oracle produce, and the exact model of
the RGB conversion agrees with zimg (FFmpeg's zscale) for full-range input.
Negative: every check refuses its planted defect (a format without an enum
value, an enum value without a row, a duplicate name, a missing generated
list), and a committed list that lacks a planted extra format fails the drift
check. Boundary: gray is gray in every matrix; the coefficient rows sum as the
conversion needs.
"""

from __future__ import annotations

import copy
import re
import struct
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

from support import ROOT, document, parse, quiet, render_into, run, tool
from vmafx_api import cli, emit_formats, rgb_coefficients
from vmafx_api.model import DefinitionError


def live_doc() -> dict:
    return copy.deepcopy(document())


def refused(doc: dict) -> str:
    try:
        parse(doc)
    except DefinitionError as err:
        return str(err)
    return ""


class TableTest(unittest.TestCase):
    def test_every_enum_value_has_one_row(self) -> None:
        api = parse(live_doc())
        enum = next(e for e in api.enums if e.name == "VmafxPixelFormat")
        names = {v.name for v in enum.values if v.value != 0}
        self.assertEqual(names, {row.enum for row in api.pixel_formats})

    def test_a_row_without_an_enum_value_is_refused(self) -> None:
        doc = live_doc()
        doc["pixel_formats"].append({**doc["pixel_formats"][0], "enum": "VMAFX_PIXEL_FORMAT_ZZ"})
        self.assertIn("not a VmafxPixelFormat value", refused(doc))

    def test_an_enum_value_without_a_row_is_refused(self) -> None:
        doc = live_doc()
        enum = next(e for e in doc["enums"] if e["name"] == "VmafxPixelFormat")
        enum["values"].append({"name": "VMAFX_PIXEL_FORMAT_ZZ", "value": 99, "doc": "x"})
        self.assertIn("no row for VMAFX_PIXEL_FORMAT_ZZ", refused(doc))

    def test_duplicate_names_are_refused(self) -> None:
        doc = live_doc()
        doc["pixel_formats"][1]["ffmpeg"]["8"] = doc["pixel_formats"][0]["ffmpeg"]["8"]
        self.assertIn("duplicate ffmpeg name", refused(doc))
        doc = live_doc()
        doc["pixel_formats"][1]["gstreamer"]["8"] = doc["pixel_formats"][0]["gstreamer"]["8"]
        self.assertIn("duplicate gstreamer name", refused(doc))

    def test_inconsistent_rows_are_refused(self) -> None:
        for key, value, needle in (
            ("planar", "VMAFX_PIXEL_FORMAT_YUV444P", "a 420 frame is"),
            ("planes", 2, "planes do not fit"),
            ("bpc", [8, 20], "not inside 8 to 16"),
            ("devices", ["cuda"], "read on the CPU"),
            ("siting", "centre", "not a known value"),
        ):
            doc = live_doc()
            doc["pixel_formats"][0][key] = value
            self.assertIn(needle, refused(doc), key)
        doc = live_doc()
        rgb = next(r for r in doc["pixel_formats"] if r["layout"] == "rgb")
        rgb["needs_statement"] = False
        self.assertIn("needs_statement", refused(doc))

    def test_a_ffmpeg_depth_outside_the_row_is_refused(self) -> None:
        doc = live_doc()
        doc["pixel_formats"][4]["ffmpeg"] = {"10": "nv12"}
        self.assertIn("outside", refused(doc))


class ListTest(unittest.TestCase):
    def setUp(self) -> None:
        self.api = parse(live_doc())

    def test_ffmpeg_lists_are_the_rows_that_qualify(self) -> None:
        text = emit_formats.ffmpeg_formats(self.api)
        self.assertIn("AV_PIX_FMT_GRAY8", text)  # `gray` is GRAY8
        self.assertIn("AV_PIX_FMT_YUV444P12MSBLE", text)
        yuv = text.split("#define VMAFX_FFMPEG_RGB_PIX_FMTS")[0]
        self.assertNotIn("AV_PIX_FMT_RGBA", yuv)  # RGB is a list of its own
        self.assertNotIn("AV_PIX_FMT_V210", text)  # FFmpeg has no V210 pixel format
        cuda = text.split("#define VMAFX_FFMPEG_YUV_CUDA_SW_FORMATS")[1].split("#define")[0]
        self.assertIn("AV_PIX_FMT_NV12", cuda)

    def test_a_device_without_the_format_does_not_list_it(self) -> None:
        doc = live_doc()
        for row in doc["pixel_formats"]:
            if row["name"] == "nv16":
                row["devices"] = ["cpu"]
        text = emit_formats.ffmpeg_formats(parse(doc))
        cuda = text.split("#define VMAFX_FFMPEG_YUV_CUDA_SW_FORMATS")[1].split("#define")[0]
        self.assertNotIn("AV_PIX_FMT_NV16,", cuda)
        self.assertIn("AV_PIX_FMT_NV16", text.split("#define VMAFX_FFMPEG_YUV_PIX_FMTS")[1])

    def test_gstreamer_lists_carry_every_name(self) -> None:
        text = emit_formats.gstreamer_formats(self.api)
        for row in self.api.pixel_formats:
            for name in row.gstreamer.values():
                self.assertIn(name, text)

    def test_a_planted_extra_format_fails_the_drift_check(self) -> None:
        """The committed lists are what the definition produces: one more row and they differ."""
        doc = live_doc()
        enum = next(e for e in doc["enums"] if e["name"] == "VmafxPixelFormat")
        enum["values"].append({"name": "VMAFX_PIXEL_FORMAT_ZZ", "value": 99, "doc": "x"})
        extra = {
            **copy.deepcopy(doc["pixel_formats"][0]),
            "enum": "VMAFX_PIXEL_FORMAT_ZZ",
            "name": "zz",
            "ffmpeg": {"8": "zz8"},
            "gstreamer": {"8": "ZZ8"},
        }
        doc["pixel_formats"].append(extra)
        planted = cli.render(parse(doc))
        live = cli.render(parse(live_doc()))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            quiet(cli.write, root, live)
            self.assertEqual(quiet(cli.check, root, live), 0)
            self.assertEqual(quiet(cli.check, root, planted), 1)
        differing = {p for p in planted if planted[p] != live.get(p)}
        for path in (
            "core/src/vmafx/import_layouts_gen.h",
            "core/api/generated/vmafx_ffmpeg_formats.h",
            "core/api/generated/vmafx_gstreamer_formats.h",
            "docs/usage/pixel-formats.md",
        ):
            self.assertIn(path, differing)

    def test_generated_files_are_committed(self) -> None:
        for path, text in emit_formats.outputs(self.api):
            self.assertEqual((ROOT / path).read_text(encoding="utf-8"), text, path)
        render_into  # noqa: B018 -- imported for the sibling tests' helpers


class KnownNamesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.api = parse(live_doc())

    def test_ffmpeg_knows_every_name(self) -> None:
        if tool("ffmpeg") is None:
            self.skipTest("no ffmpeg")
        listing = run(["ffmpeg", "-hide_banner", "-pix_fmts"]).stdout
        known = {line.split()[1] for line in listing.splitlines() if re.match(r"^[I.][O.]", line)}
        names = {n for row in self.api.pixel_formats for n in row.ffmpeg.values()}
        self.assertEqual(sorted(names - known), [])

    def test_ffmpeg_has_the_macros(self) -> None:
        header = Path("/usr/include/libavutil/pixfmt.h")
        if not header.exists():
            self.skipTest("no libavutil headers")
        text = header.read_text(encoding="utf-8")
        for row in self.api.pixel_formats:
            for name in row.ffmpeg.values():
                self.assertRegex(text, rf"\b{emit_formats.ffmpeg_macro(name)}\b", name)

    def test_gstreamer_knows_every_name(self) -> None:
        header = Path("/usr/include/gstreamer-1.0/gst/video/video-format.h")
        if not header.exists():
            self.skipTest("no GStreamer video headers")
        text = header.read_text(encoding="utf-8")
        known = set(re.findall(r"[A-Za-z0-9_]+", " ".join(re.findall(r'"([^"]+)"', text))))
        names = {n for row in self.api.pixel_formats for n in row.gstreamer.values()}
        self.assertEqual(sorted(names - known), [])


class FixtureTest(unittest.TestCase):
    def test_format_fixtures_are_what_ffmpeg_writes(self) -> None:
        if tool("ffmpeg") is None:
            self.skipTest("no ffmpeg")
        done = run(["python3", str(ROOT / "scripts/dev/gen_format_fixtures.py"), "--check"])
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_rgb_oracle_is_current(self) -> None:
        done = run(["python3", str(ROOT / "scripts/dev/gen_rgb_oracle.py"), "--check"])
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


MATRICES = {
    "bt601": (Fraction("0.299"), Fraction("0.114"), "470bg"),
    "bt709": (Fraction("0.2126"), Fraction("0.0722"), "709"),
    "bt2020": (Fraction("0.2627"), Fraction("0.0593"), "2020_ncl"),
}


class CoefficientTest(unittest.TestCase):
    def test_rows_sum_as_the_conversion_needs(self) -> None:
        for kr, kb, _ in MATRICES.values():
            for rin in rgb_coefficients.RANGES:
                for rout in rgb_coefficients.RANGES:
                    for bpc in rgb_coefficients.DEPTHS:
                        y, cb, cr = rgb_coefficients.matrix(kr, kb, rin, rout, bpc)
                        a_y = Fraction(
                            rgb_coefficients.swing_luma(rout, bpc),
                            rgb_coefficients.swing_luma(rin, bpc),
                        )
                        self.assertEqual(sum(y), round(a_y * (1 << rgb_coefficients.Q)))
                        self.assertEqual(sum(cb), 0)  # a gray has no chroma
                        self.assertEqual(sum(cr), 0)

    def test_limited_to_limited_is_depth_independent(self) -> None:
        for kr, kb, _ in MATRICES.values():
            rows = {
                str(rgb_coefficients.matrix(kr, kb, "limited", "limited", b))
                for b in rgb_coefficients.DEPTHS
            }
            self.assertEqual(len(rows), 1)

    def test_the_model_agrees_with_zimg(self) -> None:
        """Full-range RGB input against zimg (zscale), 16 bits, every matrix and output range."""
        if (
            tool("ffmpeg") is None
            or "zscale" not in run(["ffmpeg", "-hide_banner", "-filters"]).stdout
        ):
            self.skipTest("no ffmpeg with zscale")
        width = height = 32
        n = width * height
        state, pixels = 0x1234567, []
        for _ in range(n):
            triple = []
            for _ in range(3):
                state = (state * 6364136223846793005 + 1442695040888963407) % (1 << 64)
                triple.append((state >> 33) % 65536)
            pixels.append(tuple(triple))
        raw = b"".join(struct.pack("<HHH", *p) for p in pixels)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "rgb.raw"
            path.write_bytes(raw)
            for kr, kb, zname in MATRICES.values():
                for rout, zrange in (("limited", "limited"), ("full", "full")):
                    vf = (
                        f"zscale=rangein=full:range={zrange}:matrixin=gbr:matrix={zname}:"
                        "primariesin=709:primaries=709:transferin=709:transfer=709:"
                        "dither=none:filter=point,format=yuv444p16le"
                    )
                    done = run(
                        [
                            "ffmpeg",
                            "-v",
                            "error",
                            "-y",
                            "-f",
                            "rawvideo",
                            "-pix_fmt",
                            "rgb48le",
                            "-s",
                            f"{width}x{height}",
                            "-i",
                            str(path),
                            "-vf",
                            vf,
                            "-f",
                            "rawvideo",
                            "-pix_fmt",
                            "yuv444p16le",
                            str(Path(tmp) / "o.yuv"),
                        ]
                    )
                    self.assertEqual(done.returncode, 0, done.stderr)
                    out = struct.unpack(f"<{3 * n}H", (Path(tmp) / "o.yuv").read_bytes())
                    worst = 0.0
                    for i, px in enumerate(pixels):
                        exact = rgb_coefficients.exact(kr, kb, "full", rout, 16, px)
                        for k in range(3):
                            worst = max(worst, abs(out[k * n + i] - float(exact[k])))
                    self.assertLessEqual(worst, 0.51, f"{zname} {rout}")


if __name__ == "__main__":
    unittest.main()
