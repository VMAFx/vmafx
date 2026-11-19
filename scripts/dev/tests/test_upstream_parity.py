#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The upstream parity guard without a build: documents, comparison, verdict (ADR-1487).

The fixtures under ``fixtures/upstream_parity/`` are two small result documents
(what the harness printed for upstream and for this tree) and an allowlist.
Every case edits a copy of them, so the guard's three failures (a difference
nothing covers, a difference above its bound, a stale fragment) are each
planted and seen to fail.
"""

from __future__ import annotations

import contextlib
import copy
import dataclasses
import io
import json
import math
import os
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import upstream_parity_allowlist as allowlist  # noqa: E402
from scripts.dev import upstream_parity as guard  # noqa: E402
from scripts.dev import upstream_parity_matrix as matrix_data  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "upstream_parity"
CIEDE = "scalar/nflx422p10/F.ciede.default"
PSNR = "scalar/nflx8/F.psnr.default"
CAMBI = "scalar/q160x90/F.cambi.default"


def documents() -> tuple[dict[str, Any], dict[str, Any]]:
    upstream = json.loads((FIXTURES / "upstream.json").read_text(encoding="utf-8"))
    fork = json.loads((FIXTURES / "fork.json").read_text(encoding="utf-8"))
    return upstream, fork


def fragments() -> tuple[allowlist.Fragment, ...]:
    return allowlist.load_fragments(FIXTURES / "allowlist.d", FIXTURES / "adr")


def verdict(
    upstream: dict[str, Any],
    fork: dict[str, Any],
    entries: tuple[allowlist.Fragment, ...] | None = None,
) -> tuple[int, str]:
    status, lines = guard.judge(upstream, fork, fragments() if entries is None else entries, {})
    return status, "\n".join(lines)


class Verdict(unittest.TestCase):
    def test_the_fixture_documents_pass(self) -> None:
        status, text = verdict(*documents())
        self.assertEqual(status, guard.EXIT_PASS, text)
        self.assertIn("upstream parity: PASS", text)
        self.assertIn("ciede.chroma-422 [value] 2 differences", text)
        self.assertIn("cambi.small-frames [error] 1 differences", text)

    def test_identical_documents_with_an_empty_allowlist_pass(self) -> None:
        upstream, _ = documents()
        status, text = verdict(upstream, copy.deepcopy(upstream), ())
        self.assertEqual(status, guard.EXIT_PASS, text)

    def test_a_planted_difference_nothing_covers_fails(self) -> None:
        upstream, fork = documents()
        fork["runs"][PSNR]["values"]["1|psnr_y"] = "30.000000000000004"
        status, text = verdict(upstream, fork)
        self.assertEqual(status, guard.EXIT_DIFFERENT)
        self.assertIn("NOT COVERED (1)", text)
        self.assertIn(f"{PSNR} psnr_y#1", text)

    def test_a_difference_above_its_bound_fails(self) -> None:
        upstream, fork = documents()
        fork["runs"][CIEDE]["values"]["0|ciede2000"] = "32.9"
        status, text = verdict(upstream, fork)
        self.assertEqual(status, guard.EXIT_DIFFERENT)
        self.assertIn("ABOVE BOUND (1)", text)
        self.assertIn("ciede.chroma-422 allows 0.2", text)

    def test_a_difference_at_its_bound_passes(self) -> None:
        upstream, fork = documents()
        upstream["runs"][CIEDE]["values"]["0|ciede2000"] = "33.25"
        fork["runs"][CIEDE]["values"]["0|ciede2000"] = "33.0"
        entries = tuple(
            fragment if fragment.name != "ciede.chroma-422" else _with_bound(fragment, 0.25)
            for fragment in fragments()
        )
        self.assertEqual(verdict(upstream, fork, entries)[0], guard.EXIT_PASS)
        entries = tuple(
            (
                fragment
                if fragment.name != "ciede.chroma-422"
                else _with_bound(fragment, math.nextafter(0.25, 0.0))
            )
            for fragment in fragments()
        )
        self.assertEqual(verdict(upstream, fork, entries)[0], guard.EXIT_DIFFERENT)

    def test_a_fragment_that_matches_nothing_any_more_fails(self) -> None:
        upstream, fork = documents()
        fork["runs"][CIEDE] = copy.deepcopy(upstream["runs"][CIEDE])
        status, text = verdict(upstream, fork)
        self.assertEqual(status, guard.EXIT_DIFFERENT)
        self.assertIn("STALE", text)
        self.assertIn("ciede.chroma-422", text)

    def test_a_filtered_run_set_reports_a_stale_fragment_without_failing(self) -> None:
        upstream, fork = documents()
        fork["runs"][CIEDE] = copy.deepcopy(upstream["runs"][CIEDE])
        status, lines = guard.judge(upstream, fork, fragments(), {}, complete=False)
        text = "\n".join(lines)
        self.assertEqual(status, guard.EXIT_PASS, text)
        self.assertNotIn("STALE", text)
        self.assertIn(
            "fragments without a difference in this slice of the matrix (not judged) (1)", text
        )

    def test_a_skipped_fixture_is_named_in_the_report(self) -> None:
        upstream, fork = documents()
        status, lines = guard.judge(upstream, fork, fragments(), {"bbb4k": "not installed"})
        self.assertEqual(status, guard.EXIT_PASS)
        self.assertIn("fixture not compared: bbb4k: not installed", lines)

    def test_a_crash_of_this_trees_harness_fails_whatever_the_allowlist_says(self) -> None:
        upstream, fork = documents()
        fork["runs"][CAMBI] = {"status": "crash", "detail": "signal 11", "values": {}, "pooled": {}}
        upstream["runs"][CAMBI] = {
            "status": "crash",
            "detail": "signal 11",
            "values": {},
            "pooled": {},
        }
        status, text = verdict(upstream, fork)
        self.assertEqual(status, guard.EXIT_DIFFERENT)
        self.assertIn("FORK HARNESS CRASHED (1)", text)

    def test_documents_with_different_runs_cannot_be_compared(self) -> None:
        upstream, fork = documents()
        del fork["runs"][PSNR]
        with self.assertRaisesRegex(guard.CannotRun, "different runs"):
            guard.compare_documents(upstream, fork)


def _with_bound(fragment: allowlist.Fragment, bound: float) -> allowlist.Fragment:
    return dataclasses.replace(fragment, bound=bound)


class Comparison(unittest.TestCase):
    def test_what_counts_as_the_same_number(self) -> None:
        same = (("1.5", "1.5"), ("nan", "-nan"), ("-nan", "nan"), ("inf", "inf"))
        for upstream, fork in same:
            with self.subTest(upstream=upstream, fork=fork):
                self.assertIsNone(guard.value_difference(upstream, fork))
        self.assertEqual(guard.value_difference("0", "-0"), 0.0)
        self.assertEqual(guard.value_difference("1", "1.5"), 0.5)
        for upstream, fork in (("nan", "1"), ("1", "inf"), ("inf", "-inf"), ("1", "junk")):
            with self.subTest(upstream=upstream, fork=fork):
                self.assertEqual(guard.value_difference(upstream, fork), math.inf)

    def test_every_kind_of_difference_is_found(self) -> None:
        differences, executed, totals = guard.compare_documents(*documents())
        self.assertEqual(len(executed), 4)
        self.assertEqual(totals.runs, 4)
        self.assertEqual(totals.both_ok, 3)
        kinds = sorted((item.what, item.run, item.metric, item.frame) for item in differences)
        self.assertEqual(
            kinds,
            [
                ("name", "F.psnr_hvs.default", "psnr_hvs_y", "*"),
                ("status", "F.cambi.default", "", ""),
                ("value", "F.ciede.default", "ciede2000", "0"),
                ("value", "F.ciede.default", "ciede2000", "1"),
            ],
        )

    def test_a_pooled_difference_is_derived_only_when_frames_differ(self) -> None:
        upstream, fork = documents()
        _, _, totals = guard.compare_documents(upstream, fork)
        self.assertEqual(totals.derived, 2)
        fork["runs"][PSNR]["pooled"]["mean|psnr_y"] = "30.25"
        differences, _, _ = guard.compare_documents(upstream, fork)
        pooled = [item for item in differences if item.frame.startswith("pool-")]
        self.assertEqual([(item.metric, item.frame) for item in pooled], [("psnr_y", "pool-mean")])

    def test_both_sides_failing_is_not_a_difference(self) -> None:
        upstream, fork = documents()
        for document in (upstream, fork):
            document["runs"][PSNR] = {
                "status": "error",
                "detail": "ERR flush -22",
                "values": {},
                "pooled": {},
            }
        differences, _, totals = guard.compare_documents(upstream, fork)
        self.assertEqual(totals.both_failed, 1)
        self.assertFalse([item for item in differences if item.run == "F.psnr.default"])


class HarnessOutput(unittest.TestCase):
    def test_a_complete_run(self) -> None:
        text = "0 psnr_y 30\n1 psnr_y 31\npool psnr_y 30.5 30.4\nagg apsnr_y 30.4\nEND 2\n"
        parsed = guard.parse_output(text, 0)
        self.assertEqual(parsed["status"], "ok")
        self.assertEqual(
            parsed["values"], {"0|psnr_y": "30", "1|psnr_y": "31", "agg|apsnr_y": "30.4"}
        )
        self.assertEqual(parsed["pooled"], {"mean|psnr_y": "30.5", "harmonic|psnr_y": "30.4"})

    def test_a_run_without_values_is_still_a_run(self) -> None:
        self.assertEqual(guard.parse_output("END 3\n", 0)["status"], "ok")

    def test_failures(self) -> None:
        cases = {
            ("ERR flush -22\n", 9): ("error", "ERR flush -22"),
            ("0 psnr_y 30\n", 0): ("error", "exit 0"),
            ("error: scale below 1x1!\nEND 1\n", 0): ("error", "error: scale below 1x1!"),
            ("", -11): ("crash", "signal 11"),
            ("0 psnr_y 30\nEND 1\n", 3): ("error", "exit 3"),
        }
        for (text, returncode), (status, detail) in cases.items():
            with self.subTest(text=text, returncode=returncode):
                parsed = guard.parse_output(text, returncode)
                self.assertEqual((parsed["status"], parsed["detail"]), (status, detail))
                self.assertEqual(parsed["values"], {})


class CommandLine(unittest.TestCase):
    def run_main(self, *argv: str) -> tuple[int, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = guard.main(list(argv))
        return status, out.getvalue() + err.getvalue()

    def test_compare_only_mode(self) -> None:
        status, text = self.run_main(
            "--from-json",
            str(FIXTURES / "upstream.json"),
            str(FIXTURES / "fork.json"),
            "--fragments",
            str(FIXTURES / "allowlist.d"),
            "--adr-dir",
            str(FIXTURES / "adr"),
        )
        self.assertEqual(status, guard.EXIT_PASS, text)
        self.assertIn("upstream parity: PASS", text)

    def test_a_malformed_allowlist_cannot_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "ciede.bad").write_text("kind: tolerance\nevidence: e\n", encoding="utf-8")
            status, text = self.run_main(
                "--from-json",
                str(FIXTURES / "upstream.json"),
                str(FIXTURES / "fork.json"),
                "--fragments",
                tmp,
            )
        self.assertEqual(status, guard.EXIT_CANNOT_RUN)
        self.assertIn("kind must be one of", text)

    def test_an_unreadable_document_cannot_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.json"
            bad.write_text("{}", encoding="utf-8")
            status, text = self.run_main("--from-json", str(bad), str(bad))
        self.assertEqual(status, guard.EXIT_CANNOT_RUN)
        self.assertIn("not a result document", text)


class Builds(unittest.TestCase):
    def test_the_compilers_of_a_configured_build_are_read(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            info = Path(tmp) / "meson-info"
            info.mkdir()
            compilers = {"host": {"c": {"exelist": ["gcc"]}, "cpp": {"exelist": ["g++"]}}}
            (info / "intro-compilers.json").write_text(json.dumps(compilers), encoding="utf-8")
            self.assertEqual(guard.golden_compilers(Path(tmp)), ("gcc", "g++"))

    def test_an_unconfigured_build_cannot_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(guard.CannotRun, "cannot read the build's compilers"):
                guard.golden_compilers(Path(tmp))
            info = Path(tmp) / "meson-info"
            info.mkdir()
            (info / "intro-compilers.json").write_text('{"host": {}}', encoding="utf-8")
            with self.assertRaisesRegex(guard.CannotRun, "cannot read the build's compilers"):
                guard.golden_compilers(Path(tmp))

    def test_a_cached_run_is_identified_by_its_request_and_its_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            clip = Path(tmp) / "clip.yuv"
            clip.write_bytes(bytes(8))
            first = guard._request_digest(["harness", "a", "3"], [clip])
            self.assertEqual(first, guard._request_digest(["other-harness", "a", "3"], [clip]))
            self.assertNotEqual(first, guard._request_digest(["harness", "a", "6"], [clip]))
            clip.write_bytes(bytes(16))
            self.assertNotEqual(first, guard._request_digest(["harness", "a", "3"], [clip]))


class PinnedEnvironment(unittest.TestCase):
    def test_the_image_is_known_only_inside_a_container_started_for_the_guard(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / ".dockerenv"
            marker.touch()
            variable = {guard.IMAGE_VARIABLE: "sha256:abc"}
            self.assertEqual(guard.container_image(variable, [marker]), "sha256:abc")
            self.assertEqual(guard.container_image(variable, [Path(tmp) / "absent"]), "")
            self.assertEqual(guard.container_image({}, [marker]), "")
            self.assertEqual(guard.container_image({guard.IMAGE_VARIABLE: ""}, [marker]), "")

    def test_each_environment_has_its_own_builds(self) -> None:
        workdir = Path("work")
        self.assertEqual(guard.environment_dir(workdir, ""), workdir / "host")
        self.assertEqual(
            guard.environment_dir(workdir, "sha256:0123456789abcdef99"),
            workdir / "image-0123456789ab",
        )

    def test_documents_of_the_pinned_environment_name_it(self) -> None:
        status, text = verdict(*documents())
        self.assertEqual(status, guard.EXIT_PASS, text)
        self.assertTrue(text.startswith("environment: image sha256:0123456789abcdef;"), text)
        self.assertNotIn("advisory", text)

    def test_unpinned_documents_are_refused_unless_marked_advisory(self) -> None:
        upstream, fork = documents()
        for document in (upstream, fork):
            document["environment"]["pinned"] = False
            document["environment"]["image"] = "none"
        with self.assertRaisesRegex(guard.CannotRun, "not in the pinned environment"):
            guard.judge(upstream, fork, fragments(), {})
        status, lines = guard.judge(upstream, fork, fragments(), {}, allow_unpinned=True)
        self.assertEqual(status, guard.EXIT_PASS)
        self.assertEqual(lines[-1], "upstream parity: PASS (advisory: environment not pinned)")
        self.assertIn("NOT PINNED", lines[0])

    def test_documents_of_two_environments_cannot_be_compared(self) -> None:
        upstream, fork = documents()
        fork["environment"]["libc"] = "glibc 2.44"
        with self.assertRaisesRegex(guard.CannotRun, "not measured in one environment"):
            guard.judge(upstream, fork, fragments(), {})
        del fork["environment"]
        with self.assertRaisesRegex(guard.CannotRun, "not measured in one environment"):
            guard.judge(upstream, fork, fragments(), {}, allow_unpinned=True)

    def test_measuring_outside_the_container_is_refused_before_anything_is_built(self) -> None:
        with mock.patch.dict(os.environ, {guard.IMAGE_VARIABLE: ""}):
            status, text = CommandLine.run_main(CommandLine(), "--mode", "probe", "--no-fetch")
        self.assertEqual(status, guard.EXIT_CANNOT_RUN)
        self.assertIn("not in the pinned environment", text)

    def test_the_container_flag_does_not_combine_with_compare_only_or_unpinned(self) -> None:
        upstream, fork = str(FIXTURES / "upstream.json"), str(FIXTURES / "fork.json")
        for extra in (("--from-json", upstream, fork), ("--unpinned",)):
            with self.subTest(extra=extra):
                status, text = CommandLine.run_main(CommandLine(), "--container", *extra)
                self.assertEqual(status, guard.EXIT_CANNOT_RUN)
                self.assertIn("--container measures", text)


class Container(unittest.TestCase):
    def test_the_guard_inside_takes_the_same_arguments_without_container_or_fetch(self) -> None:
        cases = {
            ("--container",): ["--no-fetch"],
            ("--container", "--mode", "full"): ["--mode", "full", "--no-fetch"],
            ("--container", "img:tag", "--heap-check"): ["--heap-check", "--no-fetch"],
            ("--mode", "full", "--container=img:tag"): ["--mode", "full", "--no-fetch"],
            ("--no-fetch", "--container"): ["--no-fetch"],
        }
        for argv, inner in cases.items():
            with self.subTest(argv=argv):
                self.assertEqual(guard.inner_arguments(argv), inner)

    def test_the_docker_command_pins_image_network_user_and_paths(self) -> None:
        command = guard.container_command(
            "/usr/bin/docker", "img:tag", "sha256:abc", ["--no-fetch"], [Path("/repo")]
        )
        self.assertEqual(command[:3], ["/usr/bin/docker", "run", "--rm"])
        text = " ".join(command)
        for part in (
            "--pull never",
            "--network none",
            f"--env {guard.IMAGE_VARIABLE}=sha256:abc",
            "--volume /repo:/repo",
            f"--workdir {guard.REPO}",
        ):
            self.assertIn(part, text)
        self.assertEqual(command[-3:], ["img:tag", "scripts/dev/upstream_parity.py", "--no-fetch"])

    def test_a_worktree_inside_its_main_checkout_is_one_mount(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            main_checkout = Path(tmp).resolve()
            worktree = main_checkout / ".claude" / "worktrees" / "w"
            elsewhere = main_checkout.parent / "other"
            self.assertEqual(guard.mount_roots(worktree, main_checkout), [main_checkout])
            self.assertEqual(guard.mount_roots(worktree, elsewhere), [elsewhere, worktree])


def _heap(upstream: set[tuple[str, str]], fork: set[tuple[str, str]]) -> guard.HeapCheck:
    return guard.HeapCheck(guard.HEAP_FILL, frozenset(upstream), frozenset(fork))


class HeapContents(unittest.TestCase):
    def test_outputs_that_change_between_two_documents_are_found(self) -> None:
        first, _ = documents()
        second = copy.deepcopy(first)
        self.assertEqual(guard.unstable_outputs(first, second), frozenset())
        second["runs"][PSNR]["values"]["1|psnr_y"] = "31"
        second["runs"][PSNR]["pooled"]["mean|psnr_y"] = "30.5"
        second["runs"][CAMBI]["status"] = "crash"
        self.assertEqual(
            guard.unstable_outputs(first, second),
            {(PSNR, "1|psnr_y"), (PSNR, "pool-mean|psnr_y"), (CAMBI, "*")},
        )

    def test_a_stable_heap_check_passes_and_is_reported(self) -> None:
        status, lines = guard.judge(*documents(), fragments(), {}, heap=_heap(set(), set()))
        text = "\n".join(lines)
        self.assertEqual(status, guard.EXIT_PASS, text)
        self.assertIn("upstream outputs that depend on heap contents: 0 in 0 runs", text)

    def test_an_output_of_this_tree_that_depends_on_the_heap_fails(self) -> None:
        heap = _heap(set(), {(PSNR, "0|psnr_y")})
        status, lines = guard.judge(*documents(), fragments(), {}, heap=heap)
        self.assertEqual(status, guard.EXIT_DIFFERENT)
        self.assertIn("THIS TREE'S OUTPUT DEPENDS ON HEAP CONTENTS (1):", lines)

    def test_a_finite_bound_over_an_undefined_upstream_value_fails(self) -> None:
        heap = _heap({(CIEDE, "0|ciede2000")}, set())
        status, lines = guard.judge(*documents(), fragments(), {}, heap=heap)
        text = "\n".join(lines)
        self.assertEqual(status, guard.EXIT_DIFFERENT, text)
        self.assertIn("FINITE BOUND OVER AN UNDEFINED UPSTREAM VALUE", text)
        self.assertIn("ciede.chroma-422 has bound 0.2", text)

    def test_an_undefined_upstream_value_may_be_covered_without_a_bound(self) -> None:
        entries = tuple(
            fragment if fragment.name != "ciede.chroma-422" else _with_bound(fragment, math.inf)
            for fragment in fragments()
        )
        heap = _heap({(CIEDE, "0|ciede2000"), (CAMBI, "*")}, set())
        status, lines = guard.judge(*documents(), entries, {}, heap=heap)
        self.assertEqual(status, guard.EXIT_PASS, "\n".join(lines))

    def test_the_heap_fill_is_set_for_the_second_run_and_removed_for_the_first(self) -> None:
        caller = {"PATH": "/bin", "MALLOC_PERTURB_": "7"}
        self.assertEqual(guard.run_environment(0, caller), {"PATH": "/bin"})
        self.assertEqual(
            guard.run_environment(guard.HEAP_FILL, caller),
            {"PATH": "/bin", "MALLOC_PERTURB_": str(guard.HEAP_FILL)},
        )


class Matrix(unittest.TestCase):
    def test_run_names_are_unique_and_the_probe_set_is_part_of_the_full_matrix(self) -> None:
        full = matrix_data.matrix("full")
        probe = matrix_data.matrix("probe")
        keys = [(run.fixture, run.name) for run in full]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertLess(len(probe), len(full))
        self.assertLessEqual({(run.fixture, run.name) for run in probe}, set(keys))
        with self.assertRaises(ValueError):
            matrix_data.matrix("nightly")

    def test_every_run_names_a_known_fixture_and_a_known_request(self) -> None:
        names = {fixture.name for fixture in matrix_data.FIXTURES}
        for run in matrix_data.matrix("full"):
            self.assertIn(run.fixture, names)
            self.assertIn(run.spec[0], "FMCB")
            self.assertEqual(run.name[0], run.spec[0])

    def test_option_strings_become_file_safe_names(self) -> None:
        self.assertEqual(matrix_data.spec_id(""), "default")
        self.assertEqual(matrix_data.spec_id("a=1:b=2.5"), "a-1+b-2.5")

    def test_derived_fixtures_name_a_source_that_precedes_them(self) -> None:
        seen: set[str] = set()
        for fixture in matrix_data.FIXTURES:
            if fixture.source == matrix_data.DERIVED and fixture.recipe[0] != "noise":
                self.assertIn(fixture.recipe[1], seen, fixture.name)
            seen.add(fixture.name)


class FixtureAvailability(unittest.TestCase):
    """Required, optional and derived fixtures, against a scratch tree of files."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.netflix = self.root / "yuv"
        self.netflix.mkdir()
        self._saved = (guard.NETFLIX_DIR, guard.BBB_DIR)
        guard.NETFLIX_DIR, guard.BBB_DIR = self.netflix, self.root / "bbb"
        self.addCleanup(self._restore)

    def _restore(self) -> None:
        guard.NETFLIX_DIR, guard.BBB_DIR = self._saved

    def install(self, name: str) -> None:
        fixture = matrix_data.fixture_by_name(name)
        size = guard._frame_bytes(fixture.w, fixture.h, fixture.pix, fixture.bpc) * fixture.frames
        for path in guard.fixture_paths(fixture, self.root / "derived"):
            path.write_bytes(bytes(size))

    def test_a_missing_required_fixture_cannot_run(self) -> None:
        with self.assertRaisesRegex(guard.CannotRun, "fetch-test-yuvs.sh"):
            guard.available_fixtures({"nflx8"}, self.root / "derived")

    def test_optional_fixtures_are_skipped_with_a_reason_and_derived_ones_are_written(self) -> None:
        self.install("nflx8")
        wanted = {"nflx8", "nflx10", "bbb4k", "s8x8", "nflx444p12", "odd444"}
        usable, skipped = guard.available_fixtures(wanted, self.root / "derived")
        self.assertEqual(usable, ["nflx8", "odd444", "s8x8"])
        self.assertEqual(set(skipped), {"nflx10", "bbb4k", "nflx444p12"})
        self.assertIn("its source nflx12 is not installed", skipped["nflx444p12"])
        crop = matrix_data.fixture_by_name("s8x8")
        self.assertEqual(
            guard.fixture_paths(crop, self.root / "derived")[0].stat().st_size, (64 + 2 * 16) * 2
        )


