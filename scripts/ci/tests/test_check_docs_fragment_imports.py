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

# The graceful fallback of scripts/docs/generate-charts.py.
GUARDED_RENDERER = (
    "import importlib\n"
    "def load_renderer():\n"
    "    try:\n"
    "        return importlib.import_module('vl_convert')\n"
    "    except ImportError:\n"
    "        return None\n"
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
                    "scripts/docs/generate-charts.py": GUARDED_RENDERER,
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

    def test_an_env_prefix_does_not_hide_the_script(self) -> None:
        """`env LC_ALL=C python3 x.py` runs x.py and is scanned."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE + "\n\t@env LC_ALL=C python3 scripts/docs/env.py"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/env.py": "import flask\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertIn("imports 'flask'", "\n".join(problems), problems)

    def test_a_make_variable_interpreter_names_the_script(self) -> None:
        """`$(PYTHON) x.py` holds an interpreter and still names x.py."""
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@$(PYTHON) scripts/docs/makevar.py"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/makevar.py": "import flask\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertIn("imports 'flask'", "\n".join(problems), problems)

    def test_an_absolute_interpreter_path_names_the_script(self) -> None:
        """`/usr/bin/python3 x.py` runs x.py, not nothing."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE + "\n\t@/usr/bin/python3 scripts/docs/absolute.py"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/absolute.py": "import flask\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertIn("imports 'flask'", "\n".join(problems), problems)

    def test_a_versioned_interpreter_name_names_the_script(self) -> None:
        """`python3.14 x.py` runs x.py, not nothing."""
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@python3.14 scripts/docs/versioned.py"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/versioned.py": "import flask\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertIn("imports 'flask'", "\n".join(problems), problems)

    def test_a_module_run_is_a_finding(self) -> None:
        """`python3 -m pkg.mod` names no script the scan can read; it must fail
        loudly rather than drop the module from the scan."""
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@python3 -m scripts.docs.tool"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertTrue(any("python -m" in problem for problem in problems), problems)

    def test_a_value_option_does_not_hide_the_script(self) -> None:
        """`-X utf8` takes a value and `-x` does not (`python3 --help`): in a
        recipe and in a bash helper, both still name the script."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE
                    + "\n\t@python3 -X utf8 scripts/docs/xvalue.py"
                    + "\n\t@python3 -x scripts/docs/xflag.py"
                    + "\n\t@bash scripts/docs/x-helper.sh"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/xvalue.py": "import flask\n",
                "scripts/docs/xflag.py": "import httpx\n",
                "scripts/docs/x-helper.sh": (
                    "#!/usr/bin/env bash\n" 'python3 -X utf8 "$SCRIPT_DIR/xhelper.py"\n'
                ),
                "scripts/docs/xhelper.py": "import requests\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        joined = "\n".join(problems)
        for name in ("flask", "httpx", "requests"):
            self.assertIn(f"imports '{name}'", joined, problems)

    def test_code_and_module_runs_are_findings_whatever_follows(self) -> None:
        """`-c` and `-m` end the option list, so a later `.py` word is an
        argument, not the script; each line is a finding."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE
                    + "\n\t@python3 -m pytest scripts/docs/arg.py"
                    + "\n\t@python3 -c 'import flask'"
                    + "\n\t@python3 -Bc 'import flask'"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/arg.py": "import os\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertEqual(sum("python -m" in p for p in problems), 3, problems)

    def test_a_helper_reads_interpreters_like_the_recipe(self) -> None:
        """A bash helper follows the recipe's rules: a versioned interpreter is
        scanned, and a `-m` or `-c` run is a finding, not a skip, a scan of
        the module's argument, or a missing file named after the code."""
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@bash scripts/docs/forms.sh"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/forms.sh": (
                    "#!/usr/bin/env bash\n"
                    'python3.14 "$SCRIPT_DIR/versioned.py"\n'
                    'python3 -m pytest "$SCRIPT_DIR/argument.py"\n'
                    "python3 -c 'import flask'\n"
                ),
                "scripts/docs/versioned.py": "import httpx\n",
                "scripts/docs/argument.py": "import requests\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        joined = "\n".join(problems)
        self.assertIn("imports 'httpx'", joined, problems)
        self.assertNotIn("imports 'requests'", joined, problems)
        self.assertNotIn("does not exist", joined, problems)
        self.assertEqual(sum("python -m" in p for p in problems), 2, problems)

    def test_a_space_indented_continuation_keeps_the_recipe(self) -> None:
        """make joins a backslash continuation whatever its indentation; the
        recipe lines after it are still scanned."""
        problems = self.findings(
            {
                "Makefile": makefile(
                    CHECKER_LINE
                    + "\n\t@python3 scripts/docs/first.py \\\n  --flag"
                    + "\n\t@python3 scripts/docs/after.py"
                ),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/first.py": "import os\n",
                "scripts/docs/after.py": "import flask\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertIn("imports 'flask'", "\n".join(problems), problems)

    def test_a_relative_import_runs_the_intermediate_package_init(self) -> None:
        """`from .pkg.helper import value` runs pkg/__init__.py too."""
        problems = self.findings(
            {
                "Makefile": makefile(CHECKER_LINE + "\n\t@python3 scripts/docs/wrapper.py"),
                "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                "scripts/docs/wrapper.py": "from .pkg.helper import value\n",
                "scripts/docs/pkg/__init__.py": "import flask\n",
                "scripts/docs/pkg/helper.py": "value = 1\n",
                "requirements/locks/docs-check.in": DOCS_CHECK_IN,
            }
        )
        self.assertIn("imports 'flask'", "\n".join(problems), problems)

    def test_an_optional_import_without_its_fallback_is_a_finding(self) -> None:
        """The vl_convert exemption holds only in generate-charts.py and only
        behind its ImportError fallback."""
        unguarded = "import vl_convert\n"
        for name, script, body in (
            ("fallback removed", "scripts/docs/generate-charts.py", unguarded),
            ("another importer", "scripts/docs/other.py", GUARDED_RENDERER),
        ):
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp:
                planted = {
                    "Makefile": makefile(CHECKER_LINE + f"\n\t@python3 {script}"),
                    "scripts/ci/check-docs-fragment-imports.py": "# stub\n",
                    "requirements/locks/docs-check.in": DOCS_CHECK_IN,
                    script: body,
                }
                problems = module.findings(plant(Path(tmp), planted))
                self.assertIn("imports 'vl_convert'", "\n".join(problems), problems)


if __name__ == "__main__":
    unittest.main()
