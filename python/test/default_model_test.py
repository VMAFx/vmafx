# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Fork-added coverage for the built-in default model (ADR-1169).

The fork scores with ``vmaf_v1.0.16_3d0h`` when the caller names no model,
where upstream Netflix still defaults to ``vmaf_v0.6.1``. Upstream's own
``test_run_vmafexec_runner_use_default_built_in_model`` used to cover the
no-``--model`` path; it now names ``vmaf_v0.6.1`` explicitly, because its
assertions are golden values for the v0.6.1 feature family and the v1.0.16
family does not emit those features at all. This file picks up the coverage
that test gave up.

Deliberately **no hardcoded score values**. Inventing golden numbers for the
new default would be asserting our own output back at ourselves. Instead these
tests assert the property that actually matters and that actually broke: which
model the default resolves to, established by comparing the default invocation
against an explicitly-named one.
"""

from __future__ import absolute_import

import os
import re
import subprocess
import tempfile
import unittest
from test.testutil import set_default_576_324_videos_for_testing

from vmaf import ExternalProgram
from vmaf.config import VmafConfig

#: Must equal ``VMAF_DEFAULT_MODEL_VERSION`` in ``core/include/libvmaf/model.h``.
#: ``scripts/ci/check-default-model-single-source.sh`` does not police this file
#: (tests are exempt by design), so a drift here shows up as a test failure
#: rather than a gate failure -- which is the point of having it.
EXPECTED_DEFAULT_MODEL = "vmaf_v1.0.16_3d0h"

#: A feature only the v0.6.1 family emits. Its ABSENCE under the default is the
#: exact condition that made the upstream golden test raise
#: KeyError('VMAFEXEC_vif_scale0_score') once the default moved.
V0_ONLY_FEATURE = "vif_scale0"

#: Features characteristic of the v1.0.16 family.
V1_FEATURES = ("integer_aim", "cambi", "speed_chroma")

_METRIC_RE = re.compile(r'<metric name="([^"]+)"')


def _score(extra_args):
    """Score the standard 576x324 pair and return the set of metric names."""
    ref_path, dis_path, _asset, _asset_original = set_default_576_324_videos_for_testing()
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.xml")
        cmd = [
            ExternalProgram.vmafexec,
            "-r",
            ref_path,
            "-d",
            dis_path,
            "-w",
            "576",
            "-h",
            "324",
            "-p",
            "420",
            "-b",
            "8",
            "-o",
            out,
        ] + list(extra_args)
        subprocess.run(cmd, check=True, capture_output=True)
        with open(out, encoding="utf-8") as fh:
            return set(_METRIC_RE.findall(fh.read()))


class DefaultBuiltInModelTest(unittest.TestCase):
    """The no-``--model`` invocation resolves to the fork's declared default."""

    def test_default_matches_explicitly_named_default_model(self):
        # The strongest available statement that the default IS the model we
        # say it is, without hardcoding a single score: the metric-key set the
        # default produces must equal the set produced by naming it.
        implicit = _score([])
        explicit = _score(["-m", f"version={EXPECTED_DEFAULT_MODEL}"])
        self.assertEqual(
            implicit,
            explicit,
            f"the no---model invocation does not match {EXPECTED_DEFAULT_MODEL}; "
            "either the default changed without this test being updated, or "
            "core/include/libvmaf/model.h and this file disagree",
        )

    def test_default_emits_the_v1_feature_family(self):
        keys = _score([])
        for feature in V1_FEATURES:
            self.assertTrue(
                any(feature in k for k in keys),
                f"default model emits no {feature!r} metric; the v1.0.16 family "
                f"should. Emitted: {sorted(keys)}",
            )

    def test_default_does_not_emit_the_v0_only_feature(self):
        # This is the condition that forced the upstream golden test to name
        # its model. Pinning it here means a silent revert of the default would
        # fail loudly with an explanation instead of resurfacing as a KeyError
        # in an unrelated upstream test.
        keys = _score([])
        self.assertFalse(
            any(V0_ONLY_FEATURE in k for k in keys),
            f"default model emits {V0_ONLY_FEATURE!r}, which belongs to the "
            "v0.6.1 family -- has the default been reverted?",
        )

    def test_v0_6_1_is_still_selectable_and_emits_its_own_family(self):
        # Changing the default must not remove the old model. Downstream users
        # pinning vmaf_v0.6.1 (and the AOM CTC preset, which the spec requires
        # to use it) depend on this.
        keys = _score(["-m", "version=vmaf_v0.6.1"])
        self.assertTrue(
            any(V0_ONLY_FEATURE in k for k in keys),
            f"vmaf_v0.6.1 no longer emits {V0_ONLY_FEATURE!r}: {sorted(keys)}",
        )

    def test_small_resolution_fails_loudly_even_when_quiet(self):
        # vmaf_v1.0.16_3d0h needs cambi (width or height >= 216) and
        # speed_chroma; at 160x90 neither can run. The CLI must refuse with an
        # error that names the model, the feature and the constraint - and it
        # must do so even under --quiet, because a silent wrong-model score is
        # exactly the failure this test exists to prevent. A silent fallback to
        # vmaf_v0.6.1 was rejected: it would hardcode a second default
        # (contradicting ADR-1168) and make scores incomparable across a
        # mixed-resolution corpus.
        ref_path = VmafConfig.test_resource_path(
            "yuv",
            "ref_test_0_1_src01_hrc00_576x324_576x324_vs_src01_hrc01_576x324_576x324_q_160x90.yuv",
        )
        dis_path = VmafConfig.test_resource_path(
            "yuv",
            "dis_test_0_1_src01_hrc00_576x324_576x324_vs_src01_hrc01_576x324_576x324_q_160x90.yuv",
        )
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "out.xml")
            cmd = [
                ExternalProgram.vmafexec,
                "-r",
                ref_path,
                "-d",
                dis_path,
                "-w",
                "160",
                "-h",
                "90",
                "-p",
                "420",
                "-b",
                "8",
                "-o",
                out,
                "--quiet",
            ]
            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
            self.assertNotEqual(
                proc.returncode,
                0,
                "sub-216 input with the default model must fail, not score silently",
            )
            self.assertIn("vmaf_v1.0.16_3d0h", proc.stderr, proc.stderr)
            self.assertIn("cambi", proc.stderr, proc.stderr)
            self.assertIn("216", proc.stderr, proc.stderr)
            self.assertIn("--model", proc.stderr, proc.stderr)
            self.assertFalse(
                os.path.exists(out) and os.path.getsize(out) > 0,
                "no output file may be written when the model cannot run",
            )

    def test_small_resolution_scores_with_an_explicit_model_that_fits(self):
        # The escape hatch the error message names: an explicit --model that
        # has no sub-SD constraint runs at 160x90 exactly as before.
        ref_path = VmafConfig.test_resource_path(
            "yuv",
            "ref_test_0_1_src01_hrc00_576x324_576x324_vs_src01_hrc01_576x324_576x324_q_160x90.yuv",
        )
        dis_path = VmafConfig.test_resource_path(
            "yuv",
            "dis_test_0_1_src01_hrc00_576x324_576x324_vs_src01_hrc01_576x324_576x324_q_160x90.yuv",
        )
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "out.xml")
            cmd = [
                ExternalProgram.vmafexec,
                "-r",
                ref_path,
                "-d",
                dis_path,
                "-w",
                "160",
                "-h",
                "90",
                "-p",
                "420",
                "-b",
                "8",
                "-o",
                out,
                "--model",
                "version=vmaf_v0.6.1",
            ]
            subprocess.run(cmd, capture_output=True, text=True, check=True)
            with open(out, encoding="utf-8") as fh:
                keys = set(_METRIC_RE.findall(fh.read()))
            self.assertTrue(
                any(V0_ONLY_FEATURE in k for k in keys),
                f"explicit v0.6.1 should score at 160x90; got keys: {sorted(keys)}",
            )


