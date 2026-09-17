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

import importlib.util
import sys
import unittest
from pathlib import Path

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

    def test_prose_that_names_the_tag_is_not_a_declaration(self) -> None:
        """ "carries an SPDX-License-Identifier: tag" is English, not a licence.

        The file has no licence of its own, so it gains one, and the sentence
        that merely names the tag is left exactly as it was.
        """
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

    def test_fork_authored_code_resolves_to_no_origin(self) -> None:
        prov = REL.load_provenance(ROOT / "dev/relicense_provenance.toml", ROOT.parent)
        self.assertIsNone(prov.port_sources("core/src/mcp/dispatcher.c"))


if __name__ == "__main__":
    unittest.main()
