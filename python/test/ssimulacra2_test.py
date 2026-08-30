"""Fork-added snapshot gate for the SSIMULACRA 2 feature extractor (T3-3,
ADR-0164). Pins the CPU output of `vmaf --feature ssimulacra2` against
reference values generated on the current master.

Unlike the Netflix golden assertions (`python/test/quality_runner_test.py`
et al.), SSIMULACRA 2 is a fork-added metric — these values are NOT part
of the upstream Netflix bit-exactness contract. They pin the fork's
self-consistency; drift here means the SIMD ports (or scalar) changed
behaviour.

Tolerance is `places=4` (1e-4). Both the phase-1 `cbrtf` and the
phase-3 sRGB EOTF (`powf((x + 0.055)/1.055, 2.4)`) have been
replaced with deterministic host-independent implementations in
`core/src/feature/ssimulacra2_math.h` — a Newton-Raphson
cube root (accuracy ~7e-7) and a 1024-entry LUT for the EOTF
(accuracy ~5e-7, LUT values committed as hardcoded hex-float
literals by the `scripts/gen_ssimulacra2_eotf_lut.py` generator).
No runtime libm dependency for transcendentals.

The reference values below are captured from a Linux x86_64 build
(gcc, AVX2 path) and constitute the primary CI gate. The `places=4`
tolerance (1e-4) holds stably on Linux x86_64 across gcc and clang.

Cross-arch note: Apple Clang on macOS arm64 (Apple Silicon) may
produce per-frame scores that differ by up to ~1e-2 from the Linux
x86_64 reference. The source is the Gaussian IIR blur recurrence:
  out_k = n2_k * sum - d1_k * prev1_k - prev2_k
Apple Clang auto-contracts the `mul + sub` pair to a single-rounded
FMLS instruction even with `#pragma STDC FP_CONTRACT OFF`, while gcc
on Linux x86_64 honours the pragma and emits separate multiply and
subtract instructions. The resulting float blur outputs differ by up
to 1 ULP per IIR step; these propagate through the 6-scale pyramid
and can shift individual per-frame scores by ~1e-2 and the 48-frame
mean by ~1e-3. Per-arch bit-exactness (scalar vs SIMD on the same
machine) is verified by `test_ssimulacra2_simd`.

The ADR-0891 FMA unification (round-2) makes the colour-matrix
stage (`picture_to_linear_rgb`) bit-identical across all SIMD paths
and compilers by using explicit FMA intrinsics everywhere. The IIR
blur stage remains subject to the Apple Clang FP-contract behaviour
described above.
"""

import json
import os
import subprocess
import tempfile
import unittest

from vmaf import ExternalProgram
from vmaf.config import VmafConfig


class Ssimulacra2SnapshotTest(unittest.TestCase):
    """Snapshot gate for the `ssimulacra2` feature extractor (fork-added)."""

    RC_SUCCESS = 0

    @staticmethod
    def _primary_fixture_expected():
        # Reference values from Linux x86_64 build (gcc, AVX2), captured
        # with the ADR-0891 round-2 FMA-unified colour-matrix path active.
        # Regenerated via `/regen-snapshots` if the implementation changes.
        return {
            "mean": 24.614428,
            "min": 13.816386,
            "max": 49.968184,
            "harmonic_mean": 22.904302,
            "frame0": 49.968184,
            "frame47": 37.415193,
        }

    def setUp(self):
        self.output_file_path = tempfile.NamedTemporaryFile(delete=False, suffix=".json").name

    def tearDown(self):
        if os.path.exists(self.output_file_path):
            os.remove(self.output_file_path)

    def _run_ssimulacra2(self, ref, dis, width, height, bitdepth=8):
        """Invoke `vmaf --feature ssimulacra2` and return the parsed JSON."""
        exe = ExternalProgram.vmafexec
        cmd = [
            exe,
            "--reference",
            ref,
            "--distorted",
            dis,
            "--width",
            str(width),
            "--height",
            str(height),
            "--pixel_format",
            "420",
            "--bitdepth",
            str(bitdepth),
            "--json",
            "--feature",
            "ssimulacra2",
            "--output",
            self.output_file_path,
            "--quiet",
        ]
        ret = subprocess.call(cmd)
        self.assertEqual(ret, self.RC_SUCCESS, f"vmaf exited {ret}: {cmd}")
        with open(self.output_file_path) as fo:
            return json.load(fo)

    def test_ssimulacra2_src01_576x324(self):
        """src01_hrc00 vs src01_hrc01 at 576x324, 48 frames. Primary gate."""
        result = self._run_ssimulacra2(
            VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324.yuv"),
            VmafConfig.test_resource_path("yuv", "src01_hrc01_576x324.yuv"),
            576,
            324,
        )
        pooled = result["pooled_metrics"]["ssimulacra2"]
        frames = result["frames"]
        expected = self._primary_fixture_expected()
        self.assertEqual(len(frames), 48)
        self.assertAlmostEqual(pooled["mean"], expected["mean"], places=4)
        self.assertAlmostEqual(pooled["min"], expected["min"], places=4)
        self.assertAlmostEqual(pooled["max"], expected["max"], places=4)
        self.assertAlmostEqual(pooled["harmonic_mean"], expected["harmonic_mean"], places=4)
        self.assertAlmostEqual(frames[0]["metrics"]["ssimulacra2"], expected["frame0"], places=4)
        self.assertAlmostEqual(frames[47]["metrics"]["ssimulacra2"], expected["frame47"], places=4)

    def test_ssimulacra2_small_160x90(self):
        """Tiny 160x90 derived fixture — exercises the scale tail path."""
        ref_name = (
            "ref_test_0_1_src01_hrc00_576x324_576x324_vs_"
            "src01_hrc01_576x324_576x324_q_160x90.yuv"
        )
        dis_name = (
            "dis_test_0_1_src01_hrc00_576x324_576x324_vs_"
            "src01_hrc01_576x324_576x324_q_160x90.yuv"
        )
        result = self._run_ssimulacra2(
            VmafConfig.test_resource_path("yuv", ref_name),
            VmafConfig.test_resource_path("yuv", dis_name),
            160,
            90,
        )
        pooled = result["pooled_metrics"]["ssimulacra2"]
        frames = result["frames"]
        self.assertEqual(len(frames), 48)
        # Reference values from Linux x86_64 build (gcc, AVX2).
        self.assertAlmostEqual(pooled["mean"], 77.692804, places=4)
        self.assertAlmostEqual(pooled["min"], 72.804479, places=4)
        self.assertAlmostEqual(pooled["max"], 86.797437, places=4)
        self.assertAlmostEqual(frames[0]["metrics"]["ssimulacra2"], 86.797437, places=4)
        self.assertAlmostEqual(frames[47]["metrics"]["ssimulacra2"], 82.603522, places=4)


if __name__ == "__main__":
    unittest.main()
