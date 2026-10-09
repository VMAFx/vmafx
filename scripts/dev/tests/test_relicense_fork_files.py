#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Controls for the header rewriting in scripts/dev/relicense_fork_files.py (ADR-1250).

The classification these tests do not cover is checked by the tool itself:
`--check` recomputes every verdict and fails when the tree disagrees. What is
pinned here is the part that edits files, where a mistake is silent: a grant left
behind contradicting the tag it sits under, a tag the rewrite missed inside a
generator string, a header inserted twice into a file that already had a
copyright line, or a rewrite that is not idempotent and so churns the tree on
every run.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "relicense_fork_files", ROOT / "dev/relicense_fork_files.py"
)
assert SPEC is not None and SPEC.loader is not None
REL = importlib.util.module_from_spec(SPEC)
# Registered before execution: dataclasses resolves field types through
# sys.modules, and a module that is not there yet raises during class creation.
sys.modules[SPEC.name] = REL
SPEC.loader.exec_module(REL)

TAG = REL.TAG
YEAR = "2026"


def rewrite(text: str, path: str = "x.c") -> str:
    return REL.rewrite(text, path, lambda: YEAR)


class TagRewriting(unittest.TestCase):
    def test_tag_is_retargeted(self) -> None:
        out = rewrite(f"/*\n * Copyright 2026 Lusoris\n * {TAG} BSD-2-Clause-Patent\n */\n")
        self.assertIn(f"{TAG} EUPL-1.2", out)
        self.assertNotIn("BSD-2-Clause-Patent", out)

    def test_dual_licence_loses_the_permissive_alternative(self) -> None:
        out = rewrite(
            f"// Copyright 2026 Lusoris\n// {TAG} BSD-3-Clause-Plus-Patent OR MIT\n", "x.go"
        )
        self.assertIn(f"{TAG} EUPL-1.2", out)
        self.assertNotIn("MIT", out)

    def test_a_generator_string_is_retargeted_too(self) -> None:
        """What a generator emits is fork-authored, so its tag moves with it."""
        src = (
            f"// Copyright 2026 Lusoris\n// {TAG} BSD-3-Clause-Plus-Patent\n"
            f'fn main() {{ print("{TAG} BSD-3-Clause-Plus-Patent\\n"); }}\n'
        )
        self.assertEqual(rewrite(src, "build.rs").count(f"{TAG} EUPL-1.2"), 2)

    # REUSE-IgnoreStart
    def test_prose_that_names_the_tag_is_not_a_declaration(self) -> None:
        """ "carries an SPDX-License-Identifier: tag" is English, not a licence.

        The file has no licence of its own, so it gains one, and the sentence
        that merely names the tag is left exactly as it was.
        """
        # REUSE-IgnoreEnd
        src = f"/*\n * Copyright 2026 Lusoris\n * Each file carries an {TAG} tag.\n */\n"
        out = rewrite(src)
        self.assertIn(f"Each file carries an {TAG} tag.", out)
        self.assertIn(f" * {TAG} EUPL-1.2\n", out)


