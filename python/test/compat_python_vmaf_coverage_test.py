# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Focused unit tests lifting coverage on fork-touched and previously-untested
# pure-Python modules under compat/python-vmaf/. Targets the easy wins where
# upstream Netflix code shipped without test exercise — small leaf utilities
# (typing_utils, exceptions, convex_hull, stats, writer, interpolation_utils,
# decorator) plus fork-local plumbing in config.py (download_reactively
# happy + error paths, VmafConfig.workspace_path / resource_path).
#
# Strictly off-limits: anything that would shell out to the vmaf binary
# (covered by the slower Netflix-golden suite) and the Netflix golden-data
# assertAlmostEqual values (CLAUDE.md §8). No score thresholds touched.
"""Coverage uplift for compat/python-vmaf leaf modules and fork-local config."""

from __future__ import annotations

import os
import tempfile
import unittest
import urllib.error
import warnings
from unittest import mock

import numpy as np

from vmaf import config as vmaf_config
from vmaf.config import VmafConfig, VmafExternalConfig, download_reactively
from vmaf.tools.convex_hull import calculate_convex_hull, cross
from vmaf.tools.decorator import change_repr, deprecated, dummy, memoized, override
from vmaf.tools.exceptions import (
    BdRateException,
    BdRateNonMonotonicException,
    BdRateNoOverlapException,
    BdRateNotEnoughPointsException,
    BdRateZeroRateException,
    CalibrationError,
    EnsembleNotSupportedError,
    MissingLabelStddevError,
)
from vmaf.tools.interpolation_utils import InterpolationUtils
from vmaf.tools.stats import ListStats
from vmaf.tools.typing_utils import RdPoint
from vmaf.tools.writer import YuvWriter

__copyright__ = "Copyright 2026 Lusoris"
__license__ = "BSD-3-Clause-Clear"


class TypingUtilsTest(unittest.TestCase):
    """`tools/typing_utils.py` — single dataclass with a custom __hash__."""

    def test_rdpoint_attrs(self):
        p = RdPoint(rate=1.5, metric=42.0)
        self.assertEqual(p.rate, 1.5)
        self.assertEqual(p.metric, 42.0)

    def test_rdpoint_is_hashable_by_coords(self):
        a = RdPoint(rate=1.0, metric=2.0)
        b = RdPoint(rate=1.0, metric=2.0)
        c = RdPoint(rate=1.0, metric=3.0)
        # Same coords -> same hash (and usable in sets / dict keys).
        self.assertEqual(hash(a), hash(b))
        self.assertNotEqual(hash(a), hash(c))
        self.assertEqual(len({a, b, c}), 2)


class ExceptionsTest(unittest.TestCase):
    """`tools/exceptions.py` — all exception classes must instantiate + carry msg."""

    def test_calibration_error_is_exception(self):
        with self.assertRaises(CalibrationError):
            raise CalibrationError("boom")

    def test_missing_label_stddev_error(self):
        with self.assertRaises(MissingLabelStddevError):
            raise MissingLabelStddevError()

    def test_ensemble_not_supported_error(self):
        with self.assertRaises(EnsembleNotSupportedError):
            raise EnsembleNotSupportedError()

    def test_bdrate_exception_hierarchy(self):
        # All five BD-rate exceptions must subclass BdRateException
        # so callers can catch the family with a single except clause.
        for exc_cls in (
            BdRateNotEnoughPointsException,
            BdRateNoOverlapException,
            BdRateNonMonotonicException,
            BdRateZeroRateException,
        ):
            self.assertTrue(issubclass(exc_cls, BdRateException))

    def test_bdrate_default_messages(self):
        # Each subclass has a default message that names the failure mode.
        self.assertIn("at least 4 points", str(BdRateNotEnoughPointsException()))
        self.assertIn("No overlap", str(BdRateNoOverlapException()))
        self.assertIn("Non-monotonic", str(BdRateNonMonotonicException()))
        self.assertIn("zero rate", str(BdRateZeroRateException()))

    def test_bdrate_custom_message(self):
        msg = "ad-hoc test message"
        self.assertEqual(str(BdRateNotEnoughPointsException(msg)), msg)


