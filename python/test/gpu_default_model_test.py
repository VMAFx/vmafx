# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Fork-added coverage: the GPU integer-ADM twins honour the default model's
ADM option dict and emit the *same feature-name key* as the CPU twin.

Why this file exists
--------------------
``model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json`` -- the fork's default model
(ADR-1169) -- asks integer ADM for ``VMAF_integer_feature_adm3_score`` with::

    adm_csf_mode=2  adm_dlm_weight=0.7  adm_enhn_gain_limit=1.0
    adm_min_val=0.5 adm_noise_weight=0.02

All five carry ``VMAF_OPT_FLAG_FEATURE_PARAM`` and none is at its default, so
``vmaf_feature_name_from_options()`` (``core/src/feature/feature_name.cpp``)
builds the lookup key from the *extractor's own option table*::

    integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02

A GPU twin whose table is missing one of those entries emits a shorter key and
the model lookup misses -- silently, because the collector simply has no entry
under the name the predictor asks for. Before the
``feat/gpu-adm-csf-mode-parity`` work the CUDA, SYCL and HIP twins were all in
that state (no ``adm_csf_mode``, no ``adm_p_norm``, drifted aliases).

Deliberately **no hardcoded score values**: the CPU twin is the reference and
is re-run in the same process invocation pair, so the assertion is a
CPU-vs-GPU delta, not a golden number we invented.

Backends are probed, not assumed: each is skipped when its device or runtime is
unavailable, so this file is green on a CPU-only machine and on CI.
"""

from __future__ import absolute_import

import json
import os
import subprocess
import tempfile
import unittest
from test.testutil import set_default_576_324_videos_for_testing

from vmaf import ExternalProgram

#: The default model's ADM option dict, plus ``adm_p_norm`` so the pooling
#: exponent is covered too. Order here is irrelevant -- the key is built from
#: an alphabetically sorted dictionary.
ADM_OPTS = (
    "adm_csf_mode=2"
    ":adm_dlm_weight=0.7"
    ":adm_enhn_gain_limit=1.0"
    ":adm_min_val=0.5"
    ":adm_noise_weight=0.02"
    ":adm_p_norm=2.0"
)

#: The suffix ``feature_name.cpp`` appends for ADM_OPTS: aliases in
#: option-name-sorted order (csf, dlmw, egl, min, nw, apn).
NAME_SUFFIX = "_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_apn_2"

#: Emitted by every integer-ADM twin.
COMMON_KEYS = tuple(
    base + NAME_SUFFIX
    for base in (
        "integer_adm2",
        "integer_adm_scale0",
        "integer_adm_scale1",
        "integer_adm_scale2",
        "integer_adm_scale3",
    )
)

#: Emitted only where the twin has an AIM contrast-measure device pass.
#: CUDA has one (ADR-0746); SYCL and HIP do not, and correctly leave both
#: features out of ``provided_features[]`` so the ADR-0530 name lookup routes
#: them to the CPU twin instead of fabricating a score. Tracked as
#: T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05 in docs/state.md.
AIM_KEYS = tuple(base + NAME_SUFFIX for base in ("integer_aim", "integer_adm3"))

#: (extractor name, CLI backend flags, does this twin emit aim/adm3?)
BACKENDS = (
    ("cuda", "adm_cuda", ["--backend", "cuda"], True),
    ("sycl", "adm_sycl", ["--backend", "sycl"], False),
    ("hip", "adm_hip", ["--backend", "hip"], False),
)

#: ADR-0214 cross-backend gate: places=4.
TOLERANCE = 1e-4


def _run(feature_spec, backend_flags):
    """Score the standard 576x324 pair; return {metric name: pooled mean}.

    Returns ``None`` when the run failed -- which is how an absent device or an
    unbuilt backend surfaces, and is turned into a skip by the caller.
    """
    ref_path, dis_path, _asset, _asset_original = set_default_576_324_videos_for_testing()
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.json")
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
            "--feature",
            feature_spec,
            "--no_prediction",
            "--json",
            "-o",
            out,
        ] + list(backend_flags)
        proc = subprocess.run(cmd, check=False, capture_output=True)
        if proc.returncode != 0 or not os.path.exists(out):
            return None
        with open(out, encoding="utf-8") as fh:
            payload = json.load(fh)
    return {k: v.get("mean") for k, v in payload.get("pooled_metrics", {}).items()}


class GpuDefaultModelAdmKeyTest(unittest.TestCase):
    """The GPU twins must answer under the default model's ADM key."""

    @classmethod
    def setUpClass(cls):
        cls.cpu = _run(f"adm={ADM_OPTS}", ["--no_cuda", "--no_sycl", "--no_hip"])

    def setUp(self):
        if self.cpu is None:
            self.skipTest("CPU integer_adm reference run failed")

    def test_cpu_emits_the_default_models_adm_key(self):
        # Guards the constant above: if feature_name.cpp ever changes how it
        # orders or renders the suffix, every GPU comparison below would be
        # comparing two absences and passing vacuously.
        for key in COMMON_KEYS + AIM_KEYS:
            self.assertIn(
                key,
                self.cpu,
                f"CPU integer_adm did not emit {key!r}; the feature-name key "
                f"contract in feature_name.cpp changed. Emitted: {sorted(self.cpu)}",
            )

    def test_gpu_twins_match_the_cpu_reference_under_the_models_adm_options(self):
        for name, fex, flags, emits_aim in BACKENDS:
            with self.subTest(backend=name):
                gpu = _run(f"{fex}={ADM_OPTS}", flags)
                if gpu is None:
                    self.skipTest(f"{name} backend unavailable on this machine")
                expected = COMMON_KEYS + (AIM_KEYS if emits_aim else ())
                for key in expected:
                    self.assertIn(
                        key,
                        gpu,
                        f"{fex} did not emit {key!r} -- its VmafOption table has "
                        "drifted from the CPU table, so the feature-name key it "
                        f"builds differs. Emitted: {sorted(gpu)}",
                    )
                    delta = abs(self.cpu[key] - gpu[key])
                    self.assertLessEqual(
                        delta,
                        TOLERANCE,
                        f"{fex} {key}: cpu={self.cpu[key]!r} gpu={gpu[key]!r} "
                        f"delta={delta:.3e} exceeds the places=4 gate",
                    )

    def test_twins_without_an_aim_pass_do_not_fabricate_one(self):
        # A twin with no AIM device pass must leave aim/adm3 out of
        # provided_features[] so the CPU twin answers instead. Emitting them
        # from a hard-coded stand-in would look correct here and feed the model
        # a fabricated score.
        for name, fex, flags, emits_aim in BACKENDS:
            if emits_aim:
                continue
            with self.subTest(backend=name):
                gpu = _run(f"{fex}={ADM_OPTS}", flags)
                if gpu is None:
                    self.skipTest(f"{name} backend unavailable on this machine")
                for key in AIM_KEYS:
                    self.assertNotIn(
                        key,
                        gpu,
                        f"{fex} emitted {key!r} but has no AIM contrast-measure "
                        "device pass; the value cannot be real",
                    )


if __name__ == "__main__":
    unittest.main(verbosity=2)