class GrantRemoval(unittest.TestCase):
    LONG_GRANT = (
        "/**\n"
        " *\n"
        " *  Copyright 2026 Lusoris\n"
        " *\n"
        ' *     Licensed under the BSD+Patent License (the "License");\n'
        " *     you may not use this file except in compliance with the License.\n"
        " *     You may obtain a copy of the License at\n"
        " *\n"
        " *         https://opensource.org/licenses/BSDplusPatent\n"
        " *\n"
        " *     Unless required by applicable law or agreed to in writing, software\n"
        ' *     distributed under the License is distributed on an "AS IS" BASIS,\n'
        " *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n"
        " *     See the License for the specific language governing permissions and\n"
        " *     limitations under the License.\n"
        " *\n"
        " */\n"
    )

    def test_prose_only_grant_becomes_the_tag(self) -> None:
        out = rewrite(self.LONG_GRANT)
        self.assertIn(f"{TAG} EUPL-1.2", out)
        self.assertNotIn("BSD+Patent", out)
        self.assertNotIn("limitations under the License", out)

    def test_grant_below_a_tag_is_removed_not_left_contradicting_it(self) -> None:
        """The defect this rewrite exists to avoid: two licences in one header."""
        src = (
            "/**\n"
            " *  Copyright 2026 Lusoris\n"
            f" *  {TAG} BSD-2-Clause-Patent\n"
            " *\n"
            ' *  Licensed under the BSD+Patent License (the "License");\n'
            " *  you may not use this file except in compliance with the License.\n"
            " *  You may obtain a copy of the License at\n"
            " *\n"
            " *      https://opensource.org/licenses/BSDplusPatent\n"
            " */\n"
        )
        out = rewrite(src)
        self.assertIn(f"{TAG} EUPL-1.2", out)
        self.assertNotIn("BSD+Patent", out)
        self.assertEqual(out.count(TAG), 1)

    def test_go_style_grant_becomes_the_tag(self) -> None:
        src = (
            "// Copyright 2026 Lusoris. All rights reserved.\n"
            "// Use of this source code is governed by the BSD-3-Clause-Plus-Patent\n"
            "// license that can be found in the LICENSE file.\n"
            "\npackage main\n"
        )
        out = rewrite(src, "main.go")
        self.assertIn(f"// {TAG} EUPL-1.2", out)
        self.assertNotIn("governed by", out)
        self.assertIn("package main", out)

    def test_a_grant_truncated_to_its_first_sentence_is_still_a_grant(self) -> None:
        src = (
            "/**\n *\n *  Copyright 2026 Lusoris\n *\n"
            ' *     Licensed under the BSD+Patent License (the "License");\n *\n */\n'
        )
        out = rewrite(src)
        self.assertIn(f"{TAG} EUPL-1.2", out)
        self.assertNotIn("BSD+Patent", out)


class GrantOutsideTheHeader(unittest.TestCase):
    """A grant below the file's own header is content (T-RELICENSE-CHECK-PENDING-2026-10-02)."""

    OWN_HEADER = "#!/usr/bin/env bash\n# Copyright 2026 Lusoris\n# " + TAG + " EUPL-1.2\n"
    # A script that writes another file's header carries that header as text.
    TEMPLATE = 'prefix = """' + GrantRemoval.LONG_GRANT + '"""\n'

    def test_a_header_template_further_down_is_left_alone(self) -> None:
        body = "".join(f"step_{i}() {{ :; }}\n" for i in range(REL.HEADER_SCAN))
        text = self.OWN_HEADER + body + self.TEMPLATE
        self.assertEqual(rewrite(text, "sync.sh"), text)

    def test_a_grant_in_the_header_is_still_rewritten(self) -> None:
        out = rewrite(GrantRemoval.LONG_GRANT + "int x;\n")
        self.assertNotIn("BSD+Patent", out)
        self.assertIn(f"{TAG} EUPL-1.2", out)

    def test_the_boundary_is_the_header_scan(self) -> None:
        lines = ["x\n"] * REL.HEADER_SCAN + [
            'Licensed under the BSD+Patent License (the "License");\n'
        ]
        self.assertEqual(REL.header_prose_blocks(lines), [])
        self.assertEqual(len(REL.prose_blocks(lines)), 1)
        lines = ["x\n"] * (REL.HEADER_SCAN - 1) + lines[-1:]
        self.assertEqual(len(REL.header_prose_blocks(lines)), 1)

    def test_the_shipped_sync_script_is_not_rewritten(self) -> None:
        """The Pelorus sync script writes the fixture's header below its own.

        Until ADR-2817 that template carried Pelorus's BSD+Patent grant, which
        test_a_header_template_further_down_is_left_alone now pins with a
        constructed file; since Pelorus v0.3.0 it carries the EUPL-1.2 tag.
        Either way the rewrite leaves it alone.
        """
        path = ROOT / "sync-pelorus-interop.sh"
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines()
        templates = [i for i, line in enumerate(lines) if "Copyright 2026 Lusoris" in line]
        self.assertTrue(
            [i for i in templates if i >= REL.HEADER_SCAN],
            "the mirrored header template is gone",
        )
        self.assertEqual(rewrite(text, "scripts/sync-pelorus-interop.sh"), text)