class NegRoutingTest(unittest.TestCase):
    """`--neg` must resolve to a model that actually exists (ADR-1169).

    There is no NEG counterpart to any ``vmaf_v1.0.16_*`` model, so a NEG
    router that appends ``"neg"`` to the default synthesises
    ``vmaf_v1.0.16_3d0hneg``, which libvmaf rejects at load. That shipped
    briefly in the Go per-shot router and aborted every scored shot. These
    tests assert the end state that matters: whatever NEG resolves to, the
    binary can load it.
    """

    def _loads(self, version):
        ref_path, dis_path, _a, _b = set_default_576_324_videos_for_testing()
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "out.xml")
            proc = subprocess.run(
                [
                    ExternalProgram.vmafexec,
                    "-r",
                    ref_path,
                    "-d",
                    dis_path,
                    "-w",
                    "576",
                    "-h",
                    "324",
                    "-p",
                    "420",
                    "-b",
                    "8",
                    "-m",
                    f"version={version}",
                    "-o",
                    out,
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            return proc.returncode == 0, proc.stderr

    def test_neg_default_is_a_loadable_model(self):
        ok, err = self._loads("vmaf_v0.6.1neg")
        self.assertTrue(ok, f"the NEG default does not load: {err}")

    def test_appending_neg_to_the_default_is_NOT_a_real_model(self):
        # Pins the reason DEFAULT_MODEL_NEG is an independent constant rather
        # than DEFAULT_MODEL + "neg". If this ever starts passing, Netflix has
        # published a v1 NEG model and the NEG constants should follow it.
        ok, _err = self._loads(EXPECTED_DEFAULT_MODEL + "neg")
        self.assertFalse(
            ok,
            f"{EXPECTED_DEFAULT_MODEL}neg now loads -- a v1 NEG model exists, so "
            "DEFAULT_MODEL_NEG / DefaultNEGVersion should be updated to use it",
        )


class DefaultModelMirrorTest(unittest.TestCase):
    """The Python mirrors agree with the C header (ADR-1168)."""

    def test_vmaftune_mirror_matches_expected_default(self):
        try:
            from vmaftune.defaultmodel import DEFAULT_MODEL
        except ImportError:
            self.skipTest("vmaf-tune is not installed in this environment")
        self.assertEqual(DEFAULT_MODEL, EXPECTED_DEFAULT_MODEL)

    def test_neg_default_stays_on_the_v0_6_1_family(self):
        # There is no NEG counterpart to any vmaf_v1.0.16_* model, so the NEG
        # default deliberately names a different generation. Deriving it as
        # DEFAULT_MODEL + "neg" would synthesise a model that does not exist.
        try:
            from vmaftune.defaultmodel import DEFAULT_MODEL_NEG
        except ImportError:
            self.skipTest("vmaf-tune is not installed in this environment")
        self.assertEqual(DEFAULT_MODEL_NEG, "vmaf_v0.6.1neg")
        self.assertNotEqual(DEFAULT_MODEL_NEG, EXPECTED_DEFAULT_MODEL + "neg")


if __name__ == "__main__":
    unittest.main(verbosity=2)
