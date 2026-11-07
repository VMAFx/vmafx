# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary tests for check-docs-fragment-imports.py (issue 3244)."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "scripts" / "ci" / "check-docs-fragment-imports.py"

spec = importlib.util.spec_from_file_location("check_docs_fragment_imports", CHECKER)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

# A recipe line: the Makefile's real recipe lines are tab-indented.
CHECKER_LINE = "\t@python3 scripts/ci/check-docs-fragment-imports.py"
DOCS_CHECK_IN = (
    "# The third-party imports of `make docs-fragments-check` in the Docs job\n"
    "# of lint-and-format.yml.\n"
    "jsonschema==4.26.0\n"
    "pyyaml==6.0.3\n"
)


def plant(root: Path, files: dict[str, str]) -> Path:
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return root


def makefile(recipes: str) -> str:
    """A Makefile whose docs-fragments-check target runs `recipes` (tab lines)."""
    return (
        "# Copyright 2026 Lusoris\n"
        "SHELL := /bin/bash\n\n"
        "docs-fragments-check:\n"
        f"{recipes}\n"
        "docs-fragments-write:\n"
        "\t@echo next target\n"
    )


class CheckDocsFragmentImportsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def findings(self, files: dict[str, str]) -> list[str]:
        plant(self.root, files)
        return module.findings(self.root)

    def test_the_real_repository_is_clean(self) -> None:
        """The gate is wired and the tree's docs-fragment imports are declared."""
        self.assertEqual(module.findings(ROOT), [])

    def test_a_new_third_party_import_fails_until_declared(self) -> None:
        """Planted defect: a recipe script imports `requests`, absent from the lock input."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE + "\n\t@python3 scripts/ci/check-hardware-reports.py"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/ci/check-hardware-reports.py": ("import json\nimport requests\n"),
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("scripts/ci/check-hardware-reports.py imports 'requests'", problems[0])
        self.assertIn(module.LOCK_INPUT, problems[0])

    def test_an_importlib_literal_is_caught(self) -> None:
        """check-hardware-reports.py imports jsonschema through
        importlib.import_module; a literal there must count as an import."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE + "\n\t@python3 scripts/ci/check-hardware-reports.py"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/ci/check-hardware-reports.py": (
                    "import importlib\n" "jsonschema = importlib.import_module('jsonschema')\n"
                ),
                "requirements/locks/docs-check.in": "pyyaml==6.0.3\n",
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("imports 'jsonschema'", problems[0])

    def test_yaml_through_the_import_closure_is_recognised(self) -> None:
        """generate-credits.py imports yaml only through scripts.docs.credits_lib;
        the closure walk must reach it, and the pyyaml -> yaml mapping must hold."""
        recipes = CHECKER_LINE + "\n\t@python3 scripts/docs/generate-credits.py"
        base = {
            "Makefile": makefile(recipes),
            "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
            "scripts/docs/generate-credits.py": (
                "import sys\n"
                "from pathlib import Path\n"
                "sys.path.insert(0, str(Path(__file__).resolve().parents[2]))\n"
                "from scripts.docs.credits_lib import build\n"
            ),
            "scripts/docs/credits_lib.py": "import yaml\n\ndef build():\n    return yaml\n",
            "requirements/locks/docs-check.in": DOCS_CHECK_IN,
        }
        self.assertEqual(self.findings(dict(base)), [])
        undeclared = dict(base)
        undeclared["requirements/locks/docs-check.in"] = "jsonschema==4.26.0\n"
        problems = self.findings(undeclared)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("imports 'yaml'", problems[0])

    def test_a_bash_helper_python_call_is_scanned(self) -> None:
        """The bash helpers of the recipe run python3 scripts of their own."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE + "\n\t@bash scripts/docs/concat-helper.sh --lint"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/concat-helper.sh": (
                    "#!/usr/bin/env bash\n" 'python3 "$SCRIPT_DIR/fragment-order.py" "$@"\n'
                ),
                "scripts/docs/fragment-order.py": "import flask\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("scripts/docs/fragment-order.py imports 'flask'", problems[0])

    def test_an_unwired_gate_is_itself_a_finding(self) -> None:
        """A recipe that drops the check line must fail, not pass silently."""
        problems = self.findings(
            {
                "Makefile": makefile("\t@python3 scripts/docs/check-adr-index.py"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/check-adr-index.py": "import re\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("unwired", problems[0])

    def test_the_documented_optional_import_stays_out_of_the_lock(self) -> None:
        """vl-convert-python is a graceful-fallback renderer, deliberately absent
        from docs-check.in; the gate must not demand it."""
        self.assertEqual(
            self.findings(
                {
                    "Makefile": makefile(
                        CHECKER_LINE + "\n\t@python3 scripts/docs/generate-charts.py"
                    ),
                    "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                    "scripts/docs/generate-charts.py": (
                        "import importlib\n"
                        "def load_renderer():\n"
                        "    return importlib.import_module('vl_convert')\n"
                    ),
                    "requirements/locks/docs-check.in": DOCS_CHECK_IN,
                }
            ),
            [],
        )

    def test_a_recipe_script_that_does_not_exist_is_a_finding(self) -> None:
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@python3 scripts/docs/renamed-away.py"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("renamed-away.py", problems[0])
        self.assertIn("does not exist", problems[0])

    def test_stdlib_and_relative_imports_stay_clean(self) -> None:
        recipes = CHECKER_LINE + "\n\t@python3 scripts/docs/check-adr-index.py"
        self.assertEqual(
            self.findings(
                {
                    "Makefile": makefile(recipes),
                    "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                    "scripts/docs/check-adr-index.py": (
                        "from __future__ import annotations\n"
                        "import re\n"
                        "from . import fragment_order\n"
                    ),
                    "scripts/docs/fragment_order.py": "import tomllib\n",
                    "requirements/locks/docs-check.in": DOCS_CHECK_IN,
                }
            ),
            [],
        )

    def test_missing_inputs_exit_2(self) -> None:
        """Without a Makefile or the lock input the gate cannot run."""
        with tempfile.TemporaryDirectory() as name:
            plant(Path(name), {"requirements/locks/docs-check.in": DOCS_CHECK_IN})
            self.assertEqual(module.main(["--root", name]), 2)
        with tempfile.TemporaryDirectory() as name:
            plant(Path(name), {"Makefile": makefile(CHECKER_LINE)})
            self.assertEqual(module.main(["--root", name]), 2)

    # --- the five bypass shapes from the review ---

    def test_a_from_package_import_reaches_the_submodule(self) -> None:
        """`from pkg import mod` must scan pkg/mod.py, not stop at the package."""
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@python3 scripts/docs/user.py"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/pkg/__init__.py": "",
                "scripts/docs/pkg/mod.py": "import flask\n",
                "scripts/docs/user.py": "from pkg import mod\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("imports 'flask'", problems[0])

    def test_a_from_package_import_of_an_init_name_stays_clean(self) -> None:
        """`from pkg import reexport` with no pkg/reexport.py file resolves
        inside the package; it must not become a third-party finding."""
        self.assertEqual(
            self.findings(
                {
                    "Makefile": makefile(CHECKER_LINE + "\n\t@python3 scripts/docs/user.py"),
                    "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                    "scripts/docs/pkg/__init__.py": "reexport = 1\n",
                    "scripts/docs/user.py": "from pkg import reexport\n",
                    "requirements/locks/docs-check.in": DOCS_CHECK_IN,
                }
            ),
            [],
        )

    def test_a_truncated_closure_walk_is_a_finding(self) -> None:
        """A closure cut short by the file bound must fail loudly, not pass
        a partial result as clean."""
        old = module.MAX_FILES
        module.MAX_FILES = 4
        self.addCleanup(setattr, module, "MAX_FILES", old)
        chain = {
            "Makefile": makefile(CHECKER_LINE + "\n\t@python3 scripts/docs/chain_5.py"),
            "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
            "requirements/locks/docs-check.in": DOCS_CHECK_IN,
        }
        for i in range(6):
            chain[f"scripts/docs/chain_{i}.py"] = (
                "import flask\n" if i == 0 else f"from scripts.docs.chain_{i - 1} import x\n"
            )
        problems = self.findings(chain)
        self.assertTrue(any("stopped" in p for p in problems), problems)

    def test_an_unparsable_script_is_a_finding(self) -> None:
        """A script that cannot be parsed must fail the gate, not drop its
        imports silently."""
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@python3 scripts/docs/broken.py"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/broken.py": "def f(:\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("unreadable or unparsable", problems[0])
        self.assertIn("broken.py", problems[0])

    def test_a_backslash_continued_recipe_line_is_joined(self) -> None:
        """A recipe line continued over two physical lines must still name
        its script."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE + "\n\t@python3 scripts/docs/split.py \\\n\t    --flag"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/split.py": "import flask\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("imports 'flask'", problems[0])

    def test_an_untokenizable_recipe_line_is_a_finding(self) -> None:
        """A recipe line shlex cannot split must fail loudly, not crash or
        skip."""
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@python3 'scripts/docs/x.py"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertTrue(any("tokenize" in p for p in problems), problems)

    def test_an_interpreter_option_does_not_hide_the_script(self) -> None:
        """`python3 -B x.py` in a recipe, and `python3 -B x.py` inside a bash
        helper, both still name the script."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE
                    + "\n\t@python3 -W ignore -B scripts/docs/opts.py"
                    + "\n\t@bash scripts/docs/opts-helper.sh"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/opts.py": "import flask\n",
                "scripts/docs/opts-helper.sh": (
                    "#!/usr/bin/env bash\n" 'python3 -B "$SCRIPT_DIR/opts2.py" "$@"\n'
                ),
                "scripts/docs/opts2.py": "import httpx\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        joined = "\n".join(problems)
        self.assertIn("imports 'flask'", joined, problems)
        self.assertIn("imports 'httpx'", joined, problems)


if __name__ == "__main__":
    unittest.main()