class Candidates(unittest.TestCase):
    def test_data_and_managed_files_are_not_candidates(self) -> None:
        for path in (
            "scripts/ci/exact_twins.d/adm.hip",
            "scripts/ci/exact_twins.d/float_ssim.sycl",
            "tools/figures/mkdocs_hook.py",
            "tools/figures/dist/player.js",
            ".config/agent/hooks/block_evasion.py",
            ".codex/agents/c-reviewer.toml",
        ):
            with self.subTest(path=path):
                self.assertFalse(REL.is_candidate_path(path))

    def test_their_neighbours_still_are(self) -> None:
        for path in (
            "scripts/ci/cross_backend_calibration.py",
            "core/src/feature/hip/integer_adm_hip.c",
            ".config/agent/hooks/other_hook.py",
            "tools/vmaf-tune/src/vmaftune/cli.py",
            ".codex/config.toml",
            "core/src/feature/rust/tad/Cargo.toml",
        ):
            with self.subTest(path=path):
                self.assertTrue(REL.is_candidate_path(path))

    def test_a_toml_file_takes_a_hash_comment_header(self) -> None:
        """ADR-1699: manifests and tool configuration are classified, not skipped."""
        self.assertEqual(REL.comment_style("core/src/feature/rust/tad/Cargo.toml"), "hash")
        out = rewrite('[package]\nname = "x"\n', "Cargo.toml")
        self.assertEqual(
            out, f'# Copyright {YEAR} Lusoris\n# {TAG} EUPL-1.2\n\n[package]\nname = "x"\n'
        )

    def test_a_package_manifest_name_carries_no_provenance(self) -> None:
        """Upstream's python/pyproject.toml does not veto a fork package's manifest."""
        up = REL.Upstream(
            frozenset({"python/pyproject.toml"}), frozenset({"pyproject.toml"}), frozenset()
        )
        prov = REL.load_provenance(REL.PROVENANCE, ROOT.parent)
        text = '[project]\nname = "fork-tool"\n'
        self.assertIsNone(REL.static_verdict("tools/fork-tool/pyproject.toml", text, up, prov))
        self.assertEqual(
            REL.static_verdict("python/pyproject.toml", text, up, prov), "upstream-path"
        )


class HeaderInsertion(unittest.TestCase):
    def test_tag_joins_an_existing_copyright_line(self) -> None:
        """A file with a notice but no tag gains a line, not a second header."""
        out = rewrite("/**\n *  Copyright 2026 Lusoris\n */\n\n#define X 1\n")
        self.assertEqual(out.count("Copyright"), 1)
        self.assertIn(f" *  {TAG} EUPL-1.2\n", out)

    def test_untagged_shell_script_keeps_its_shebang_first(self) -> None:
        out = rewrite("#!/usr/bin/env bash\nset -e\n", "run.sh")
        self.assertTrue(out.startswith("#!/usr/bin/env bash\n"))
        self.assertIn(f"# {TAG} EUPL-1.2", out)
        self.assertIn(f"# Copyright {YEAR} Lusoris", out)

    def test_python_encoding_cookie_stays_in_the_first_two_lines(self) -> None:
        out = rewrite("#!/usr/bin/env python3\n# -*- coding: utf-8 -*-\nx = 1\n", "a.py")
        self.assertEqual(out.splitlines()[1], "# -*- coding: utf-8 -*-")

    def test_empty_file_gains_no_trailing_blank(self) -> None:
        self.assertEqual(
            rewrite("", "__init__.py"), f"# Copyright {YEAR} Lusoris\n# {TAG} EUPL-1.2\n"
        )

    def test_python_license_attribute_follows_the_tag(self) -> None:
        out = rewrite(
            f'# Copyright 2026 Lusoris\n# {TAG} BSD-3-Clause\n__license__ = "BSD+Patent"\n', "a.py"
        )
        self.assertIn('__license__ = "EUPL-1.2"', out)