class ConvexHullTest(unittest.TestCase):
    """`tools/convex_hull.py` — Andrew's monotone chain (lower hull only)."""

    def test_cross_product_sign(self):
        o = RdPoint(0.0, 0.0)
        a = RdPoint(1.0, 0.0)
        b_ccw = RdPoint(1.0, 1.0)
        b_cw = RdPoint(1.0, -1.0)
        self.assertGreater(cross(o, a, b_ccw), 0)
        self.assertLess(cross(o, a, b_cw), 0)

    def test_empty_input_returns_empty(self):
        self.assertEqual(calculate_convex_hull([]), [])

    def test_single_point(self):
        p = RdPoint(rate=1.0, metric=2.0)
        self.assertEqual(calculate_convex_hull([p]), [p])

    def test_duplicate_single_point_dedup(self):
        # Duplicates collapse to one unique point and short-circuit.
        p = RdPoint(rate=1.0, metric=2.0)
        self.assertEqual(calculate_convex_hull([p, p, p]), [p])

    def test_nonmonotonic_points_dropped(self):
        # A point with higher rate but lower metric than a predecessor
        # is non-monotonic and must be discarded before hull construction.
        good = [
            RdPoint(rate=1.0, metric=10.0),
            RdPoint(rate=2.0, metric=20.0),
        ]
        bad = RdPoint(rate=1.5, metric=5.0)  # higher rate than (1,10), lower metric
        hull = calculate_convex_hull(good + [bad])
        # All survivors must still be monotonic.
        self.assertNotIn(bad, hull)

    def test_lower_hull_orientation(self):
        # 4-point RD curve; lower hull yields the concave-down envelope.
        points = [
            RdPoint(rate=1.0, metric=10.0),
            RdPoint(rate=2.0, metric=18.0),
            RdPoint(rate=3.0, metric=22.0),
            RdPoint(rate=4.0, metric=23.0),
        ]
        hull = calculate_convex_hull(points)
        # At minimum the lowest-rate and highest-rate survive on the lower hull.
        self.assertIn(points[0], hull)
        self.assertIn(points[-1], hull)


class ListStatsTest(unittest.TestCase):
    """`tools/stats.py` — supplementing the existing doctests with edges."""

    def test_moving_average_simple(self):
        out = ListStats.moving_average([1, 2, 3, 4, 5], n=2, type="simple")
        # Simple MA with window 2 produces a length-equal series.
        self.assertEqual(len(out), 5)
        # The first n elements get replaced with a[n] (impl detail), so the
        # whole prefix is the same value.
        self.assertEqual(out[0], out[1])

    def test_moving_average_exponential_default(self):
        out = ListStats.moving_average([1.0, 2.0, 3.0, 4.0, 5.0], n=3)
        self.assertEqual(len(out), 5)
        # Result is finite for a well-formed input.
        self.assertTrue(np.all(np.isfinite(out)))

    def test_moving_average_unknown_type_asserts(self):
        with self.assertRaises(AssertionError):
            ListStats.moving_average([1, 2, 3], n=2, type="bogus")

    def test_nonemean_all_none(self):
        # Mean of an empty filtered list is NaN.
        result = ListStats.nonemean([None, None, None])
        self.assertTrue(np.isnan(result))

    def test_nonemean_mixed(self):
        self.assertEqual(float(ListStats.nonemean([None, 1.0, 3.0, None])), 2.0)

    def test_print_stats_runs(self):
        # Smoke test: must not raise; we don't assert format.
        ListStats.print_stats([1, 2, 3, 4, 5])

    def test_print_moving_average_stats_runs(self):
        ListStats.print_moving_average_stats([1.0, 2.0, 3.0, 4.0, 5.0], n=2, type="simple")