class DerivedFixtures(unittest.TestCase):
    """The generators on hand-made frames."""

    SOURCE = matrix_data.Fixture("src", matrix_data.DERIVED, "r", "d", 4, 4, "420", 8, 1)

    def frame(self) -> bytes:
        luma = bytes(range(16))
        return luma + bytes([100, 101, 102, 103]) + bytes([200, 201, 202, 203])

    def test_plane_sizes_round_subsampled_chroma_up(self) -> None:
        self.assertEqual(guard._plane_sizes(19, 19, "420"), ((19, 19), (10, 10), (10, 10)))
        self.assertEqual(guard._plane_sizes(19, 19, "422"), ((19, 19), (10, 19), (10, 19)))
        self.assertEqual(guard._plane_sizes(19, 19, "444"), ((19, 19), (19, 19), (19, 19)))
        self.assertEqual(guard._plane_sizes(19, 19, "400"), ((19, 19),))
        self.assertEqual(guard._frame_bytes(576, 324, "420", 10), 576 * 324 * 3)

    def test_a_crop_takes_the_top_left_of_every_plane(self) -> None:
        target = matrix_data.Fixture("crop", matrix_data.DERIVED, "r", "d", 3, 2, "420", 8, 1)
        cropped = guard._crop_frame(self.frame(), self.SOURCE, target)
        self.assertEqual(cropped, bytes([0, 1, 2, 4, 5, 6]) + bytes([100, 101]) + bytes([200, 201]))

    def test_chroma_is_repeated_to_the_luma_size(self) -> None:
        full = guard._to444_frame(self.frame(), self.SOURCE)
        self.assertEqual(len(full), 48)
        self.assertEqual(full[:16], bytes(range(16)))
        self.assertEqual(full[16:24], bytes([100, 100, 101, 101, 100, 100, 101, 101]))
        self.assertEqual(full[40:48], bytes([202, 202, 203, 203, 202, 202, 203, 203]))

    def test_chroma_rows_are_repeated_for_422(self) -> None:
        half = guard._to422_frame(self.frame(), self.SOURCE)
        self.assertEqual(len(half), 16 + 2 * 8)
        self.assertEqual(half[16:24], bytes([100, 101, 100, 101, 102, 103, 102, 103]))
        self.assertEqual(half[24:32], bytes([200, 201, 200, 201, 202, 203, 202, 203]))

    def test_samples_are_widened_little_endian(self) -> None:
        self.assertEqual(guard._widen_frame(bytes([0, 1, 255]), 16), bytes([0, 0, 0, 1, 0, 255]))
        self.assertEqual(guard._widen_frame(bytes([0, 1, 255]), 10), bytes([0, 0, 4, 0, 0xFC, 3]))

    def test_sixteen_bit_samples_stay_whole_when_columns_are_doubled(self) -> None:
        self.assertEqual(
            guard._double_columns(bytes([1, 2, 3, 4]), 2), bytes([1, 2, 1, 2, 3, 4, 3, 4])
        )

    def test_noise_is_the_same_on_every_run_and_stays_in_range(self) -> None:
        fixture = matrix_data.fixture_by_name("noise10")
        first = guard._noise_frame(fixture, "ref", 0)
        self.assertEqual(first, guard._noise_frame(fixture, "ref", 0))
        self.assertNotEqual(first, guard._noise_frame(fixture, "dis", 0))
        self.assertEqual(len(first), guard._frame_bytes(576, 324, "420", 10))
        self.assertLess(max(first[1::2]), 4)


if __name__ == "__main__":
    unittest.main()
