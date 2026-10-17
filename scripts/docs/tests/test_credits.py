#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixture coverage for the credits generator and its gate (ADR-2485).

Every check of ``scripts/docs/check-credits.py`` has a planted defect here: a
fixture repository that is clean, then one change that the gate must report, then
the entry or edit that clears it. A check that was never seen failing is not a
check. The last test runs the gate on this repository.
"""

from __future__ import annotations

import datetime
import importlib.util
import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.docs import credits_checks as checks  # noqa: E402
from scripts.docs import credits_lib as lib  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

TODAY = datetime.date(2026, 10, 8)
PAGE = """# Credits
<!-- credits:table summary -->
<!-- credits:end -->
<!-- credits:table code -->
<!-- credits:end -->
<!-- credits:table libraries -->
<!-- credits:end -->
<!-- credits:table models -->
<!-- credits:end -->
<!-- credits:table datasets -->
<!-- credits:end -->
<!-- credits:table papers -->
<!-- credits:end -->
<!-- credits:table texts -->
<!-- credits:end -->
<!-- credits:table tools -->
<!-- credits:end -->
<!-- credits:table fonts -->
<!-- credits:end -->
"""
BASE_YAML = """entries:
- id: netflix-vmaf
  name: Netflix VMAF
  url: https://github.com/Netflix/vmaf
  kind: upstream
  relation: vendored
  license: BSD-2-Clause-Patent
  paths: [core/]
