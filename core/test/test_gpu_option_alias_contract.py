#!/usr/bin/env python3
"""Keep GPU option aliases identical to their CPU reference extractors."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# ADR-1183 makes these aliases part of the collector-key ABI whenever the
# option differs from its default.  The CPU spelling is therefore authority.
EXPECTED_ALIASES = (
    ("cuda/float_adm_cuda.c", "adm_csf_scale", "scf"),
    ("cuda/float_adm_cuda.c", "adm_csf_diag_scale", "scfd"),
    ("sycl/float_adm_sycl.cpp", "adm_csf_scale", "scf"),
    ("sycl/float_adm_sycl.cpp", "adm_csf_diag_scale", "scfd"),
    ("hip/float_adm_hip.c", "adm_csf_scale", "scf"),
    ("hip/float_adm_hip.c", "adm_csf_diag_scale", "scfd"),
    ("cuda/float_motion_cuda.c", "motion_force_zero", "force_0"),
    ("sycl/float_motion_sycl.cpp", "motion_force_zero", "force_0"),
    ("hip/float_motion_hip.c", "motion_force_zero", "force_0"),
    ("metal/float_motion_metal.mm", "motion_force_zero", "force_0"),
    ("cuda/integer_motion_cuda.c", "motion_force_zero", "force_0"),
    ("sycl/integer_motion_sycl.cpp", "motion_force_zero", "force_0"),
    ("hip/integer_motion_hip.c", "motion_force_zero", "force_0"),
    ("cuda/float_vif_cuda.c", "vif_kernelscale", "ks"),
    ("sycl/float_vif_sycl.cpp", "vif_kernelscale", "ks"),
    ("hip/float_vif_hip.c", "vif_kernelscale", "ks"),
    ("cuda/integer_vif_cuda.c", "vif_skip_scale0", "ssclz"),
    ("sycl/integer_vif_sycl.cpp", "vif_skip_scale0", "ssclz"),
    ("hip/integer_vif_hip.c", "vif_skip_scale0", "ssclz"),
)


def option_initializer(source: str, option: str) -> str:
    """Return the balanced initializer containing one named option."""
    matches = list(re.finditer(rf'\.name\s*=\s*"{re.escape(option)}"', source))
    if len(matches) != 1:
        raise AssertionError(f"expected one {option!r} option, found {len(matches)}")

    start = source.rfind("{", 0, matches[0].start())
    if start < 0:
        raise AssertionError(f"{option!r} has no initializer start")

    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"{option!r} has no initializer end")


class GpuOptionAliasContractTest(unittest.TestCase):
    def test_gpu_aliases_match_cpu_collector_key_spelling(self) -> None:
        feature_root = ROOT / "core/src/feature"
        for relative_path, option, expected_alias in EXPECTED_ALIASES:
            with self.subTest(path=relative_path, option=option):
                source = (feature_root / relative_path).read_text(encoding="utf-8")
                initializer = option_initializer(source, option)
                alias = re.search(r'\.alias\s*=\s*"([^"]+)"', initializer)
                self.assertIsNotNone(alias, f"{relative_path}: {option} has no alias")
                self.assertEqual(alias.group(1), expected_alias)


if __name__ == "__main__":
    unittest.main()