class Attribution(unittest.TestCase):
    NETFLIX = REL.Source(("Copyright 2016-2026 Netflix, Inc.",), ("BSD-2-Clause-Patent",))
    IQA = REL.Source(("Copyright (c) 2011, Tom Distler (http://tdistler.com)",), ("BSD-3-Clause",))

    def test_missing_notice_is_restored_and_the_licence_joins_the_tag(self) -> None:
        src = f"/**\n *  Copyright 2026 Lusoris\n *  {TAG} BSD-2-Clause-Patent\n */\n"
        out = REL.attribute(src, [self.IQA])
        self.assertIn("Tom Distler", out)
        self.assertIn(f"{TAG} BSD-2-Clause-Patent AND BSD-3-Clause", out)

    def test_a_credited_holder_is_not_credited_twice_with_different_years(self) -> None:
        src = (
            "/**\n *  Copyright 2016-2023 Netflix, Inc.\n *  Copyright 2026 Lusoris\n"
            f" *  {TAG} BSD-2-Clause-Patent\n */\n"
        )
        self.assertEqual(REL.attribute(src, [self.NETFLIX]).count("Netflix"), 1)

    def test_invalid_identifier_is_repaired_on_a_file_that_stays(self) -> None:
        src = f"/**\n *  Copyright 2026 Lusoris\n *  {TAG} BSD-3-Clause-Plus-Patent\n */\n"
        self.assertIn(f"{TAG} BSD-2-Clause-Patent", REL.repair(src))

    def test_attribution_is_idempotent(self) -> None:
        src = f"/**\n *  Copyright 2026 Lusoris\n *  {TAG} BSD-2-Clause-Patent\n */\n"
        once = REL.attribute(src, [self.NETFLIX, self.IQA])
        self.assertEqual(REL.attribute(once, [self.NETFLIX, self.IQA]), once)


class Idempotence(unittest.TestCase):
    def test_rewriting_twice_changes_nothing_further(self) -> None:
        for name, src in (
            ("x.c", GrantRemoval.LONG_GRANT),
            (
                "main.go",
                "// Copyright 2026 Lusoris\n// Use of this source code is governed by the X\n// license that can be found in the LICENSE file.\n\npackage main\n",
            ),
            ("run.sh", "#!/usr/bin/env bash\nset -e\n"),
            ("a.py", "#!/usr/bin/env python3\n'''doc'''\n"),
        ):
            with self.subTest(name=name):
                once = rewrite(src, name)
                self.assertEqual(rewrite(once, name), once)


