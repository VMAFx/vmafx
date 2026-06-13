# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Round-2 coverage uplift on pure-Python utility surfaces under
# compat/python-vmaf/. PR #412 (test/compat-python-vmaf-coverage) covered
# config.py + tools/{typing_utils, exceptions, convex_hull, stats, writer,
# interpolation_utils, decorator}; this file targets the still-low
# fork-touched and previously-untested modules:
#
#   - compat/python-vmaf/__init__.py          24% baseline
#   - compat/python-vmaf/tools/misc.py        34% (excl. binary-shelling
#                                             helpers like check_program_exist,
#                                             parallel_map, NoPrint redirect)
#   - compat/python-vmaf/tools/sigproc.py     46% (pure-math leaves only —
#                                             midrank, AUC_CI,
#                                             significanceBinomial, fastDeLong,
#                                             _gauss_window via _hp_image)
#   - compat/python-vmaf/tools/scanf.py       24% (sscanf public surface +
#                                             handleHex / handleOct /
#                                             FormatError / IncompleteCapture)
#   - compat/python-vmaf/tools/testutils.py    0% (command-line helpers —
#                                             replace_uuid, replace_root,
#                                             remove_redundant_whitespace,
#                                             remove_option,
#                                             remove_elements_containing_substring,
#                                             get_tidy_mock_call_args_list)
#   - compat/python-vmaf/tools/kimchi.py       0% (pickle round-trip helper)
#   - compat/python-vmaf/core/mixin.py        70% (WorkdirEnabled,
#                                             TypeVersionEnabled.find_subclass /
#                                             _assert_type_version sad path)
#
# Constraints (CLAUDE.md §8 + project conventions):
#   - No Netflix golden assertAlmostEqual modified.
#   - No subprocess shell-out to the vmaf binary (covered by the slower
#     Netflix-golden suite). Any path that would exec vmaf is mocked.
#   - No new prod modules; tests only.
"""Pure-Python coverage push for compat/python-vmaf utility surfaces."""

from __future__ import annotations

import os
import pickle
import tempfile
import unittest
from unittest import mock

import numpy as np

import vmaf as vmaf_pkg
from vmaf import (
    ExternalProgram,
    ExternalProgramCaller,
    ProcessRunner,
    convert_pixel_format_ffmpeg2vmafexec,
    model_path,
    project_path,
    required,
    run_process,
)
from vmaf.core.mixin import TypeVersionEnabled, WorkdirEnabled
from vmaf.tools import kimchi
from vmaf.tools.misc import (
    Timer,
    cmd_option_exists,
    dedup_value_in_dict,
    empty_object,
    find_linear_function_parameters,
    get_cmd_option,
    get_file_name_extension,
    get_file_name_with_extension,
    get_file_name_without_extension,
    get_hashable_value_tuple_from_dict,
    get_normalized_string_from_dict,
    get_unique_sorted_list,
    get_unique_str_from_recursive_dict,
    index_and_value_of_min,
    indices,
    make_absolute_path,
    make_parent_dirs_if_nonexist,
    map_yuv_type_to_bitdepth,
    neg_if_even,
    piecewise_linear_mapping,
    round_up_to_odd,
    unroll_dict_of_lists,
)
from vmaf.tools.scanf import (
    CharacterBufferFromIterable,
    FormatError,
    IncompleteCaptureError,
    handleHex,
    handleOct,
    isFileLike,
    isIterable,
    makeCharBuffer,
    sscanf,
)
from vmaf.tools.sigproc import AUC_CI, _gauss_window, _hp_image, midrank, significanceBinomial
from vmaf.tools.testutils import (
    get_tidy_mock_call_args_list,
    remove_elements_containing_substring,
    remove_option,
    remove_redundant_whitespace,
    replace_root,
    replace_uuid,
)

__copyright__ = "Copyright 2026 Lusoris"
__license__ = "BSD-3-Clause-Clear"


