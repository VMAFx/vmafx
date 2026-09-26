# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for scripts/ci/tidy-ratchet.py (ADR-1142)."""

from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SCRIPT = HERE.parent / "tidy-ratchet.py"


def _load():
    spec = importlib.util.spec_from_file_location("tidy_ratchet", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


ratchet = _load()


class ExactPelorusMirror(unittest.TestCase):
    def test_only_manifested_paths_are_lint_exempt(self) -> None:
        for path in (
            "core/include/libvmaf/pelorus/pelorus.h",
            "core/include/libvmaf/pelorus/interop.h",
            "core/include/libvmaf/pelorus/deband.h",
            "core/include/libvmaf/pelorus/denoise.h",
            "core/src/interop/pelorus_interop.c",
            "core/src/interop/pelorus_deband_params.c",
            "core/src/interop/pelorus_denoise_params.c",
            "core/src/interop/pelorus_qp_report_csv.c",
            "core/src/interop/pelorus_version.c",
            "core/test/test_pelorus_interop.c",
        ):
            with self.subTest(path=path):
                self.assertTrue(ratchet.is_exact_pelorus_mirror(path))

        for path in (
            "core/include/libvmaf/pelorus/unmanifested.h",
            "core/include/libvmaf/pelorus/nested/unmanifested.h",
            "core/src/interop/pelorus_unmanifested.c",
        ):
            with self.subTest(path=path):
                self.assertFalse(ratchet.is_exact_pelorus_mirror(path))


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

    def test_omits_diagnostics_from_exact_pelorus_headers(self) -> None:
        header = self.root / "core/include/libvmaf/pelorus/interop.h"
        header.parent.mkdir(parents=True)
        header.write_text("int x;\n", encoding="utf-8")
        out = f"{header}:1:1: warning: upstream spelling [modernize-use-nullptr]"
        diags, failed = ratchet.parse_diagnostics(out, self.root, self.root)
        self.assertFalse(failed)
        self.assertEqual(diags, set())


class UncitedNolints(unittest.TestCase):
    def test_counts_only_uncited(self) -> None:
        text = "\n".join(
            [
                "int a; // NOLINT",
                "int z;",
                "// ADR-0138 bit-exact per-lane reduction",
                "int b; // NOLINTNEXTLINE(readability-function-size)",
                "int c; // NOLINT(cert-dcl37-c) ADR-0278",
                "int e;",
                "// NOLINTBEGIN(bugprone-macro-parentheses)",
                "int d;",
                "// NOLINTEND",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 2)

    def test_citation_on_next_line_counts(self) -> None:
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

    def test_no_markers(self) -> None:
        self.assertEqual(ratchet.count_uncited_nolints("int x;\n"), 0)

    def test_scan_omits_exact_pelorus_headers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            owned = root / "core/include/libvmaf/owned.h"
            mirror = root / "core/include/libvmaf/pelorus/interop.h"
            owned.parent.mkdir(parents=True)
            mirror.parent.mkdir(parents=True)
            owned.write_text("int owned; // NOLINT\n", encoding="utf-8")
            mirror.write_text("int mirrored; // NOLINT\n", encoding="utf-8")

            self.assertEqual(
                ratchet.scan_nolints(root, [], "cpu"),
                {"core/include/libvmaf/owned.h": 1},
            )


class Compare(unittest.TestCase):
    def _m(self, warnings: dict, nolint: dict | None = None) -> ratchet.Measurement:
        return ratchet.Measurement(lane="cpu", warnings=warnings, nolint_uncited=nolint or {})

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

    def test_baseline_round_trip(self) -> None:
        m = self._m({"b.c": 1, "a.c": 2}, {"a.c": 1})
        m.tus = 2
        data = json.loads(json.dumps(m.to_json()))
        back = ratchet.Measurement.from_json(data)
        self.assertEqual(back.warnings, {"a.c": 2, "b.c": 1})
        self.assertEqual(back.nolint_uncited, {"a.c": 1})
        self.assertEqual(back.tus, 2)
        self.assertEqual(list(data["warnings"]), ["a.c", "b.c"])

    def test_schema_mismatch_rejected(self) -> None:
        with self.assertRaises(ValueError):
            ratchet.Measurement.from_json({"schema": 99})

    def test_legacy_baseline_omits_exact_pelorus_mirror(self) -> None:
        data = {
            "schema": 1,
            "lane": "cpu",
            "tus": 3,
            "measured_sources": [
                "core/src/a.c",
                "core/src/interop/pelorus_interop.c",
                "core/test/test_pelorus_interop.c",
            ],
            "warnings": {
                "core/include/libvmaf/pelorus/interop.h": 4,
                "core/src/a.c": 2,
                "core/src/interop/pelorus_interop.c": 10,
                "core/test/test_pelorus_interop.c": 9,
            },
            "nolint_uncited": {
                "core/src/a.c": 1,
                "core/test/test_pelorus_interop.c": 2,
            },
        }
        baseline = ratchet.Measurement.from_json(data)
        self.assertEqual(baseline.tus, 1)
        self.assertEqual(baseline.sources, ["core/src/a.c"])
        self.assertEqual(baseline.warnings, {"core/src/a.c": 2})
        self.assertEqual(baseline.nolint_uncited, {"core/src/a.c": 1})


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

    def test_omits_exact_pelorus_mirror_translation_units(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            build.mkdir()
            entries = []
            for rel in (
                "core/src/a.c",
                "core/src/interop/pelorus_interop.c",
                "core/test/test_pelorus_interop.c",
            ):
                path = root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("int x;\n", encoding="utf-8")
                entries.append({"directory": str(build), "file": str(path), "command": "cc"})
            (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

            units = ratchet.load_compile_commands(build, root)
            self.assertEqual([u[0].relative_to(root).as_posix() for u in units], ["core/src/a.c"])


def _fake_clang_tidy(directory: Path) -> Path:
    """A clang-tidy stand-in that prints its argv and succeeds."""
    script = directory / "fake-clang-tidy.sh"
    script.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n', encoding="utf-8")
    script.chmod(0o755)
    return script


class RunClangTidy(unittest.TestCase):
    def test_extra_args_reach_the_compiler(self) -> None:
        # The GPU lanes pass --cuda-host-only / -x hip; clang-tidy only accepts
        # them wrapped in its own --extra-arg.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = _fake_clang_tidy(root)
            _source, output, returncode = ratchet.run_one(
                str(binary),
                Path("build"),
                ["--cuda-host-only", "-nocudalib"],
                (root / "a.cu", root),
            )
            self.assertEqual(returncode, 0)
            argv = output.split()
            self.assertIn("--extra-arg=--cuda-host-only", argv)
            self.assertIn("--extra-arg=-nocudalib", argv)
            self.assertNotIn("--cuda-host-only", argv)
            self.assertTrue(Path(argv[argv.index("-p") + 1]).is_absolute())
            # A value that already carries the wrapper passes through once, not
            # twice: the lanes mix both spellings in TIDY_RATCHET_EXTRA_*.
            self.assertNotIn("--extra-arg=--extra-arg=-nocudalib", argv)

    def test_relative_wrapper_path_survives_the_tu_directory(self) -> None:
        # clang-tidy runs in each TU's directory; the SYCL lane names its wrapper
        # relative to the repository root.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "core").mkdir()
            (root / "core" / "a.c").write_text("int a;\n", encoding="utf-8")
            build = root / "build"
            build.mkdir()
            entries = [{"directory": str(build), "file": str(root / "core/a.c"), "command": "cc"}]
            (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
            _fake_clang_tidy(root)
            previous = Path.cwd()
            os.chdir(root)
            try:
                measured = ratchet.measure("sycl", build, root, "./fake-clang-tidy.sh", [], 1)
            finally:
                os.chdir(previous)
            self.assertEqual(measured.compile_failures, [])
            self.assertEqual(measured.tus, 1)

    def test_relative_wrapper_path_in_subdirectory_survives_safe_subprocess(self) -> None:
        # Repository-relative wrapper (scripts/ci/clang-tidy-sycl.sh) has multiple
        # path components; safe_subprocess rejects relative executables. tidy-ratchet
        # must resolve it to an absolute path so version probing and per-TU execution pass.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "core").mkdir()
            (root / "core" / "a.c").write_text("int a;\n", encoding="utf-8")
            (root / "scripts" / "ci").mkdir(parents=True)
            script = root / "scripts" / "ci" / "fake-clang-tidy.sh"
            script.write_text(
                '#!/bin/sh\nprintf "LLVM version 22.0.0\\n%s\\n" "$@"\n', encoding="utf-8"
            )
            script.chmod(0o755)
            build = root / "build"
            build.mkdir()
            entries = [{"directory": str(build), "file": str(root / "core/a.c"), "command": "cc"}]
            (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

            measured = ratchet.measure("sycl", build, root, "scripts/ci/fake-clang-tidy.sh", [], 1)
            self.assertEqual(measured.compile_failures, [])
            self.assertEqual(measured.tus, 1)
            self.assertEqual(measured.clang_tidy_version, "22.0.0")

            report = root / "report.json"
            baseline = root / "scripts" / "ci" / "tidy-baseline-sycl.json"
            baseline.write_text(json.dumps(measured.to_json()), encoding="utf-8")
            exit_code = ratchet.main(
                [
                    "--lane",
                    "sycl",
                    "--build-dir",
                    str(build),
                    "--repo-root",
                    str(root),
                    "--clang-tidy",
                    "scripts/ci/fake-clang-tidy.sh",
                    "--report",
                    str(report),
                ]
            )
            self.assertEqual(exit_code, 0)


class ResolveClangTidy(unittest.TestCase):
    def test_bare_executable_is_preserved_or_resolved(self) -> None:
        self.assertEqual(ratchet.resolve_clang_tidy("nonexistent-tool-xyz"), "nonexistent-tool-xyz")

    def test_absolute_executable_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tool = Path(tmp) / "tool.sh"
            tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            tool.chmod(0o755)
            self.assertEqual(ratchet.resolve_clang_tidy(str(tool)), str(tool.resolve()))

    def test_relative_path_resolves_against_repo_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "scripts" / "ci").mkdir(parents=True)
            tool = root / "scripts" / "ci" / "wrapper.sh"
            tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            tool.chmod(0o755)
            resolved = ratchet.resolve_clang_tidy("scripts/ci/wrapper.sh", root)
            self.assertEqual(resolved, str(tool.resolve()))


class SyclLaneFlags(unittest.TestCase):
    def test_lane_wrapper_uses_curdir_absolute_path(self) -> None:
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        for line in text.splitlines():
            if line.startswith("TIDY_RATCHET_EXTRA_sycl"):
                self.assertIn("$(CURDIR)/scripts/ci/clang-tidy-sycl.sh", line)
                return
        self.fail("Makefile defines no TIDY_RATCHET_EXTRA_sycl lane")


class Arm64LaneFlags(unittest.TestCase):
    """The arm64 lane's cross flags are load-bearing (ADR-1283).

    Its compile database is produced by ``aarch64-linux-gnu-gcc`` from
    ``build-aux/aarch64-linux-gnu.ini``. clang-tidy parses those commands with
    its own driver, so without ``--target`` it reads ``<arm_neon.h>`` against
    the host's x86 headers and every NEON translation unit becomes a
    ``clang-diagnostic-error`` -- ratchet exit 4, a failed measurement rather
    than a clean one. Without ``--sysroot`` libc resolves against the host.
    Dropping either flag silently turns the lane into the very defect it
    exists to close, so the Makefile's definition is pinned here.
    """

    def lane_flags(self) -> str:
        """Return TIDY_RATCHET_EXTRA_arm64's value, backslash continuations joined."""
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        joined = text.replace("\\\n", " ")
        for line in joined.splitlines():
            if line.startswith("TIDY_RATCHET_EXTRA_arm64"):
                return line.split(":=", 1)[1]
        self.fail("Makefile defines no TIDY_RATCHET_EXTRA_arm64 lane")
        raise AssertionError  # unreachable; keeps the return type honest

    def test_lane_forwards_target_and_sysroot(self) -> None:
        flags = self.lane_flags().split()
        self.assertIn("--extra-arg=--target=$(AARCH64_TARGET)", flags)
        self.assertIn("--extra-arg=--sysroot=$(AARCH64_SYSROOT)", flags)

    def test_lane_defaults_are_the_cross_packages_own_paths(self) -> None:
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("AARCH64_TARGET ?= aarch64-linux-gnu", text)
        self.assertIn("AARCH64_SYSROOT ?= /usr/aarch64-linux-gnu", text)

    def test_a_baseline_exists_for_the_lane(self) -> None:
        """A lane with no committed baseline measures nothing on the next run."""
        self.assertTrue((ROOT / "scripts/ci/tidy-baseline-arm64.json").is_file())


class SyclMotionAddUvParityTidyContract(unittest.TestCase):
    """The SYCL tidy lane measures test_sycl_motion_add_uv_parity.c at zero baseline.

    Under readability-function-size (BranchThreshold 15), repeated mu_assert
    ladders counted as branches and tripped the ratchet on run_sycl_pass_add_uv
    and run_sycl_pass_y_only (T-SYCL-RATCHET-TEST-BRANCH-COUNT-2026-09-22).
    This contract proves:
      1. test_sycl_motion_add_uv_parity.c is in measured_sources with 0 baseline debt.
      2. Any diagnostic on that file (including the old 2-warning shape) is rejected
         as a regression.
      3. test_sycl_motion3_parity.c is also measured in the SYCL lane.
    """

    def setUp(self) -> None:
        self.baseline_path = ROOT / "scripts/ci/tidy-baseline-sycl.json"
        self.data = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        self.baseline = ratchet.Measurement.from_json(self.data)
        self.target_source = "core/test/test_sycl_motion_add_uv_parity.c"

    def test_sycl_baseline_measures_source_with_zero_allowance(self) -> None:
        self.assertIn(self.target_source, self.data["measured_sources"])
        self.assertNotIn(self.target_source, self.data.get("warnings", {}))
        self.assertNotIn(self.target_source, self.data.get("nolint_uncited", {}))
        self.assertEqual(self.baseline.warnings.get(self.target_source, 0), 0)

    def test_old_unrefactored_warning_shape_fails_ratchet_contract(self) -> None:
        """The old shape produced 2 readability-function-size warnings (0 -> 2 (+2))."""
        old_output = "\n".join(
            [
                f"{self.target_source}:169:14: warning: function 'run_sycl_pass_add_uv' exceeds recommended size/complexity thresholds [readability-function-size]",
                f"{self.target_source}:218:14: warning: function 'run_sycl_pass_y_only' exceeds recommended size/complexity thresholds [readability-function-size]",
            ]
        )
        diags, failed = ratchet.parse_diagnostics(old_output, ROOT, ROOT)
        self.assertFalse(failed)
        self.assertEqual(len(diags), 2)

        measured = ratchet.Measurement(lane="sycl", tus=self.baseline.tus)
        for path, _line, _col, _check in diags:
            measured.warnings[path] = measured.warnings.get(path, 0) + 1

        regressions, _slack = ratchet.compare(self.baseline, measured)
        target_regressions = [r for r in regressions if r.path == self.target_source]
        self.assertEqual(len(target_regressions), 1)
        self.assertEqual(target_regressions[0].baseline, 0)
        self.assertEqual(target_regressions[0].measured, 2)
        self.assertEqual(target_regressions[0].change, 2)

    def test_motion3_checkerboard_also_measured_in_sycl_lane(self) -> None:
        """core/test/test_sycl_motion3_parity.c is in measured_sources."""
        self.assertIn("core/test/test_sycl_motion3_parity.c", self.data["measured_sources"])


if __name__ == "__main__":
    unittest.main()
