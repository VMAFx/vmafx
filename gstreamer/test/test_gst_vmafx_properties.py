#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The vmafx GStreamer element's properties are the definition's options (RC4 WP8, #2474).

Expected set: every FFmpeg spelling of every option of ``core/api/vmafx.toml`` that names the
``ffmpeg`` surface, aliases included, with ``_`` written ``-`` (the rule of
``gst_vmafx_opts_install()`` in ``gstreamer/src/gstvmafx_opts.c``).

Checked against two surfaces:

* the property table of ``docs/usage/gstreamer.md`` (always runs);
* the properties the element itself installs, read through GObject introspection
  (needs ``gi`` and ``GST_VMAFX_PLUGIN_DIR``, set by run.sh; skips with the reason otherwise).

A planted extra or missing property fails the comparison; those cases always run.
"""

from __future__ import annotations

import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "codegen"))
from vmafx_api.emit_ffmpeg_options import ffmpeg_options  # noqa: E402 -- path set above
from vmafx_api.loader import load  # noqa: E402

DEFINITION = ROOT / "core" / "api" / "vmafx.toml"
DOCS = ROOT / "docs" / "usage" / "gstreamer.md"
ROW = re.compile(r"^\| `([a-z0-9-]+)` \|")


def expected_properties() -> set[str]:
    """Property names the definition implies for the element."""
    names: set[str] = set()
    for option in ffmpeg_options(load(DEFINITION)):
        names.update(name.replace("_", "-") for name in option.spellings["ffmpeg"])
    return names


def property_diff(expected: set[str], actual: set[str]) -> list[str]:
    """One line per property that is extra or missing; empty when the sets are equal."""
    lines = [f"extra property not in the definition: {n}" for n in sorted(actual - expected)]
    lines += [f"missing property of the definition: {n}" for n in sorted(expected - actual)]
    return lines


def documented_properties(text: str) -> set[str]:
    """Property names of the table under the Properties heading of the user page."""
    section = text.split("## Properties", 1)[1].split("\n## ", 1)[0]
    return {m.group(1) for line in section.splitlines() if (m := ROW.match(line))}


def element_properties(plugin_dir: str) -> set[str]:
    """Properties the vmafx element class installs itself (aggregator properties excluded)."""
    os.environ["GST_PLUGIN_PATH"] = plugin_dir
    os.environ["GST_REGISTRY"] = str(
        Path(tempfile.gettempdir()) / f"gst-vmafx-props-{os.getuid()}.bin"
    )
    import gi  # noqa: PLC0415 -- optional, imported only when the element test runs

    gi.require_version("Gst", "1.0")
    from gi.repository import Gst  # noqa: PLC0415

    Gst.init(None)
    element = Gst.ElementFactory.make("vmafx", None)
    if element is None:
        raise RuntimeError(f"no vmafx element in {plugin_dir}")
    own = type(element).__gtype__
    return {p.name for p in element.list_properties() if p.owner_type == own}


class PropertyDiffTest(unittest.TestCase):
    def setUp(self) -> None:
        self.expected = expected_properties()

    def test_expected_set_has_aliases(self) -> None:
        self.assertIn("n-threads", self.expected)
        self.assertIn("tiny-model", self.expected)
        self.assertNotIn("tiny_model", self.expected)

    def test_equal_sets_pass(self) -> None:
        self.assertEqual(property_diff(self.expected, set(self.expected)), [])

    def test_planted_extra_property_fails(self) -> None:
        diff = property_diff(self.expected, self.expected | {"planted-extra"})
        self.assertEqual(diff, ["extra property not in the definition: planted-extra"])

    def test_planted_missing_property_fails(self) -> None:
        diff = property_diff(self.expected, self.expected - {"threads"})
        self.assertEqual(diff, ["missing property of the definition: threads"])

    def test_documented_table_equals_definition(self) -> None:
        documented = documented_properties(DOCS.read_text(encoding="utf-8"))
        self.assertEqual(property_diff(self.expected, documented), [])

    def test_planted_extra_row_in_docs_fails(self) -> None:
        text = DOCS.read_text(encoding="utf-8").replace(
            "| `model` |", "| `planted-row` | string | unset | x |\n| `model` |", 1
        )
        diff = property_diff(self.expected, documented_properties(text))
        self.assertEqual(diff, ["extra property not in the definition: planted-row"])


class ElementPropertyTest(unittest.TestCase):
    def test_element_properties_equal_definition(self) -> None:
        plugin_dir = os.environ.get("GST_VMAFX_PLUGIN_DIR")
        if not plugin_dir:
            self.skipTest("GST_VMAFX_PLUGIN_DIR not set: run gstreamer/test/run.sh")
        try:
            import gi  # noqa: F401, PLC0415
        except ImportError:
            self.skipTest("python gi (PyGObject) not installed")
        actual = element_properties(plugin_dir)
        self.assertEqual(property_diff(expected_properties(), actual), [])


if __name__ == "__main__":
    unittest.main()
