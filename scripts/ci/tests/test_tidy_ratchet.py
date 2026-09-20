# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for scripts/ci/tidy-ratchet.py (ADR-1142, ADR-1267)."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "tidy-ratchet.py"
REPO_ROOT = HERE.parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("tidy_ratchet", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


ratchet = _load()


class ParseDiagnostics(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "core" / "src").mkdir(parents=True)
        (self.root / "core" / "src" / "a.c").write_text("int x;\n", encoding="utf-8")
        (self.root / "core" / "src" / "a.h").write_text("", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_dedups_and_relativises(self) -> None:
        src = self.root / "core" / "src" / "a.c"
        out = "\n".join(
            [
                f"{src}:3:5: warning: use nullptr [modernize-use-nullptr]",
                f"{src}:3:5: warning: use nullptr [modernize-use-nullptr]",
                f"{src}:9:1: warning: too long [readability-function-size]",
                "/usr/include/stdio.h:1:1: warning: foo [bar-baz]",
                "12 warnings generated.",
            ]
        )
        diags, failed = ratchet.parse_diagnostics(out, self.root, self.root)
        self.assertFalse(failed)
        self.assertEqual(
            diags,
            {
                ("core/src/a.c", 3, 5, "modernize-use-nullptr"),
                ("core/src/a.c", 9, 1, "readability-function-size"),
            },
        )

    def test_warnings_as_errors_still_count(self) -> None:
        src = self.root / "core" / "src" / "a.c"
        out = f"{src}:1:1: error: discarded [cert-err33-c,-warnings-as-errors]"
        diags, failed = ratchet.parse_diagnostics(out, self.root, self.root)
        self.assertFalse(failed)
        self.assertEqual(len(diags), 1)

    def test_compile_error_fails_closed(self) -> None:
        src = self.root / "core" / "src" / "a.c"
        out = f"{src}:1:10: error: 'x.h' file not found [clang-diagnostic-error]"
        diags, failed = ratchet.parse_diagnostics(out, self.root, self.root)
        self.assertTrue(failed)
        self.assertEqual(diags, set())

    def test_relative_paths_resolve_against_cwd(self) -> None:
        out = "../core/src/a.h:2:2: warning: w [x-y]"
        build = self.root / "build"
        build.mkdir()
        diags, _ = ratchet.parse_diagnostics(out, self.root, build)
        self.assertEqual(diags, {("core/src/a.h", 2, 2, "x-y")})


class UncitedNolints(unittest.TestCase):
    def test_counts_only_uncited(self) -> None:
        text = "\n".join(
            [
                "int a; // NOLINT",
                "int z;",
                "int b; // NOLINTNEXTLINE(readability-function-size) ADR-0138",
                "int c; // NOLINT(cert-dcl37-c) ADR-0278",
                "int e;",
                "// NOLINTBEGIN(bugprone-macro-parentheses)",
                "int d;",
                "// NOLINTEND",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 2)

    def test_citation_on_next_line_in_same_slash_slash_run_counts(self) -> None:
        text = "int a; // NOLINT(cert-dcl37-c)\n// ADR-0278: reserved identifier kept for parity\n"
        self.assertEqual(ratchet.count_uncited_nolints(text), 0)

    def test_citation_anywhere_in_marker_block_comment_counts(self) -> None:
        # The ADR-1138 shape: a NOLINTBEGIN that explains itself over several
        # lines and cites the ADR on the last line of the same block comment.
        text = "\n".join(
            [
                "/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork",
                " * builds C as C23, but this is an upstream-mirror file whose source",
                " * spells the null pointer constant `NULL`.",
                " * MSVC's C23 feature set has no `nullptr`. ADR-1138. */",
                "int a;",
                "/* NOLINTEND(modernize-use-nullptr) */",
                "/* intro",
                " * NOLINT(x)",
                " * still the same comment",
                " * ADR-0002 */",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 0)

    def test_citation_in_the_following_comment_does_not_count(self) -> None:
        text = "\n".join(
            [
                "/* NOLINTBEGIN(x): no citation here",
                " */",
                "/* ADR-0001 belongs to the next comment */",
                "int z;",
                "/* NOLINT(y) */",
                "int a;",
                "/* ADR-9999 two lines later, not the marker's comment */",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 2)

    def test_citation_before_the_marker_in_the_same_block_counts(self) -> None:
        # ADR-1266: the fork's carve-out comments name the ADR in the prose and
        # put the marker on the block's *closing* line. A forward-only scan
        # stops at that `*/` and never sees the citation.
        text = "\n".join(
            [
                "/* Verbatim port of ssimulacra2.c::picture_to_linear_rgb (ADR-0141",
                " * carve-out: line-for-line scalar-diff parity).",
                " * NOLINTNEXTLINE(readability-function-size) */",
                "static void f(void) {}",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 0)

    def test_citation_before_marker_in_same_slash_slash_run_counts(self) -> None:
        text = "\n".join(
            [
                "// ADR-0141: pointer conversion is required by the CUDA ABI.",
                "// NOLINTBEGIN(performance-no-int-to-ptr)",
                "static void f(void) {}",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 0)

    def test_citation_on_the_last_line_of_a_slash_slash_run_counts(self) -> None:
        # The SYCL NOLINTBEGIN brackets are `//` runs, not `/* */` blocks, and
        # carry the citation on the run's final line.
        text = "\n".join(
            [
                "// NOLINTBEGIN(misc-use-anonymous-namespace): the entry points use",
                "// C-style `static` because their addresses are stored in the",
                '// `extern "C" VmafFeatureExtractor` struct at the bottom of this TU.',
                "// These are load-bearing invariants of the C-API ABI. ADR-0278.",
                "static int init(void) { return 0; }",
                "// NOLINTEND(misc-use-anonymous-namespace)",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 0)

    def test_citation_in_an_adjacent_slash_slash_run_does_not_count(self) -> None:
        # A blank line ends the run, so the citation belongs to another comment.
        text = "\n".join(
            [
                "// NOLINTBEGIN(misc-use-anonymous-namespace): no citation here",
                "",
                "// ADR-0278 belongs to a different comment",
                "static int init(void) { return 0; }",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 1)

    def test_code_line_ends_the_slash_slash_run(self) -> None:
        text = "\n".join(
            [
                "// NOLINTBEGIN(x): no citation here",
                "int a;",
                "// ADR-0278 is past the code line",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 1)

    def test_adjacent_block_comment_does_not_lend_its_citation(self) -> None:
        text = "/* NOLINT(x) */\n/* ADR-0001 belongs to another comment */\n"
        self.assertEqual(ratchet.count_uncited_nolints(text), 1)

    def test_preceding_block_comment_does_not_lend_its_citation(self) -> None:
        text = "/* ADR-0001 belongs to the rationale */\n/* NOLINT(x) */\n"
        self.assertEqual(ratchet.count_uncited_nolints(text), 1)

    def test_two_block_comments_on_one_line_remain_separate(self) -> None:
        text = "/* NOLINT(x) */ /* ADR-0001 belongs to another comment */\n"
        self.assertEqual(ratchet.count_uncited_nolints(text), 1)

    def test_code_and_string_text_at_block_boundaries_do_not_count(self) -> None:
        before = "\n".join(
            [
                'const char *tag = "ADR-0001"; /* explanation',
                " * filler",
                " * NOLINT(x) */",
            ]
        )
        after = "\n".join(
            [
                "/* NOLINT(x)",
                ' * filler */ const char *tag = "ADR-0001";',
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(before), 1)
        self.assertEqual(ratchet.count_uncited_nolints(after), 1)

    def test_comment_tokens_in_quoted_literals_are_ignored(self) -> None:
        text = "\n".join(
            [
                'const char *a = "/* NOLINT(x) ADR-0001 */";',
                'const char *b = R"tag(// NOLINT(y) ADR-0002)tag";',
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 0)

    def test_multiple_markers_and_nolintend_are_counted_exactly(self) -> None:
        text = "\n".join(
            [
                "// NOLINT(x) and NOLINTNEXTLINE(y)",
                "int a;",
                "// NOLINTEND(x)",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 2)

    def test_marker_prefix_inside_a_longer_identifier_is_ignored(self) -> None:
        text = "// NOLINTEND(x) NOLINTABLE NOLINTNEXTLINE_SUFFIX\n"
        self.assertEqual(ratchet.count_uncited_nolints(text), 0)

    def test_adr_token_must_be_exactly_four_digits(self) -> None:
        text = "// NOLINT(x): XADR-0001 and ADR-00012 are not citations\n"
        self.assertEqual(ratchet.count_uncited_nolints(text), 1)

    def test_no_markers(self) -> None:
        self.assertEqual(ratchet.count_uncited_nolints("int x;\n"), 0)

    def test_all_marker_count_includes_cited_and_closing_markers(self) -> None:
        text = "\n".join(
            [
                "// NOLINT(readability-function-size) ADR-0138",
                "// NOLINTNEXTLINE(cert-err33-c)",
                "int a;",
                "// NOLINTBEGIN(modernize-use-nullptr) ADR-1138",
                "int *p = NULL;",
                "// NOLINTEND(modernize-use-nullptr)",
                'const char *literal = "NOLINT(bugprone-test)";',
            ]
        )
        self.assertEqual(ratchet.count_nolint_markers(text), 4)


class Compare(unittest.TestCase):
    def _m(
        self,
        warnings: dict,
        nolint: dict | None = None,
        markers: dict | None = None,
    ) -> ratchet.Measurement:
        return ratchet.Measurement(
            lane="cpu",
            warnings=warnings,
            nolint_uncited=nolint or {},
            nolint_markers=markers or {},
        )

    def test_regression_and_slack(self) -> None:
        base = self._m({"a.c": 3, "b.c": 2}, {"a.c": 1})
        now = self._m({"a.c": 4, "b.c": 1, "c.c": 1}, {})
        regressions, slack = ratchet.compare(base, now)
        self.assertEqual(
            [(d.path, d.metric, d.change) for d in regressions],
            [("a.c", "warnings", 1), ("c.c", "warnings", 1)],
        )
        self.assertEqual(
            [(d.path, d.metric, d.change) for d in slack],
            [("b.c", "warnings", -1), ("a.c", "nolint_uncited", -1)],
        )

    def test_exit_codes(self) -> None:
        base = self._m({"a.c": 3})
        self.assertEqual(ratchet.report(base, self._m({"a.c": 3}), False), 0)
        self.assertEqual(ratchet.report(base, self._m({"a.c": 5}), False), 2)
        self.assertEqual(ratchet.report(base, self._m({"a.c": 1}), False), 3)
        self.assertEqual(ratchet.report(base, self._m({"a.c": 1}), True), 0)

    def test_strict_zero_passes_only_empty_measurement(self) -> None:
        zero = self._m({})
        self.assertEqual(ratchet.report(zero, zero, False, require_zero=True), 0)

    def test_strict_zero_rejects_matching_nonzero_warning_baseline(self) -> None:
        nonzero = self._m({"a.c": 1})
        self.assertEqual(
            ratchet.report(nonzero, nonzero, False, require_zero=True),
            ratchet.ZERO_DEBT_EXIT,
        )

    def test_strict_zero_rejects_matching_nonzero_nolint_baseline(self) -> None:
        nonzero = self._m({}, {"a.h": 2}, {"a.h": 2})
        self.assertEqual(
            ratchet.report(nonzero, nonzero, False, require_zero=True),
            ratchet.ZERO_DEBT_EXIT,
        )

    def test_strict_zero_rejects_cited_nolint_markers(self) -> None:
        nonzero = self._m({}, {}, {"a.h": 1})
        self.assertEqual(
            ratchet.report(nonzero, nonzero, False, require_zero=True),
            ratchet.ZERO_DEBT_EXIT,
        )

    def test_strict_zero_takes_precedence_over_ratchet_delta(self) -> None:
        base = self._m({"a.c": 3})
        improved_but_nonzero = self._m({"a.c": 1})
        self.assertEqual(
            ratchet.report(base, improved_but_nonzero, False, require_zero=True),
            ratchet.ZERO_DEBT_EXIT,
        )

    def test_baseline_round_trip(self) -> None:
        m = self._m({"b.c": 1, "a.c": 2}, {"a.c": 1}, {"a.c": 2})
        m.tus = 2
        data = json.loads(json.dumps(m.to_json()))
        back = ratchet.Measurement.from_json(data)
        self.assertEqual(back.warnings, {"a.c": 2, "b.c": 1})
        self.assertEqual(back.nolint_uncited, {"a.c": 1})
        self.assertEqual(back.nolint_markers, {"a.c": 2})
        self.assertEqual(back.tus, 2)
        self.assertEqual(list(data["warnings"]), ["a.c", "b.c"])

    def test_schema_mismatch_rejected(self) -> None:
        with self.assertRaises(ValueError):
            ratchet.Measurement.from_json({"schema": 99})


class CompileCommands(unittest.TestCase):
    def test_filters_to_in_repo_sources(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "core" / "src").mkdir(parents=True)
            (root / "subprojects" / "x").mkdir(parents=True)
            build = root / "build"
            build.mkdir()
            entries = [
                {"directory": str(build), "file": "../core/src/a.c", "command": "cc"},
                {"directory": str(build), "file": str(root / "core/src/a.c"), "command": "cc"},
                {"directory": str(build), "file": str(root / "subprojects/x/y.c"), "command": "cc"},
                {"directory": str(build), "file": "/usr/src/z.c", "command": "cc"},
                {"directory": str(build), "file": "../core/src/a.h", "command": "cc"},
            ]
            (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
            units = ratchet.load_compile_commands(build, root)
            self.assertEqual([u[0].relative_to(root).as_posix() for u in units], ["core/src/a.c"])


class LanguageSpecificChecks(unittest.TestCase):
    def test_nullptr_check_is_disabled_only_for_c_translation_units(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            for suffix in (".c", ".cpp", ".cu", ".hip"):
                source = root / f"unit{suffix}"
                source.write_text("int fixture;\n", encoding="utf-8")
                completed = mock.Mock(stdout="", stderr="", returncode=0)
                with mock.patch.object(ratchet, "run_command", return_value=completed) as run:
                    ratchet.run_one("clang-tidy", build, [], (source, root))
                argv = run.call_args.args[0]
                if suffix == ".c":
                    self.assertIn("--checks=-modernize-use-nullptr", argv)
                else:
                    self.assertNotIn("--checks=-modernize-use-nullptr", argv)


class StrictZeroMain(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.baseline = self.root / "baseline.json"
        self.report = self.root / "measurement.json"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def arguments(self, *extra: str) -> list[str]:
        return [
            "--repo-root",
            str(self.root),
            "--build-dir",
            str(self.root / "build"),
            "--baseline",
            str(self.baseline),
            "--require-zero",
            *extra,
        ]

    def write_baseline(self, measurement: ratchet.Measurement) -> None:
        self.baseline.write_text(
            json.dumps(measurement.to_json(), indent=2) + "\n",
            encoding="utf-8",
        )

    def test_matching_nonzero_baseline_fails_and_report_is_retained(self) -> None:
        measured = ratchet.Measurement(
            lane="cpu",
            tus=1,
            sources=["core/src/a.c"],
            warnings={"core/src/a.c": 1},
            diagnostics=["core/src/a.c:1:1: [bugprone-test]"],
        )
        self.write_baseline(measured)
        with mock.patch.object(ratchet, "measure", return_value=measured):
            code = ratchet.main(self.arguments("--report", str(self.report)))
        self.assertEqual(code, ratchet.ZERO_DEBT_EXIT)
        payload = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(payload["total_warnings"], 1)
        self.assertEqual(payload["diagnostics"], measured.diagnostics)

    def test_zero_measurement_passes(self) -> None:
        measured = ratchet.Measurement(lane="cpu", tus=1, sources=["core/src/a.c"])
        self.write_baseline(measured)
        with mock.patch.object(ratchet, "measure", return_value=measured):
            code = ratchet.main(self.arguments())
        self.assertEqual(code, 0)

    def test_compile_failure_fails_closed_after_writing_report(self) -> None:
        measured = ratchet.Measurement(
            lane="cpu",
            tus=1,
            sources=["core/src/a.c"],
            compile_failures=["core/src/a.c"],
        )
        self.write_baseline(ratchet.Measurement(lane="cpu"))
        with mock.patch.object(ratchet, "measure", return_value=measured):
            code = ratchet.main(self.arguments("--report", str(self.report)))
        self.assertEqual(code, 4)
        payload = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(payload["compile_failures"], ["core/src/a.c"])

    def test_strict_gate_rejects_partial_write_and_slack_modes(self) -> None:
        self.write_baseline(ratchet.Measurement(lane="cpu"))
        combinations = [
            ("--only", "core/src/a.c"),
            ("--write",),
            ("--allow-slack",),
        ]
        for extra in combinations:
            with self.subTest(extra=extra), mock.patch.object(ratchet, "measure") as measure:
                self.assertEqual(ratchet.main(self.arguments(*extra)), 5)
                measure.assert_not_called()


class StrictZeroWiring(unittest.TestCase):
    def test_default_make_target_is_strict_but_writer_is_inventory_only(self) -> None:
        makefile = (REPO_ROOT / "Makefile").read_text(encoding="utf-8")
        gate = makefile.split("tidy-ratchet:\n", 1)[1].split("tidy-ratchet-write:\n", 1)[0]
        writer = makefile.split("tidy-ratchet-write:\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("--require-zero", gate)
        self.assertNotIn("--only", gate)
        self.assertIn("--write", writer)
        self.assertNotIn("--require-zero", writer)

    def test_required_cpu_and_sycl_workflow_lanes_are_strict(self) -> None:
        workflow = (REPO_ROOT / ".github/workflows/lint-and-format.yml").read_text(encoding="utf-8")
        cpu = workflow.split("  clang-tidy-ratchet:\n", 1)[1].split("  clang-tidy-sycl:\n", 1)[0]
        sycl = workflow.split("  clang-tidy-sycl:\n", 1)[1].split("  cppcheck:\n", 1)[0]
        for lane in (cpu, sycl):
            self.assertIn("--require-zero", lane)
            self.assertIn("--report", lane)
            self.assertNotIn("--only", lane)


if __name__ == "__main__":
    unittest.main()