class ProvenanceData(unittest.TestCase):
    def test_the_shipped_provenance_file_loads_and_resolves(self) -> None:
        prov = REL.load_provenance(ROOT / "dev/relicense_provenance.toml", ROOT.parent)
        self.assertTrue(prov.families, "families are what cover the SIMD and GPU kernels")
        self.assertTrue(prov.ports)
        self.assertTrue(prov.not_ports)
        for source in prov.sources.values():
            self.assertTrue(source.notices, "an origin with no notice credits nobody")
            self.assertTrue(source.licences)

    def test_a_kernel_resolves_to_the_code_it_implements(self) -> None:
        prov = REL.load_provenance(ROOT / "dev/relicense_provenance.toml", ROOT.parent)
        cases = {
            "core/src/feature/cuda/integer_vif/filter1d.cu": "netflix-integer-vif",
            "core/src/feature/hip/ssimulacra2/ssimulacra2_blur.hip": "libjxl",
            "core/src/feature/x86/psnr_hvs_avx2.c": "xiph",
            "core/src/feature/arm64/convolve_neon.c": "iqa",
        }
        for path, origin in cases.items():
            with self.subTest(path=path):
                self.assertIn(origin, prov.port_sources(path) or ())

    def test_a_helper_header_resolves_to_exactly_the_code_it_holds(self) -> None:
        # A family is the default for a kernel; these headers hold one named part
        # of the reference and must not be credited with the rest of the family.
        prov = REL.load_provenance(ROOT / "dev/relicense_provenance.toml", ROOT.parent)
        cases = {
            "core/src/feature/hip/float_ssim/ssim_decimate.h": ["netflix-float-ssim", "iqa"],
            "core/src/feature/metal/float_ms_ssim_option_semantics.h": ["netflix-ms-ssim"],
            "core/src/feature/sycl/sycl_integer_ssim_math.h": ["xiph-integer-ssim"],
            "core/src/feature/sycl/sycl_ssim_terms.h": ["netflix-float-ssim", "iqa"],
            "core/src/feature/sycl/sycl_ssimulacra2_math.h": ["libjxl"],
        }
        for path, origins in cases.items():
            with self.subTest(path=path):
                self.assertEqual(list(prov.port_sources(path) or ()), origins)

    def test_a_header_that_only_configures_a_shared_header_is_not_a_port(self) -> None:
        # An argument block, or macros and an include: the reference's code and
        # its notices are in the shared header, not in these files, although
        # each sits in a kernel directory whose family names an origin.
        prov = REL.load_provenance(ROOT / "dev/relicense_provenance.toml", ROOT.parent)
        for path in (
            "core/src/feature/cuda/speed/speed_cuda_params.h",
            "core/src/feature/hip/float_adm/float_adm_hip_math.h",
            "core/src/feature/hip/integer_ciede/ciede_hip_math.h",
            "core/src/feature/sycl/sycl_ciede_math.h",
        ):
            with self.subTest(path=path):
                self.assertIn(path, prov.not_ports)
                self.assertIsNone(prov.port_sources(path))

    def test_fork_authored_code_resolves_to_no_origin(self) -> None:
        prov = REL.load_provenance(ROOT / "dev/relicense_provenance.toml", ROOT.parent)
        self.assertIsNone(prov.port_sources("core/src/mcp/dispatcher.c"))

    def test_vendored_pelorus_files_are_mirrors(self) -> None:
        prov = REL.load_provenance(ROOT / "dev/relicense_provenance.toml", ROOT.parent)
        for path in (
            "core/include/libvmaf/pelorus/interop.h",
            "core/src/interop/pelorus_interop.c",
            "core/test/test_pelorus_interop.c",
        ):
            with self.subTest(path=path):
                self.assertEqual(prov.mirror_of(path), "pelorus")
        # The fork's own consumer of the ABI is not a mirror.
        self.assertIsNone(prov.mirror_of("core/src/feature/perceptual_weight.c"))

    def test_a_mirror_is_vetoed_before_the_port_check(self) -> None:
        prov = REL.load_provenance(ROOT / "dev/relicense_provenance.toml", ROOT.parent)
        up = REL.Upstream(frozenset(), frozenset(), frozenset())
        verdict = REL.static_verdict("core/src/interop/pelorus_interop.c", "int x;\n", up, prov)
        self.assertEqual(verdict, "vendored-mirror")