class YuvWriterTest(unittest.TestCase):
    """`tools/writer.py` — YuvWriter byte-stream + context-manager + asserts."""

    def test_unsupported_yuv_type_raises(self):
        with tempfile.NamedTemporaryFile(suffix=".yuv") as tmp:
            with self.assertRaises(AssertionError):
                YuvWriter(tmp.name, width=8, height=8, yuv_type="bogus")

    def test_yuv420p_8bit_uint_roundtrip(self):
        # yuv420p: U/V are 1/2 of Y in each dimension.
        with tempfile.NamedTemporaryFile(suffix=".yuv", delete=False) as tmp:
            path = tmp.name
        try:
            y = np.ones((4, 4), dtype=np.uint8) * 100
            u = np.ones((2, 2), dtype=np.uint8) * 50
            v = np.ones((2, 2), dtype=np.uint8) * 200
            with YuvWriter(path, width=4, height=4, yuv_type="yuv420p") as w:
                w.next(y, u, v, format="uint")
            with open(path, "rb") as fh:
                data = fh.read()
            # 4*4 (Y) + 2*2 (U) + 2*2 (V) = 24 bytes
            self.assertEqual(len(data), 24)
            self.assertEqual(data[0], 100)
            self.assertEqual(data[16], 50)
            self.assertEqual(data[20], 200)
        finally:
            os.remove(path)

    def test_gray_8bit_no_chroma(self):
        # gray: U/V multipliers are 0; caller must pass None.
        with tempfile.NamedTemporaryFile(suffix=".yuv", delete=False) as tmp:
            path = tmp.name
        try:
            y = np.full((4, 4), 7, dtype=np.uint8)
            with YuvWriter(path, width=4, height=4, yuv_type="gray") as w:
                w.next(y, None, None, format="uint")
            with open(path, "rb") as fh:
                data = fh.read()
            self.assertEqual(len(data), 16)
            self.assertTrue(all(b == 7 for b in data))
        finally:
            os.remove(path)

    def test_yuv420p10le_float2uint(self):
        # float2uint mode: inputs in [0, 1], scaled to uint16 with (2^10-1).
        with tempfile.NamedTemporaryFile(suffix=".yuv", delete=False) as tmp:
            path = tmp.name
        try:
            y = np.full((4, 4), 1.0, dtype=np.float64)
            u = np.full((2, 2), 0.5, dtype=np.float64)
            v = np.full((2, 2), 0.0, dtype=np.float64)
            with YuvWriter(path, width=4, height=4, yuv_type="yuv420p10le") as w:
                w.next(y, u, v, format="float2uint")
            with open(path, "rb") as fh:
                data = fh.read()
            # 4*4*2 (Y) + 2*2*2 (U) + 2*2*2 (V) = 48 bytes
            self.assertEqual(len(data), 48)
        finally:
            os.remove(path)

    def test_format_assert_rejects_bad_mode(self):
        with tempfile.NamedTemporaryFile(suffix=".yuv", delete=False) as tmp:
            path = tmp.name
        try:
            y = np.zeros((4, 4), dtype=np.uint8)
            u = np.zeros((2, 2), dtype=np.uint8)
            v = np.zeros((2, 2), dtype=np.uint8)
            with YuvWriter(path, width=4, height=4, yuv_type="yuv420p") as w:
                with self.assertRaises(AssertionError):
                    w.next(y, u, v, format="bogus")
        finally:
            os.remove(path)