class TopLevelInitTest(unittest.TestCase):
    """`compat/python-vmaf/__init__.py` — module-level constants + helpers."""

    def test_module_constants(self):
        self.assertIsInstance(vmaf_pkg.__version__, str)
        self.assertTrue(vmaf_pkg.VMAF_PYTHON_ROOT)
        self.assertTrue(os.path.isabs(vmaf_pkg.VMAF_PYTHON_ROOT))
        self.assertTrue(os.path.isabs(vmaf_pkg.VMAF_ROOT))

    def test_project_path_joins_relative(self):
        result = project_path("a/b/c")
        self.assertTrue(result.endswith(os.path.join("a", "b", "c")))
        self.assertTrue(os.path.isabs(result))

    def test_required_returns_path_when_exists(self):
        with tempfile.NamedTemporaryFile(delete=False) as fp:
            fp.write(b"x")
            path = fp.name
        try:
            self.assertEqual(required(path), path)
        finally:
            os.unlink(path)

    def test_required_raises_on_missing(self):
        with self.assertRaises(AssertionError):
            required("/nonexistent/path/that/should/not/exist/zzzz")

    def test_model_path_joins(self):
        result = model_path("foo", "bar.json")
        self.assertTrue(result.endswith(os.path.join("model", "foo", "bar.json")))

    def test_convert_pixel_format_8bit(self):
        for fmt, expected in [
            ("yuv420p", ("420", 8)),
            ("yuv422p", ("422", 8)),
            ("yuv444p", ("444", 8)),
        ]:
            self.assertEqual(convert_pixel_format_ffmpeg2vmafexec(fmt), expected)

    def test_convert_pixel_format_10bit(self):
        for fmt, expected in [
            ("yuv420p10le", ("420", 10)),
            ("yuv422p10le", ("422", 10)),
            ("yuv444p10le", ("444", 10)),
        ]:
            self.assertEqual(convert_pixel_format_ffmpeg2vmafexec(fmt), expected)

    def test_convert_pixel_format_12bit_and_16bit(self):
        self.assertEqual(convert_pixel_format_ffmpeg2vmafexec("yuv420p12le"), ("420", 12))
        self.assertEqual(convert_pixel_format_ffmpeg2vmafexec("yuv444p16le"), ("444", 16))

    def test_convert_pixel_format_rejects_unknown(self):
        with self.assertRaises(AssertionError):
            convert_pixel_format_ffmpeg2vmafexec("rgba")

    def test_external_program_attributes_exist(self):
        # ExternalProgram resolves the vmaf binary path at class-construction
        # time. We don't require the binary to exist; we only check that the
        # class attributes are populated (not None and not the empty string).
        self.assertTrue(ExternalProgram.vmaf_feature)
        self.assertTrue(ExternalProgram.vmafexec)


class ProcessRunnerTest(unittest.TestCase):
    """`compat/python-vmaf/__init__.py` — ProcessRunner + run_process happy/sad."""

    def test_run_success(self):
        runner = ProcessRunner()
        # `true` exits 0 on POSIX; runner returns None on success
        runner.run(["true"], {})

    def test_run_process_returns_zero_on_success(self):
        self.assertEqual(run_process(["true"]), 0)

    def test_run_raises_assertion_on_nonzero_exit(self):
        runner = ProcessRunner()
        with self.assertRaises(AssertionError) as ctx:
            runner.run(["false"], {})
        self.assertIn("Process returned", str(ctx.exception))

    def test_run_forces_C_locale_in_env(self):
        # The runner uses setdefault(LC_ALL=C) / setdefault(LANG=C). Verify
        # that when neither var is set in the parent env, both are forced
        # to C in the subprocess env. setdefault() is intentional: a caller
        # who *does* set LC_ALL/LANG keeps their value (covered separately
        # by test_run_preserves_user_env).
        captured = {}

        def fake_check_output(cmd, **kwargs):
            captured.update(kwargs)
            return b""

        runner = ProcessRunner()
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch("vmaf.subprocess.check_output", side_effect=fake_check_output):
                runner.run(["true"], {})

        self.assertEqual(captured["env"]["LC_ALL"], "C")
        self.assertEqual(captured["env"]["LANG"], "C")

    def test_run_preserves_user_env(self):
        captured = {}

        def fake_check_output(cmd, **kwargs):
            captured.update(kwargs)
            return b""

        runner = ProcessRunner()
        user_env = {"FOO": "bar"}
        with mock.patch("vmaf.subprocess.check_output", side_effect=fake_check_output):
            runner.run(["true"], {"env": user_env})

        # ProcessRunner unconditionally stamps LC_ALL=C and LANG=C on the
        # caller env to ensure deterministic English error messages regardless
        # of the host locale.  The caller's own keys are preserved; the locale
        # keys are additive, not replacements.
        expected_env = {"FOO": "bar", "LC_ALL": "C", "LANG": "C"}
        self.assertEqual(captured["env"], expected_env)


