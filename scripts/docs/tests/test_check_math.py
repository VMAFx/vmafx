#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for the documentation formula gate (ADR-2705).

Covers scripts/docs/check_math.py (compiles every formula of a built site with
the vendored KaTeX in strict mode), scripts/docs/vendor_katex.py (writes the
vendored directory from the pinned npm tarball) and the Markdown syntax
(``pymdownx.arithmatex``, generic) that produces the elements the gate reads.

The planted-error case is the proof that the gate can fail: a page whose
formula KaTeX rejects must exit 1 and name the page. Cases that run KaTeX skip,
with the reason, when Node.js is not on PATH.
"""

import contextlib
import hashlib
import io
import json
import shutil
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import markdown  # type: ignore[import-untyped]  # MkDocs dependency, ships no stubs

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.docs import check_math, check_vendored_assets, mkdocs_math_hook, vendor_katex
from scripts.docs.vendor_fonts import VendorError

ROOT = Path(__file__).resolve().parents[3]
NODE = shutil.which("node")
NEEDS_NODE = unittest.skipUnless(NODE, "Node.js is not on PATH; the KaTeX cases cannot run")


def page(*fragments: str) -> str:
    return "<html><body>" + "".join(fragments) + "</body></html>"


def inline(tex: str) -> str:
    return f'<span class="arithmatex">\\({tex}\\)</span>'


def display(tex: str) -> str:
    return f'<div class="arithmatex">\\[{tex}\\]</div>'


class CheckMathTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-math-test-")
        self.addCleanup(self.temporary.cleanup)
        self.site = Path(self.temporary.name)

    def write(self, name: str, text: str) -> None:
        target = self.site / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def run_check(self) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = check_math.main(["--site", str(self.site)])
        return status, out.getvalue(), err.getvalue()

    @NEEDS_NODE
    def test_valid_formulas_pass(self) -> None:
        self.write(
            "metrics/psnr/index.html",
            page(
                inline(r"10 \log_{10} \frac{\mathrm{peak}^2}{\mathrm{mse}}"),
                display(r"\sum_{i,j} \lvert a_{ij} \rvert"),
            ),
        )
        status, out, err = self.run_check()
        self.assertEqual((status, err), (0, ""))
        self.assertIn("2 formulas", out)

    @NEEDS_NODE
    def test_planted_error_fails_and_names_the_page(self) -> None:
        # The negative test of the gate: without it working this exits 0.
        self.write("good.html", page(inline(r"x^2")))
        self.write("metrics/broken/index.html", page(inline(r"\frac{1}{")))
        status, _, err = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("broken", err)
        self.assertNotIn("good.html", err)

    @NEEDS_NODE
    def test_unknown_command_fails(self) -> None:
        self.write("a.html", page(display(r"\notacommand{x}")))
        self.assertEqual(self.run_check()[0], 1)

    @NEEDS_NODE
    def test_strict_mode_rejects_what_the_runtime_would_forgive(self) -> None:
        # Unicode text in math mode is a strict-mode error; non-strict KaTeX
        # only warns, so the site would render it and the gate would not see it.
        self.write("a.html", page(inline("\u00e9 + 1")))
        self.assertEqual(self.run_check()[0], 1)

    @NEEDS_NODE
    def test_escaped_less_than_is_unescaped_before_compiling(self) -> None:
        self.write("a.html", page(inline("a &lt; b &amp;&amp; c")))
        # `&&` outside an alignment environment is a KaTeX error, `<` alone is not.
        self.assertEqual(self.run_check()[0], 1)
        self.write("a.html", page(inline("a &lt; b")))
        self.assertEqual(self.run_check()[0], 0)

    @NEEDS_NODE
    def test_site_without_formulas_passes(self) -> None:
        self.write("a.html", page("<p>no math here</p>"))
        status, out, _ = self.run_check()
        self.assertEqual(status, 0)
        self.assertIn("0 formulas", out)

    def test_missing_site_is_a_usage_error(self) -> None:
        self.site = self.site / "does-not-exist"
        self.assertEqual(self.run_check()[0], 2)

    def test_missing_node_fails_closed(self) -> None:
        self.write("a.html", page(inline("x")))
        with mock.patch("shutil.which", return_value=None):
            status, _, err = self.run_check()
        self.assertEqual(status, 3)
        self.assertIn("NOT checked", err)

    def test_parse_formulas_reads_both_delimiters(self) -> None:
        found = check_math.parse_formulas(
            Path("p.html"), page(inline("a"), display("b"), "<p>$c$</p>")
        )
        self.assertEqual([(f.tex, f.display) for f in found], [("a", False), ("b", True)])


class ArithmatexSyntaxTests(unittest.TestCase):
    """The Markdown the contributor guide documents yields the elements the gate reads."""

    def render(self, text: str) -> str:
        rendered: str = markdown.markdown(
            text,
            extensions=["pymdownx.arithmatex"],
            extension_configs={"pymdownx.arithmatex": {"generic": True}},
        )
        return rendered

    def test_inline_and_display_dollars(self) -> None:
        html_out = self.render("Inline $x^2$ and\n\n$$\ny = \\frac{1}{2}\n$$\n")
        found = check_math.parse_formulas(Path("p.html"), html_out)
        self.assertEqual([f.display for f in found], [False, True])
        self.assertEqual(found[0].tex, "x^2")

    def test_prices_are_not_math(self) -> None:
        self.assertEqual(
            check_math.parse_formulas(Path("p.html"), self.render("It costs $5 and $10 today.")), []
        )

    def test_code_spans_are_not_math(self) -> None:
        self.assertEqual(
            check_math.parse_formulas(Path("p.html"), self.render("Run `echo $HOME and $PWD`.")), []
        )


class FrozenPageHookTests(unittest.TestCase):
    def test_frozen_paths(self) -> None:
        for path in (
            "adr/0001-x.md",
            "research/1-a.md",
            "rebase-notes.md",
            "state.md",
            "changelog-archive/a.md",
        ):
            self.assertTrue(mkdocs_math_hook.is_frozen(path), path)
        for path in (
            "metrics/psnr.md",
            "development/rebase-sensitive-invariants.md",
            "support-vmafx.md",
        ):
            self.assertFalse(mkdocs_math_hook.is_frozen(path), path)

    def test_dollars_come_back(self) -> None:
        html_in = page(inline("a"), display("b"))
        out = mkdocs_math_hook.restore_dollars(html_in)
        self.assertEqual(out, page("$a$", "$$b$$"))
        self.assertEqual(check_math.parse_formulas(Path("p.html"), out), [])


class VendorKatexTests(unittest.TestCase):
    CSS = (
        b'@font-face{src:url(fonts/A.woff2) format("woff2"),url(fonts/A.woff) format("woff"),'
        b'url(fonts/A.ttf) format("truetype")}'
    )

    def archive(self, css: bytes) -> tuple[bytes, dict[str, Any]]:
        members = {
            "package/dist/katex.min.css": css,
            "package/dist/fonts/A.woff2": b"font",
            "package/LICENSE": b"MIT",
        }
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w:gz") as tf:
            for name, data in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                tf.addfile(info, io.BytesIO(data))
        data = raw.getvalue()
        manifest = {
            "source_sha256": hashlib.sha256(data).hexdigest(),
            "source_integrity": vendor_katex.npm_integrity(data),
            "files": {
                "katex.min.css": {"from": "package/dist/katex.min.css"},
                "fonts/A.woff2": {"from": "package/dist/fonts/A.woff2"},
                "LICENSE.txt": {"from": "package/LICENSE"},
            },
        }
        return data, manifest

    def test_build_keeps_woff2_and_drops_the_fallbacks(self) -> None:
        data, manifest = self.archive(self.CSS)
        built = vendor_katex.build_files(manifest, data)
        self.assertNotIn(b".woff)", built["katex.min.css"])
        self.assertNotIn(b".ttf", built["katex.min.css"])
        self.assertIn(b"A.woff2", built["katex.min.css"])
        self.assertEqual(built["fonts/A.woff2"], b"font")

    def test_wrong_archive_hash_is_refused(self) -> None:
        data, manifest = self.archive(self.CSS)
        manifest["source_sha256"] = "0" * 64
        with self.assertRaises(VendorError):
            vendor_katex.build_files(manifest, data)

    def test_wrong_npm_integrity_is_refused(self) -> None:
        data, manifest = self.archive(self.CSS)
        manifest["source_integrity"] = "sha512-" + "A" * 86 + "=="
        with self.assertRaises(VendorError):
            vendor_katex.build_files(manifest, data)

    def test_css_without_fallbacks_is_refused(self) -> None:
        data, manifest = self.archive(b'@font-face{src:url(fonts/A.woff2) format("woff2")}')
        with self.assertRaises(VendorError):
            vendor_katex.build_files(manifest, data)

    def test_changed_member_hash_is_refused(self) -> None:
        data, manifest = self.archive(self.CSS)
        manifest["files"]["LICENSE.txt"]["from_sha256"] = "1" * 64
        with self.assertRaises(VendorError):
            vendor_katex.build_files(manifest, data)


class CommittedVendorTests(unittest.TestCase):
    def test_committed_katex_matches_its_manifest(self) -> None:
        manifest = ROOT / "docs" / "javascripts" / "vendor" / "katex" / "vendor.json"
        self.assertEqual(check_vendored_assets.check_directory(manifest), [])

    def test_a_modified_file_is_caught(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vmafx-katex-copy-") as tmp:
            copy = Path(tmp) / "katex"
            shutil.copytree(ROOT / "docs" / "javascripts" / "vendor" / "katex", copy)
            (copy / "katex.min.js").write_bytes(b"// tampered")
            findings = check_vendored_assets.check_directory(copy / "vendor.json")
        self.assertTrue(any("katex.min.js" in line for line in findings))

    def test_manifest_pins_an_exact_version_and_an_integrity(self) -> None:
        manifest = json.loads(
            (ROOT / "docs" / "javascripts" / "vendor" / "katex" / "vendor.json").read_text()
        )
        self.assertRegex(manifest["version"], r"^\d+\.\d+\.\d+$")
        self.assertTrue(manifest["source_integrity"].startswith("sha512-"))
        self.assertIn(manifest["version"], manifest["source"])


if __name__ == "__main__":
    unittest.main()