class InterpolationUtilsTest(unittest.TestCase):
    """`tools/interpolation_utils.py` — PCHIP rate-from-metric interpolation."""

    def test_compute_rate_polynomial(self):
        # rate(x) = yk + s*dk + s^2*ck + s^3*bk where s = x - xk.
        # At s=0 the result must equal yk.
        self.assertEqual(
            InterpolationUtils.computeRate(x=5.0, xk=5.0, yk=42.0, dk=1.0, ck=2.0, bk=3.0),
            42.0,
        )
        # At s=1 we get yk + dk + ck + bk.
        self.assertEqual(
            InterpolationUtils.computeRate(x=6.0, xk=5.0, yk=10.0, dk=1.0, ck=2.0, bk=3.0),
            10.0 + 1.0 + 2.0 + 3.0,
        )

    def test_pchipend_opposite_slopes_zeroed(self):
        # When d*delta1 < 0 the helper zeros the endpoint slope.
        # delta1=-1, delta2=4 -> d = (3*(-1) - 1*4)/2 = -3.5; d*delta1 = 3.5 > 0,
        # then |d|=3.5 > |3*delta1|=3 AND delta1*delta2 < 0 -> d clamped to
        # 3*delta1 = -3. So pick inputs where d*delta1 actually goes negative:
        # delta1=1, delta2=10 -> d = (3*1 - 1*10)/2 = -3.5; d*delta1 = -3.5 < 0 -> 0.
        d = InterpolationUtils.pchipend(h1=1.0, h2=1.0, delta1=1.0, delta2=10.0)
        self.assertEqual(d, 0)

    def test_pchipend_clamps_to_3delta1(self):
        # Hits the elif branch: opposite-sign deltas AND |d| > |3*delta1|.
        d = InterpolationUtils.pchipend(h1=1.0, h2=1.0, delta1=-1.0, delta2=4.0)
        self.assertEqual(d, 3 * -1.0)

    def test_pchipend_normal_case(self):
        # All-positive deltas yield a positive non-zero slope.
        d = InterpolationUtils.pchipend(h1=1.0, h2=1.0, delta1=2.0, delta2=2.0)
        self.assertGreater(d, 0.0)

    def test_interpolate_rate_from_metric_4points(self):
        # Minimum 4-point RD curve — exercise the full pipeline.
        rd = [(1.0, 30.0), (2.0, 35.0), (3.0, 38.0), (4.0, 40.0)]
        out = InterpolationUtils.interpolateRateFromMetric(rd, [30.0, 35.0, 38.0, 40.0])
        # Interpolated rate at the original distortion values must lie within
        # [min_rate, max_rate] — the function clamps to the segment bounds.
        self.assertTrue(all(1.0 <= r <= 4.0 for r in out))


class DecoratorTest(unittest.TestCase):
    """`tools/decorator.py` — safe-to-cover helpers only.

    The sha1-based ``persist`` / ``persist_to_file`` / ``persist_to_dir``
    helpers carry a latent ``TypeError: Strings must be encoded before
    hashing`` bug (str argument to ``hashlib.sha1``); this test file
    deliberately does NOT exercise them — adding tests would force a
    drive-by code fix, which is out of scope for a coverage-uplift PR.
    See deliverables note.
    """

    def test_deprecated_emits_warning(self):
        @deprecated
        def f(x):
            return x + 1

        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            self.assertEqual(f(41), 42)
        self.assertTrue(any(issubclass(w.category, DeprecationWarning) for w in caught))

    def test_dummy_decorator_is_passthrough(self):
        @dummy
        def f(x):
            return x * 2

        self.assertEqual(f(3), 6)

    def test_memoized_caches_calls(self):
        call_count = {"n": 0}

        @memoized
        def f(x):
            call_count["n"] += 1
            return x * 10

        self.assertEqual(f(3), 30)
        self.assertEqual(f(3), 30)  # cache hit
        self.assertEqual(f(4), 40)  # miss
        self.assertEqual(call_count["n"], 2)

    def test_memoized_repr_returns_docstring(self):
        @memoized
        def f(x):
            """example doc"""
            return x

        self.assertEqual(repr(f), "example doc")

    def test_override_decorator_accepts_known_method(self):
        class Base:
            def method_a(self): ...

        class Sub(Base):
            @override(Base)
            def method_a(self):
                return "sub"

        self.assertEqual(Sub().method_a(), "sub")

    def test_override_decorator_rejects_unknown_method(self):
        class Base:
            def method_a(self): ...

        with self.assertRaises(AssertionError):

            class Sub(Base):  # noqa: F841
                @override(Base)
                def method_does_not_exist(self): ...

    def test_change_repr_preserves_call_semantics(self):
        def f(x):
            """orig doc"""
            return x + 1

        wrapped = change_repr(f)
        self.assertEqual(wrapped(2), 3)
        self.assertEqual(repr(wrapped), "f")
        self.assertEqual(wrapped.__doc__, "orig doc")