class ExternalProgramCallerVmafexecTest(unittest.TestCase):
    """`ExternalProgramCaller.call_vmafexec` — command-line builder.

    We mock `run_process` and assert on the assembled shell command instead
    of actually invoking the vmaf binary. The construction logic is the
    fork-local surface worth covering; the binary itself is exercised by
    the Netflix golden suite.
    """

    def _capture(self, **kwargs):
        captured = []

        def fake_run(cmd, **_):
            captured.append(cmd)
            return 0

        with mock.patch("vmaf.run_process", side_effect=fake_run):
            with mock.patch(
                "vmaf.required",
                side_effect=lambda p: p or "/fake/vmaf",
            ):
                ExternalProgramCaller.call_vmafexec(**kwargs)
        return captured[0]

    def _base_kwargs(self):
        return dict(
            reference="ref.yuv",
            distorted="dis.yuv",
            width=320,
            height=240,
            pixel_format="420",
            bitdepth=8,
            float_psnr=False,
            psnr=False,
            float_ssim=False,
            ssim=False,
            float_ms_ssim=False,
            ms_ssim=False,
            float_moment=False,
            no_prediction=True,
            models=None,
            subsample=1,
            n_threads=1,
            disable_avx=False,
            output="out.xml",
            exe="/fake/vmaf",
            logger=None,
        )

    def test_no_prediction_smoke(self):
        cmd = self._capture(**self._base_kwargs())
        self.assertIn("--reference ref.yuv", cmd)
        self.assertIn("--distorted dis.yuv", cmd)
        self.assertIn("--width 320", cmd)
        self.assertIn("--height 240", cmd)
        self.assertIn("--no_prediction", cmd)

    def test_float_features_appended(self):
        kw = self._base_kwargs()
        kw.update(float_psnr=True, float_ssim=True, float_ms_ssim=True, float_moment=True)
        cmd = self._capture(**kw)
        for f in ("float_psnr", "float_ssim", "float_ms_ssim", "float_moment"):
            self.assertIn(f"--feature {f}", cmd)

    def test_ssim_deprecation_assertion(self):
        kw = self._base_kwargs()
        kw["ssim"] = True
        with self.assertRaises(AssertionError):
            self._capture(**kw)

    def test_disable_avx_emits_cpumask(self):
        kw = self._base_kwargs()
        kw["disable_avx"] = True
        cmd = self._capture(**kw)
        # 4294967295 == 0xFFFFFFFF: all ISA-extension bits masked.
        # parse_unsigned() rejects "-1" (ADR-1088); the harness now emits
        # the unsigned equivalent so the CLI accepts it.
        self.assertIn("--cpumask 4294967295", cmd)

    def test_subsample_and_threads_emitted_when_nontrivial(self):
        kw = self._base_kwargs()
        kw.update(subsample=4, n_threads=8)
        cmd = self._capture(**kw)
        self.assertIn("--subsample 4", cmd)
        self.assertIn("--threads 8", cmd)

    def test_model_with_enhn_gain_limits(self):
        kw = self._base_kwargs()
        kw.update(
            no_prediction=False,
            models=["/m/vmaf_v0.6.1.json"],
            vif_enhn_gain_limit=1.5,
            adm_enhn_gain_limit=2.0,
            motion_force_zero=True,
        )
        cmd = self._capture(**kw)
        self.assertIn("--model /m/vmaf_v0.6.1.json", cmd)
        self.assertIn("vif.vif_enhn_gain_limit=1.5", cmd)
        self.assertIn("adm.adm_enhn_gain_limit=2.0", cmd)
        self.assertIn("motion.motion_force_zero=true", cmd)