class DerivationDetector(unittest.TestCase):
    UP = REL.Upstream(
        frozenset({"libvmaf/test/test.h", "libvmaf/src/feature/integer_adm.c"}),
        frozenset({"test.h", "integer_adm.c"}),
        frozenset({"integer_adm.c"}),
    )

    def test_a_port_statement_is_found(self) -> None:
        text = "/* Scalar oracle ported from integer_adm.c in upstream. */\nint x;\n"
        self.assertTrue(REL.derivation_statements(text, self.UP))

    def test_a_nolint_justification_is_not_a_port_statement(self) -> None:
        # The ADR-1138 band says the file "mirrors" the C spelling it exercises;
        # next to a header reference that read as a derivation claim.
        text = (
            "/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The\n"
            " * required Windows build compiles this TU with cl.exe, and this file mirrors\n"
            " * the C spelling of the surface it exercises. ADR-1138. */\n"
            '#include "test.h"\n'
            "/* Checks integer_adm.c's scale bands. */\n"
            "int x;\n"
        )
        self.assertEqual(REL.derivation_statements(text, self.UP), [])

    def test_a_line_comment_suppression_is_dropped_too(self) -> None:
        text = "x = 0; // NOLINT(bugprone-foo): mirrors integer_adm.c exactly\nint y;\n"
        self.assertEqual(REL.derivation_statements(text, self.UP), [])


class FullHistory(unittest.TestCase):
    """The author veto reads `git log --follow`; a shallow clone has no log."""

    def test_a_complete_checkout_passes(self) -> None:
        with mock.patch.object(REL, "git", return_value="false\n") as git:
            REL.require_full_history(Path())
        git.assert_called_once_with(Path(), "rev-parse", "--is-shallow-repository")

    def test_a_shallow_checkout_is_refused(self) -> None:
        stderr = io.StringIO()
        with (
            mock.patch.object(REL, "git", return_value="true\n"),
            contextlib.redirect_stderr(stderr),
        ):
            with self.assertRaises(SystemExit) as stop:
                REL.require_full_history(Path())
        self.assertEqual(stop.exception.code, 2)
        self.assertIn("shallow", stderr.getvalue())

    def test_an_unreadable_answer_is_refused(self) -> None:
        # Fail closed: anything but git's literal "false" is not a full history.
        with (
            mock.patch.object(REL, "git", return_value=""),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            with self.assertRaises(SystemExit):
                REL.require_full_history(Path())


class AiToolCopyright(unittest.TestCase):
    """ADR-0861: copyright lines name the project holder only."""

    def test_owner_only_notice_passes(self) -> None:
        """Positive: a file with only Copyright 2026 Lusoris passes."""
        src = f"# Copyright 2026 Lusoris\n# {TAG} EUPL-1.2\n"
        self.assertEqual(REL.check_ai_copyright(src), [])

    def test_planted_ai_copyright_fails(self) -> None:
        """Negative: a planted # Copyright 2026 Claude (Anthropic) line fails."""
        src = (
            "#!/usr/bin/env bash\n"
            "# Copyright 2026 Lusoris\n"
            "# Copyright 2026 Claude (Anthropic)\n"
            f"# {TAG} BSD-2-Clause-Patent\n"
        )
        violations = REL.check_ai_copyright(src, "scripts/test.sh")
        self.assertTrue(violations)
        self.assertIn("scripts/test.sh:3", violations[0])
        self.assertIn("ADR-0861", violations[0])
        self.assertIn("Claude", violations[0])

    def test_dual_owner_and_ai_copyright_fails(self) -> None:
        """Negative: // Copyright 2026 Lusoris and Claude (Anthropic) fails."""
        src = "// Copyright 2026 Lusoris and Claude (Anthropic)\n" f"// {TAG} BSD-2-Clause-Patent\n"
        violations = REL.check_ai_copyright(src, "test.go")
        self.assertTrue(violations)
        self.assertIn("test.go:1", violations[0])
        self.assertIn("ADR-0861", violations[0])
        self.assertIn("Claude", violations[0])

    def test_tool_mention_outside_copyright_line_passes(self) -> None:
        """Boundary: the word Claude outside a copyright line does not fail."""
        src = (
            "#!/usr/bin/env bash\n"
            "# Tested with Claude for regressions.\n"
            "# Copyright 2026 Lusoris\n"
            f"# {TAG} EUPL-1.2\n"
        )
        self.assertEqual(REL.check_ai_copyright(src), [])


if __name__ == "__main__":
    unittest.main()