"""


def load_script(name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        name.replace("-", "_"), ROOT / "scripts/docs" / name
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


GENERATE = load_script("generate-credits.py")
GATE = load_script("check-credits.py")


class Fixture(unittest.TestCase):
    """A clean disposable repository: a list, a rendered page, one own-licence tree."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vmafx-credits-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        git = shutil.which("git")
        assert git is not None
        run_command(
            (git, "init", "-q", str(self.root)),
            allowed_executables=(git,),
            # Without the hook's GIT_DIR: the fixture, not the caller's repository.
            env={key: value for key, value in os.environ.items() if not key.startswith("GIT_")},
            capture_output=True,
            check=True,
            timeout_seconds=60,
        )
        self.put("core/a.c", "/* Copyright 2016 Netflix, Inc. */\n")
        self.put("LICENSES/EUPL-1.2.txt", "text\n")
        self.put("LICENSES/BSD-2-Clause-Patent.txt", "text\n")
        self.put(
            "REUSE.toml",
            'version = 1\n[[annotations]]\npath = ["**"]\n'
            'SPDX-FileCopyrightText = "2026 Lusoris"\nSPDX-License-Identifier = "EUPL-1.2"\n',
        )
        self.put("docs/credits.yaml", BASE_YAML)
        self.put("docs/credits.md", PAGE)
        self.render()

    def put(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def render(self) -> None:
        self.assertEqual(GENERATE.main(["--write"], root=self.root), 0)

    def add_entry(self, body: str) -> None:
        listing = self.root / "docs/credits.yaml"
        text = listing.read_text(encoding="utf-8")
        head, _, tail = text.partition("\nexceptions:")
        listing.write_text(
            head.rstrip("\n") + "\n" + body + ("\nexceptions:" + tail if tail else "")
        )
        self.render()

    def findings(self) -> dict[str, list[str]]:
        entries = lib.load_entries(self.root / "docs/credits.yaml")
        waivers = lib.load_waivers(self.root / "docs/credits.yaml")
        return checks.run_all(self.root, lib.tracked_files(self.root), entries, waivers, TODAY)

    def only(self, check: str) -> list[str]:
        found = self.findings()
        others = {k: v for k, v in found.items() if k != check and v}
        self.assertEqual(others, {}, "another check fired")
        return found[check]


class CleanFixture(Fixture):
    def test_clean_fixture_has_no_findings(self) -> None:
        self.assertEqual({k: v for k, v in self.findings().items() if v}, {})

    def test_gate_exit_zero_on_the_clean_fixture(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(GATE.main([], root=self.root), 0)
        self.assertIn("OK", out.getvalue())


class PageDrift(Fixture):
    """Check 1: the page equals what the list renders."""

    def test_edited_table_is_drift_and_write_repairs_it(self) -> None:
        page = self.root / "docs/credits.md"
        page.write_text(page.read_text().replace("Netflix VMAF", "Netflix VMAF (edited by hand)"))
        self.assertEqual(len(self.only("page-drift")), 1)
        err, out = io.StringIO(), io.StringIO()
        with redirect_stderr(err), redirect_stdout(out):
            self.assertEqual(GENERATE.main(["--check"], root=self.root), 1)
        self.assertEqual(GENERATE.main(["--write"], root=self.root), 0)
        self.assertEqual(self.findings()["page-drift"], [])

    def test_new_entry_without_rendering_is_drift(self) -> None:
        listing = self.root / "docs/credits.yaml"
        listing.write_text(
            listing.read_text()
            + "- id: cjson\n  name: cJSON\n  url: https://github.com/DaveGamble/cJSON\n"
            "  kind: code\n  relation: vendored\n  license: MIT\n  paths: [core/]\n"
        )
        self.assertEqual(len(self.only("page-drift")), 1)

    def test_missing_page_and_missing_marker_are_findings(self) -> None:
        (self.root / "docs/credits.md").write_text("# Credits\n")
        self.assertTrue(self.findings()["page-drift"])
        (self.root / "docs/credits.md").unlink()
        self.assertIn("missing", self.findings()["page-drift"][0])


class UncreditedPaths(Fixture):
    """Check 2: a vendored or inherited third-party path has no entry."""

    def assert_flagged_then_cleared(self, path: str, entry_body: str) -> None:
        found = self.only("uncredited-path")
        self.assertTrue(any(path in line for line in found), found)
        self.add_entry(entry_body)
        self.assertEqual(self.findings()["uncredited-path"], [])

    def entry(self, ident: str, paths: str, lic: str = "MIT") -> str:
        return (
            f"- id: {ident}\n  name: {ident}\n  url: https://example.org/{ident}\n  kind: code\n"
            f"  relation: vendored\n  license: {lic}\n  paths: [{paths}]\n"
        )

    def test_reuse_annotation_with_a_foreign_licence(self) -> None:
        self.put(
            "REUSE.toml",
            (self.root / "REUSE.toml").read_text()
            + '[[annotations]]\npath = ["lib/x/**"]\nprecedence = "override"\n'
            'SPDX-FileCopyrightText = "2020 Someone"\nSPDX-License-Identifier = "MIT"\n',
        )
        self.put("lib/x/a.c", "int a;\n")
        self.assert_flagged_then_cleared("lib/x/a.c", self.entry("someone-lib", "lib/x/"))

    def test_third_party_directory(self) -> None:
        self.put("pkg/third_party/foo.py", "x = 1\n")
        self.assert_flagged_then_cleared(
            "pkg/third_party/foo.py", self.entry("foo", "pkg/third_party/")
        )

    def test_vendor_and_3rdparty_directories(self) -> None:
        self.put("a/vendor/x.js", "1\n")
        self.put("b/3rdparty/y.c", "1\n")
        found = " ".join(self.only("uncredited-path"))
        self.assertIn("a/vendor/x.js", found)
        self.assertIn("b/3rdparty/y.c", found)

    def test_notice_file(self) -> None:
        self.put("cmd/bpf/object.NOTICE", "generated\n")
        self.assert_flagged_then_cleared("cmd/bpf/object.NOTICE", self.entry("bpf", "cmd/bpf/"))

    def test_font_file(self) -> None:
        self.put("docs/assets/fonts/x/Foo.woff2", "binary\n")
        self.assert_flagged_then_cleared(
            "Foo.woff2", self.entry("foo-font", "docs/assets/fonts/x/", "OFL-1.1")
        )

    def test_foreign_copyright_header(self) -> None:
        self.put("pkg/b.c", "/*\n * Copyright (c) 2011, Tom Distler\n */\n")
        self.assert_flagged_then_cleared("pkg/b.c", self.entry("iqa", "pkg/b.c", "BSD-3-Clause"))

    def test_project_and_netflix_headers_are_not_flagged(self) -> None:
        self.put("core/c.c", "// Copyright 2026 Lusoris\n")
        self.put("core/d.c", "/* Copyright 2016-2023 Netflix, Inc. */\n")
        self.put("core/e.c", "/* the copyright line, as a sentence */\n")
        self.put("docs/note.md", "Copyright (c) 2011 Somebody\n")
        self.assertEqual(self.findings()["uncredited-path"], [])

    def test_markdown_licence_page_is_not_a_notice(self) -> None:
        self.put("docs/adr/by-tag/license.md", "# license\n")
        self.assertEqual(self.findings()["uncredited-path"], [])

    def test_glob_entry_path_covers(self) -> None:
        self.put("pkg/third_party/foo.py", "x = 1\n")
        self.add_entry(self.entry("foo", "pkg/**/foo.py"))
        self.assertEqual(self.findings()["uncredited-path"], [])


class Waivers(Fixture):
    def waiver(self, expires: str, path: str = "scripts/x.sh") -> None:
        listing = self.root / "docs/credits.yaml"
        listing.write_text(
            listing.read_text()
            + f"exceptions:\n- path: {path}\n  rule: uncredited-path\n  reason: a reason\n  expires: '{expires}'\n"
        )
        self.render()

    def test_unexpired_waiver_excuses_one_file(self) -> None:
        self.put("scripts/x.sh", "# Copyright 2026 Somebody Else\n")
        self.assertTrue(self.findings()["uncredited-path"])
        self.waiver("2026-12-31")
        self.assertEqual(self.findings()["uncredited-path"], [])

    def test_expired_waiver_fails(self) -> None:
        self.put("scripts/x.sh", "# Copyright 2026 Somebody Else\n")
        self.waiver("2026-10-07")
        lines = self.findings()["uncredited-path"]
        self.assertTrue(any("expired" in line for line in lines), lines)

    def test_waiver_that_excuses_nothing_fails(self) -> None:
        self.put("scripts/x.sh", "# Copyright 2026 Lusoris\n")
        self.waiver("2026-12-31")
        lines = self.findings()["uncredited-path"]
        self.assertTrue(any("excuses nothing" in line for line in lines), lines)

    def test_malformed_waiver_is_an_error(self) -> None:
        listing = self.root / "docs/credits.yaml"
        listing.write_text(
            listing.read_text()
            + "exceptions:\n- path: a\n  rule: other\n  reason: r\n  expires: x\n"
        )
        with self.assertRaises(lib.CreditsError):
            lib.load_waivers(listing)


class UnusedLicences(Fixture):
    """Check 3: a LICENSES text that nothing uses."""

    def test_unused_text_is_flagged_and_use_clears_it(self) -> None:
        self.put("LICENSES/ISC.txt", "text\n")
        found = self.only("unused-licence")
        self.assertEqual(len(found), 1)
        self.assertIn("ISC", found[0])
        self.add_entry(
            "- id: x86inc\n  name: x86inc\n  url: https://example.org/x\n  kind: code\n"
            "  relation: vendored\n  license: ISC\n  paths: [core/]\n"
        )
        self.assertEqual(self.findings()["unused-licence"], [])

    def test_own_licences_need_no_entry(self) -> None:
        self.assertEqual(self.findings()["unused-licence"], [])

    def test_a_licence_inside_an_expression_counts(self) -> None:
        self.put("LICENSES/Apache-2.0.txt", "text\n")
        self.add_entry(
            "- id: both\n  name: both\n  url: https://example.org/b\n  kind: library\n"
            "  relation: shipped\n  license: MIT AND (Apache-2.0 OR BSD-3-Clause)\n  paths: [core/]\n"
        )
        self.assertEqual(self.findings()["unused-licence"], [])


class Adaptations(Fixture):
    """Check 4: a skill that says where it came from has an entry for that upstream."""

    SKILL = (
        '---\nname: caveman\nmetadata:\n  derived_from: "https://github.com/JuliusBrussee/caveman (MIT)"\n---\n'
        "Adapted from [Caveman](https://github.com/JuliusBrussee/caveman) by Julius Brussee.\n"
    )

    def caveman(self, url: str) -> str:
        return (
            f"- id: caveman\n  name: Caveman\n  url: {url}\n  kind: text\n  relation: adapted\n"
            "  license: MIT\n  paths: []\n"
        )

    def test_derived_skill_without_an_entry_is_flagged(self) -> None:
        self.put(".agents/skills/caveman/SKILL.md", self.SKILL)
        found = self.only("uncredited-adaptation")
        self.assertEqual(len(found), 2)
        self.assertIn("JuliusBrussee/caveman", found[0])

    def test_entry_clears_it_whatever_the_spelling_of_the_url(self) -> None:
        self.put(".agents/skills/caveman/SKILL.md", self.SKILL)
        for url in (
            "https://github.com/JuliusBrussee/caveman",
            "https://github.com/juliusbrussee/caveman.git",
            "https://www.github.com/JuliusBrussee/caveman/",
        ):
            with self.subTest(url=url):
                listing = self.root / "docs/credits.yaml"
                listing.write_text(BASE_YAML + self.caveman(url))
                self.render()
                self.assertEqual(self.findings()["uncredited-adaptation"], [])

    def test_other_upstream_is_still_flagged(self) -> None:
        self.put(".claude/skills/x/SKILL.md", self.SKILL.replace("caveman", "other"))
        self.add_entry(self.caveman("https://github.com/JuliusBrussee/caveman"))
        self.assertEqual(len(self.findings()["uncredited-adaptation"]), 2)

    def test_files_without_a_declared_source_are_ignored(self) -> None:
        self.put(".agents/skills/own/SKILL.md", "---\nname: own\n---\nText.\n")
        self.assertEqual(self.findings()["uncredited-adaptation"], [])


class MissingPaths(Fixture):
    def test_entry_path_not_in_the_checkout(self) -> None:
        self.add_entry(
            "- id: gone\n  name: gone\n  url: https://example.org/g\n  kind: code\n  relation: vendored\n"
            "  license: MIT\n  paths: [core/a.c, vendor-moved/]\n  evidence: [docs/none.md]\n"
        )
        found = self.only("missing-path")
        self.assertEqual(len(found), 2)
        self.assertTrue(any("vendor-moved/" in line for line in found))


class Validation(Fixture):
    def load(self, body: str) -> None:
        (self.root / "docs/credits.yaml").write_text("entries:\n" + body)
        lib.load_entries(self.root / "docs/credits.yaml")

    ENTRY = "- id: a\n  name: A\n  url: https://x.org\n  kind: code\n  relation: vendored\n  license: MIT\n"

    def test_valid_entry_loads(self) -> None:
        self.load(self.ENTRY)

    def test_each_bad_field_is_refused(self) -> None:
        for old, new in (
            ("kind: code", "kind: thing"),
            ("relation: vendored", "relation: copied"),
            ("url: https://x.org", "url: http://x.org"),
            ("license: MIT", "license: GPL-3.0"),
            ("license: MIT", "license: LGPL-2.1+"),
            ("license: MIT", "license: ''"),
            ("id: a", "id: Not_Kebab"),
        ):
            with self.subTest(change=new), self.assertRaises(lib.CreditsError):
                self.load(self.ENTRY.replace(old, new))

    def test_missing_field_unknown_key_and_duplicate_id_are_refused(self) -> None:
        with self.assertRaises(lib.CreditsError):
            self.load(self.ENTRY.replace("  name: A\n", ""))
        with self.assertRaises(lib.CreditsError):
            self.load(self.ENTRY + "  colour: red\n")
        with self.assertRaises(lib.CreditsError):
            self.load(self.ENTRY + self.ENTRY)

    def test_licence_words_and_licenceref_are_accepted(self) -> None:
        for lic in (
            "proprietary",
            "none",
            "unknown",
            "LicenseRef-LIVE-BRISQUE",
            "GPL-3.0-or-later",
            "A AND (B OR C WITH D)",
        ):
            with self.subTest(lic=lic):
                self.load(self.ENTRY.replace("license: MIT", f"license: {lic}"))

    def test_gate_exits_two_on_a_broken_list(self) -> None:
        (self.root / "docs/credits.yaml").write_text("entries: 3\n")
        err = io.StringIO()
        with redirect_stderr(err):
            self.assertEqual(GATE.main([], root=self.root), 2)


class Helpers(unittest.TestCase):
    def test_path_matches(self) -> None:
        self.assertTrue(lib.path_matches("core/", "core/a/b.c"))
        self.assertTrue(lib.path_matches("core", "core/a/b.c"))
        self.assertTrue(lib.path_matches("a/b.c", "a/b.c"))
        self.assertFalse(lib.path_matches("core", "corex/a.c"))
        self.assertTrue(lib.path_matches("model/tiny/fast.*", "model/tiny/fast.onnx"))
        self.assertTrue(lib.path_matches("a/**/z.c", "a/b/c/z.c"))
        self.assertFalse(lib.path_matches("a/*.c", "a/b/z.c"))

    def test_license_tokens_and_urls(self) -> None:
        self.assertEqual(lib.license_tokens("MIT AND (A OR B WITH C)"), ["MIT", "A", "B", "C"])
        self.assertEqual(checks.normalise_url("HTTPS://www.GitHub.com/O/R.git/"), "github.com/o/r")
        self.assertEqual(
            checks.normalise_url("https://github.com/o/r/graphs/contributors"), "github.com/o/r"
        )

    def test_counts_and_unverified_summary(self) -> None:
        e = lib.Entry("a", "A", "https://x", "code", "vendored", "unknown")
        text = lib.blocks([e])["summary"]
        self.assertIn("1 entries", text)
        self.assertIn("could not verify", text)


class ThisRepository(unittest.TestCase):
    def test_the_gate_passes_on_this_repository(self) -> None:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = GATE.main([], root=ROOT)
        self.assertEqual(code, 0, out.getvalue() + err.getvalue())
        with redirect_stdout(out):
            self.assertEqual(GENERATE.main(["--check"], root=ROOT), 0)


if __name__ == "__main__":
    unittest.main()