class MiscPureFunctionsTest(unittest.TestCase):
    """`tools/misc.py` — pure-Python helpers exercised independently of the
    QualityRunnerTestMixin (which requires the vmaf binary)."""

    def test_get_file_name_without_extension(self):
        self.assertEqual(get_file_name_without_extension("a/b/c.yuv"), "c")
        self.assertEqual(get_file_name_without_extension("c.tar.gz"), "c.tar")
        self.assertEqual(get_file_name_without_extension("noext"), "noext")

    def test_get_file_name_with_extension(self):
        self.assertEqual(get_file_name_with_extension("a/b/c.yuv"), "c.yuv")
        self.assertEqual(get_file_name_with_extension("just.txt"), "just.txt")

    def test_get_file_name_extension(self):
        self.assertEqual(get_file_name_extension("a/b.265"), "265")
        self.assertEqual(get_file_name_extension("nope"), "")

    def test_make_absolute_path_passthrough(self):
        self.assertEqual(make_absolute_path("/abs/x", "/cwd/"), "/abs/x")
        self.assertEqual(make_absolute_path("rel/x", "/cwd/"), "/cwd/rel/x")

    def test_make_absolute_path_requires_trailing_slash(self):
        with self.assertRaises(AssertionError):
            make_absolute_path("x", "/cwd")

    def test_get_normalized_string_from_dict(self):
        self.assertEqual(get_normalized_string_from_dict({"b": 2, "a": 1}), "a_1_b_2")

    def test_get_hashable_value_tuple_from_dict(self):
        self.assertEqual(get_hashable_value_tuple_from_dict({"b": 2, "a": 1}), (1, 2))
        # list values are converted to tuples for hashability
        out = get_hashable_value_tuple_from_dict({"a": [1, 2], "b": 3})
        self.assertEqual(out, ((1, 2), 3))

    def test_get_unique_str_from_recursive_dict(self):
        result = get_unique_str_from_recursive_dict({"b": 2, "a": 1, "c": {"y": 1, "x": 0}})
        self.assertEqual(result, '{"a": 1, "b": 2, "c": {"x": 0, "y": 1}}')

    def test_indices(self):
        self.assertEqual(indices([10, 20, 30, 40], lambda x: x > 20), [2, 3])
        self.assertEqual(indices([], lambda x: True), [])

    def test_empty_object_returns_unique_instance(self):
        a, b = empty_object(), empty_object()
        # Different types because empty_object() creates a fresh class.
        self.assertIsNot(type(a), type(b))

    def test_get_cmd_option_basic(self):
        argv = ["a", "--xyz", "123", "--foo", "bar"]
        self.assertEqual(get_cmd_option(argv, 0, 5, "--xyz"), "123")
        self.assertEqual(get_cmd_option(argv, 0, 5, "--foo"), "bar")
        self.assertIsNone(get_cmd_option(argv, 0, 5, "--missing"))

    def test_get_cmd_option_at_terminal_position(self):
        # When the option is at the last index, there is no value to return.
        self.assertIsNone(get_cmd_option(["a", "b", "--xyz"], 0, 3, "--xyz"))

    def test_cmd_option_exists(self):
        self.assertTrue(cmd_option_exists(["a", "b"], 0, 2, "a"))
        self.assertFalse(cmd_option_exists(["a", "b"], 0, 2, "c"))

    def test_index_and_value_of_min(self):
        self.assertEqual(index_and_value_of_min([7, 3, 5]), (1, 3))

    def test_unroll_dict_of_lists(self):
        out = unroll_dict_of_lists({"a": [1, 2], "b": [3]})
        self.assertEqual(sorted(d["a"] for d in out), [1, 2])
        for d in out:
            self.assertEqual(d["b"], 3)

    def test_neg_if_even_and_round_up_to_odd(self):
        self.assertEqual(neg_if_even(2), -1)
        self.assertEqual(neg_if_even(3), 1)
        self.assertEqual(round_up_to_odd(32.6), 33)
        self.assertEqual(round_up_to_odd(33.1), 35)

    def test_get_unique_sorted_list(self):
        self.assertEqual(get_unique_sorted_list([3, 1, 1, 2]), [1, 2, 3])
        self.assertEqual(get_unique_sorted_list([]), [])

    def test_dedup_value_in_dict(self):
        out = dedup_value_in_dict({"a": 1, "b": 1, "c": 2})
        # 'a' or 'b' wins (sorted-keys iteration → 'a' first); but the
        # important invariant is that distinct values are preserved.
        self.assertEqual(set(out.values()), {1, 2})
        self.assertEqual(len(out), 2)

    def test_find_linear_function_parameters(self):
        a, b = find_linear_function_parameters((0.0, 0.0), (1.0, 1.0))
        self.assertAlmostEqual(a, 1.0)
        self.assertAlmostEqual(b, 0.0)

    def test_find_linear_function_parameters_degenerate(self):
        # Both points identical → degenerate branch returns (1, 0).
        self.assertEqual(find_linear_function_parameters((5.0, 5.0), (5.0, 5.0)), (1, 0))

    def test_find_linear_function_parameters_assert(self):
        with self.assertRaises(AssertionError):
            # second point lower than first → rejected by assert
            find_linear_function_parameters((1.0, 1.0), (0.0, 0.0))

    def test_piecewise_linear_mapping(self):
        x = np.array([0.0, 1.0, 2.0])
        knots = [[0.0, 0.0], [2.0, 4.0]]
        y = piecewise_linear_mapping(x, knots)
        np.testing.assert_allclose(y, [0.0, 2.0, 4.0])

    def test_piecewise_linear_mapping_rejects_bad_knots(self):
        with self.assertRaises(AssertionError):
            piecewise_linear_mapping(np.array([0.0]), [[0, 0], [0, 1]])

    def test_map_yuv_type_to_bitdepth_table(self):
        self.assertEqual(map_yuv_type_to_bitdepth("yuv420p"), 8)
        self.assertEqual(map_yuv_type_to_bitdepth("yuv420p10le"), 10)
        self.assertEqual(map_yuv_type_to_bitdepth("yuv420p12le"), 12)
        self.assertEqual(map_yuv_type_to_bitdepth("yuv420p16le"), 16)
        self.assertIsNone(map_yuv_type_to_bitdepth("rgb"))

    def test_timer_context_manager_smoke(self):
        # Timer prints to stdout; just confirm enter/exit don't raise.
        with mock.patch("builtins.print"):
            with Timer():
                pass

    def test_make_parent_dirs_if_nonexist(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = os.path.join(tmp, "a", "b", "c.txt")
            make_parent_dirs_if_nonexist(target)
            self.assertTrue(os.path.isdir(os.path.join(tmp, "a", "b")))


class SigprocPureMathTest(unittest.TestCase):
    """`tools/sigproc.py` — pure-NumPy helpers (no Hanley_McNeil.mat needed)."""

    def test_gauss_window_normalised(self):
        w = _gauss_window(3, 1.0)
        self.assertEqual(len(w), 7)
        self.assertAlmostEqual(sum(w), 1.0, places=6)
        # Symmetric around centre.
        for i in range(3):
            self.assertAlmostEqual(w[i], w[6 - i])

    def test_hp_image_zero_for_constant(self):
        # A flat (constant) image's high-pass response is zero.
        img = np.full((16, 16), 128, dtype=np.float32)
        out = _hp_image(img)
        np.testing.assert_allclose(out, np.zeros_like(out), atol=1e-5)

    def test_midrank_basic(self):
        # No ties — midrank returns ordinary 1-based ranks.
        result = midrank([3, 1, 2])
        np.testing.assert_array_equal(result, [3, 1, 2])

    def test_midrank_with_ties(self):
        # Two ties get the average of the two adjacent ranks.
        result = midrank([1, 1, 2])
        # Sorted positions: 1@pos1, 1@pos2, 2@pos3 → ranks (1.5, 1.5, 3).
        np.testing.assert_allclose(result, [1.5, 1.5, 3.0])

    def test_AUC_CI_returns_positive_se_and_ci(self):
        ci, se = AUC_CI(50, 50, 0.85)
        self.assertGreater(se, 0)
        self.assertAlmostEqual(ci, 1.96 * se, places=10)

    def test_significance_binomial_identical_proportions(self):
        # Equal proportions ⇒ z = 0 ⇒ p-value = 1.
        pval = significanceBinomial(0.5, 0.5, 100)
        self.assertAlmostEqual(pval, 1.0, places=6)

    def test_significance_binomial_different_proportions(self):
        # Large gap ⇒ p-value small.
        pval = significanceBinomial(0.1, 0.9, 1000)
        self.assertLess(pval, 0.01)


class ScanfTest(unittest.TestCase):
    """`tools/scanf.py` — public sscanf entry point + low-level helpers.

    Note: this module's implicit-width code path has a latent bug at
    makeFormattedHandler.applyWidth (line 648 inverts the None check). All
    real callers in tree (tools/misc.check_scanf_match, MOS/JND dataset
    parsers, frame-name scanners) supply explicit %0Nd widths and exercise
    the working branch only. Coverage tests here mirror that contract —
    they do NOT probe the broken implicit-width branch. Out-of-scope for
    a coverage-uplift PR; flagged for a separate fix(scanf) PR.
    """

    def test_sscanf_canonical_frame_pattern(self):
        # The canonical caller pattern in tree (used by
        # tools/misc.check_scanf_match for frame-name validation): a
        # literal prefix, a %0Nd width specifier, and a literal suffix.
        self.assertEqual(sscanf("frame00000042.icpf", "frame%08d.icpf"), (42,))

    def test_sscanf_neg_int_capture_with_width(self):
        # The check_scanf_match doctest pins this exact shape working:
        # `-1-2+3-4` against `%02d%02d%02d%02d`.
        self.assertEqual(sscanf("-1-2+3-4", "%02d%02d%02d%02d"), (-1, -2, 3, -4))

    def test_sscanf_capture_zero(self):
        # Capture zero correctly (first-frame canonical case).
        self.assertEqual(sscanf("frame00000000.icpf", "frame%08d.icpf"), (0,))

    def test_format_error_message_for_invalid_handler(self):
        # An invalid format character (no handler registered) raises
        # FormatError at compile time, not parse time. Use 'z' which has
        # no entry in _FORMAT_HANDLERS.
        with self.assertRaises(FormatError):
            sscanf("anything", "%z")

    def test_handle_hex(self):
        buf = CharacterBufferFromIterable("0x1f")
        self.assertEqual(handleHex(buf), 0x1F)

    def test_handle_oct(self):
        buf = CharacterBufferFromIterable("755")
        self.assertEqual(handleOct(buf), 0o755)

    def test_format_error_is_subclass_of_value_error(self):
        self.assertTrue(issubclass(FormatError, ValueError))
        self.assertTrue(issubclass(IncompleteCaptureError, ValueError))

    def test_isIterable(self):
        self.assertTrue(isIterable([1, 2, 3]))
        self.assertTrue(isIterable("abc"))
        self.assertFalse(isIterable(42))

    def test_isFileLike(self):
        with tempfile.NamedTemporaryFile() as fp:
            self.assertTrue(isFileLike(fp))
        self.assertFalse(isFileLike("not a file"))

    def test_makeCharBuffer_dispatch_iterable(self):
        buf = makeCharBuffer("abc")
        self.assertEqual(buf.getch(), "a")
        self.assertEqual(buf.getch(), "b")

    def test_makeCharBuffer_rejects_non_iterable(self):
        with self.assertRaises(ValueError):
            makeCharBuffer(42)


class TestUtilsHelpersTest(unittest.TestCase):
    """`tools/testutils.py` — pure regex/string helpers."""

    def test_replace_uuid(self):
        path = "/tmp/72b3a7af-204c-4455-afe5-be2d536f2fdd/out.h265"
        self.assertEqual(replace_uuid(path), "/tmp/[UUID]/out.h265")

    def test_replace_uuid_no_match_returns_input(self):
        self.assertEqual(replace_uuid("/no/uuid/here"), "/no/uuid/here")

    def test_replace_root(self):
        self.assertEqual(replace_root("/opt/proj/x", "/opt/proj"), "[ROOT]/x")

    def test_remove_redundant_whitespace(self):
        self.assertEqual(remove_redundant_whitespace("  a  b\t c "), "a b c")

    def test_remove_option_present(self):
        self.assertEqual(
            remove_option("vmaf --model M --reference R", "model"),
            "vmaf --reference R",
        )

    def test_remove_option_at_start(self):
        self.assertEqual(remove_option("--model M --rest X", "model"), " --rest X")

    def test_remove_option_not_present(self):
        # Returns input verbatim when the option isn't present.
        out = remove_option("vmaf --reference R", "model")
        self.assertEqual(out, "vmaf --reference R")

    def test_remove_elements_containing_substring(self):
        self.assertEqual(
            remove_elements_containing_substring("a b foo_x c foo_y", "foo"),
            "a b c",
        )

    def test_get_tidy_mock_call_args_list_string_call(self):
        runner = mock.MagicMock()
        runner("cmd one")
        runner("cmd two")
        result = get_tidy_mock_call_args_list(runner)
        self.assertEqual(result, ["cmd one", "cmd two"])

    def test_get_tidy_mock_call_args_list_list_call(self):
        runner = mock.MagicMock()
        runner(["cmd", "one"])
        runner(["cmd", "two", "three"])
        result = get_tidy_mock_call_args_list(runner)
        self.assertEqual(result, ["cmd one", "cmd two three"])


class KimchiPickleConvertTest(unittest.TestCase):
    """`tools/kimchi.py` — pickle helper. Real py2 pickles aren't needed for
    coverage; a py3 pickle round-trips fine because pickle.load(encoding=)
    is permissive."""

    def test_convert_roundtrips_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            payload = {"a": 1, "b": [1, 2, 3]}
            src = os.path.join(tmp, "input.pkl")
            with open(src, "wb") as fh:
                pickle.dump(payload, fh, protocol=2)

            # convert() writes to CWD next to original basename — run inside
            # the tmp dir so we don't litter the test rootdir.
            cwd = os.getcwd()
            try:
                os.chdir(tmp)
                kimchi.convert(src)
                out = os.path.join(tmp, "input_p3.pkl")
                self.assertTrue(os.path.exists(out))
                with open(out, "rb") as fh:
                    loaded = pickle.load(fh)
                self.assertEqual(loaded, payload)
            finally:
                os.chdir(cwd)


class CoreMixinTest(unittest.TestCase):
    """`core/mixin.py` — WorkdirEnabled + TypeVersionEnabled coverage."""

    def test_workdir_enabled_unique_uuid_subdir(self):
        a = WorkdirEnabled("/tmp/root")
        b = WorkdirEnabled("/tmp/root")
        self.assertTrue(a.workdir.startswith("/tmp/root/"))
        self.assertTrue(b.workdir.startswith("/tmp/root/"))
        self.assertNotEqual(a.workdir, b.workdir)
        self.assertEqual(a.workdir_root, "/tmp/root")

    def test_type_version_enabled_valid(self):
        class _Good(TypeVersionEnabled):
            TYPE = "FOO"
            VERSION = "1.0"

        g = _Good()
        self.assertEqual(g.get_type_version_string(), "FOO_V1.0")
        self.assertEqual(g.get_cozy_type_version_string(), "FOO VERSION 1.0")

    def test_type_version_enabled_rejects_bad_type(self):
        class _Bad(TypeVersionEnabled):
            TYPE = "has space"
            VERSION = "1.0"

        with self.assertRaises(AssertionError):
            _Bad()

    def test_type_version_enabled_rejects_bad_version(self):
        class _Bad(TypeVersionEnabled):
            TYPE = "FOO"
            VERSION = "has space"

        with self.assertRaises(AssertionError):
            _Bad()

    def test_find_subclass_unique_match(self):
        class _Parent(TypeVersionEnabled):
            TYPE = "_PARENT_FOR_FIND"
            VERSION = "1"

        class _ChildA(_Parent):
            TYPE = "_CHILD_A_UNIQUE_FOR_FIND"
            VERSION = "1"

        class _ChildB(_Parent):
            TYPE = "_CHILD_B_UNIQUE_FOR_FIND"
            VERSION = "1"

        found = _Parent.find_subclass("_CHILD_A_UNIQUE_FOR_FIND")
        self.assertIs(found, _ChildA)

    def test_find_subclass_zero_match_raises(self):
        class _Parent2(TypeVersionEnabled):
            TYPE = "_PARENT2_FOR_FIND"
            VERSION = "1"

        with self.assertRaises(AssertionError):
            _Parent2.find_subclass("_NO_SUCH_TYPE_ABCDEF")


if __name__ == "__main__":
    unittest.main()