class ConfigTest(unittest.TestCase):
    """`config.py` — fork-local plumbing: paths + env overrides + downloader."""

    def test_vmafconfig_workspace_path_joins(self):
        result = VmafConfig.workspace_path("sub", "leaf.txt")
        self.assertTrue(result.endswith(os.path.join("sub", "leaf.txt")))

    def test_vmafconfig_resource_path_joins(self):
        result = VmafConfig.resource_path("ex", "leaf.json")
        self.assertTrue(result.endswith(os.path.join("ex", "leaf.json")))

    def test_vmafconfig_file_result_store_path(self):
        result = VmafConfig.file_result_store_path("foo")
        self.assertIn("file_result_store", result)
        self.assertTrue(result.endswith("foo"))

    def test_vmafconfig_encode_store_path(self):
        result = VmafConfig.encode_store_path("bar")
        self.assertIn("encode_store", result)
        self.assertTrue(result.endswith("bar"))

    def test_vmafconfig_workdir_path(self):
        result = VmafConfig.workdir_path("zz")
        self.assertIn("workdir", result)
        self.assertTrue(result.endswith("zz"))

    def test_vmafconfig_encode_path(self):
        result = VmafConfig.encode_path("clip.mp4")
        self.assertIn("encode", result)
        self.assertTrue(result.endswith("clip.mp4"))

    def test_vmafconfig_test_resource_path_bypass_download(self):
        # bypass_download=True must not hit the network; result is just the
        # joined local path.
        result = VmafConfig.test_resource_path("yuv", "x.yuv", bypass_download=True)
        self.assertTrue(result.endswith(os.path.join("yuv", "x.yuv")))

    def test_vmafconfig_tools_resource_path(self):
        result = VmafConfig.tools_resource_path("foo.bin")
        self.assertTrue(result.endswith(os.path.join("tools", "resource", "foo.bin")))
        # Regression (R3-20): the path must point at the relocated
        # compat/python-vmaf/ tree (ADR-0700), not the stale python/vmaf/
        # prefix. Assert a real shipped resource resolves to an existing file —
        # the old endswith()-only check passed even with the wrong prefix.
        self.assertNotIn(os.path.join("python", "vmaf", "tools"), result)
        hm = VmafConfig.tools_resource_path("Hanley_McNeil.mat")
        self.assertTrue(
            os.path.isfile(hm),
            "tools_resource_path must resolve to the relocated resource tree; "
            "Hanley_McNeil.mat not found at %s" % hm,
        )

    def test_vmafconfig_model_path(self):
        result = VmafConfig.model_path("vmaf_v0.6.1.json")
        self.assertTrue(result.endswith(os.path.join("model", "vmaf_v0.6.1.json")))

    def test_vmafconfig_root_path(self):
        result = VmafConfig.root_path("README.md")
        self.assertTrue(os.path.isabs(result))
        self.assertTrue(result.endswith("README.md"))

    def test_download_reactively_skips_if_local_exists(self):
        # No-op when the file already exists; no network call.
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
            path = tmp.name
        try:
            with mock.patch.object(vmaf_config.urllib.request, "urlretrieve") as ret:
                download_reactively(path, "https://example.invalid/x")
            ret.assert_not_called()
        finally:
            os.remove(path)

    def test_download_reactively_happy_path(self):
        # Stub urlretrieve so we don't hit the network; just write a byte.
        with tempfile.TemporaryDirectory() as tmpdir:
            target = os.path.join(tmpdir, "subdir", "downloaded.bin")

            def fake_urlretrieve(url, tmp_path):
                with open(tmp_path, "wb") as fh:
                    fh.write(b"ok")

            with mock.patch.object(
                vmaf_config.urllib.request, "urlretrieve", side_effect=fake_urlretrieve
            ):
                download_reactively(target, "https://example.invalid/x")
            self.assertTrue(os.path.exists(target))
            with open(target, "rb") as fh:
                self.assertEqual(fh.read(), b"ok")

    def test_download_reactively_cleans_up_partial_on_failure(self):
        # urlretrieve raises -> partial tempfile must be removed.
        with tempfile.TemporaryDirectory() as tmpdir:
            target = os.path.join(tmpdir, "fail.bin")
            with mock.patch.object(
                vmaf_config.urllib.request,
                "urlretrieve",
                side_effect=RuntimeError("network down"),
            ):
                with self.assertRaises(RuntimeError):
                    download_reactively(target, "https://example.invalid/x")
            # No stray .part files left behind.
            leftovers = [n for n in os.listdir(tmpdir) if n.endswith(".part") or "fail.bin." in n]
            self.assertEqual(leftovers, [])
            self.assertFalse(os.path.exists(target))

    def test_download_reactively_propagates_http_error(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            target = os.path.join(tmpdir, "404.bin")
            err = urllib.error.HTTPError(
                "https://example.invalid/x", 404, "Not Found", hdrs=None, fp=None
            )
            with mock.patch.object(vmaf_config.urllib.request, "urlretrieve", side_effect=err):
                with self.assertRaises(urllib.error.HTTPError):
                    download_reactively(target, "https://example.invalid/x")

    def test_external_config_path_lookups_return_none_without_externals(self):
        # On a clean checkout with no externals.py, every external lookup
        # falls through to None — no AttributeError, no import explosion.
        for getter in (
            VmafExternalConfig.ffmpeg_path,
            VmafExternalConfig.matlab_path,
            VmafExternalConfig.matlab_runtime_path,
            VmafExternalConfig.cvx_path,
            VmafExternalConfig.psnr_path,
            VmafExternalConfig.moment_path,
            VmafExternalConfig.ssim_path,
            VmafExternalConfig.ms_ssim_path,
            VmafExternalConfig.vmaf_path,
            VmafExternalConfig.vmafexec_path,
        ):
            # We can't assert is-None universally (a dev may have externals.py
            # configured), but the call must not raise and must return either
            # None or a string-like path.
            result = getter()
            self.assertTrue(result is None or isinstance(result, str))

    def test_external_config_ffmpeg_env_does_not_raise(self):
        # Same property for the non-path attr lookup.
        env = VmafExternalConfig.ffmpeg_env()
        self.assertTrue(env is None or isinstance(env, dict))

    def test_external_config_get_and_assert_missing_raises(self):
        # If the path is None, the assert helpers must raise AssertionError.
        with mock.patch.object(VmafExternalConfig, "ffmpeg_path", return_value=None):
            with self.assertRaises(AssertionError):
                VmafExternalConfig.get_and_assert_ffmpeg()
        with mock.patch.object(VmafExternalConfig, "matlab_path", return_value=None):
            with self.assertRaises(AssertionError):
                VmafExternalConfig.get_and_assert_matlab()
        with mock.patch.object(VmafExternalConfig, "matlab_runtime_path", return_value=None):
            with self.assertRaises(AssertionError):
                VmafExternalConfig.get_and_assert_matlab_runtime()
        with mock.patch.object(VmafExternalConfig, "cvx_path", return_value=None):
            with self.assertRaises(AssertionError):
                VmafExternalConfig.get_and_assert_cvx()

    def test_external_config_get_and_assert_returns_path_when_set(self):
        with mock.patch.object(VmafExternalConfig, "ffmpeg_path", return_value="/usr/bin/ffmpeg"):
            self.assertEqual(VmafExternalConfig.get_and_assert_ffmpeg(), "/usr/bin/ffmpeg")


if __name__ == "__main__":
    unittest.main()
