#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Lock strict floating-point flags to each compiler's argument syntax.

Covers the host policy (C / SIMD / CUDA host) and, since ADR-1367, the SYCL
device policy: the one flag line every SYCL feature TU and the SYCL link get.
"""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SOURCE_MESON = Path(__file__).resolve().parents[1] / "src" / "meson.build"
TEST_MESON = Path(__file__).resolve().with_name("meson.build")
POLICY_BEGIN = "# BEGIN VMAF strict FP compiler-argument policy"
POLICY_END = "# END VMAF strict FP compiler-argument policy"
SYCL_POLICY_BEGIN = "# BEGIN VMAF SYCL strict FP policy"
SYCL_POLICY_END = "# END VMAF SYCL strict FP policy"

# ADR-1367: toolchain -> (is_sycl_acpp, sycl_strict_fp_args, sycl_fp32_prec_args).
# icpx: fast model off, then contraction off (the order is load-bearing:
# -fp-model=precise implies -ffp-contract=on), then correctly rounded fp32
# division and square root. AdaptiveCpp accepts only contraction-off.
SYCL_PREC = ["-foffload-fp32-prec-div", "-foffload-fp32-prec-sqrt"]
SYCL_MATRIX = {
    "icpx": (False, ["-fp-model=precise", "-ffp-contract=off", *SYCL_PREC], SYCL_PREC),
    "acpp": (True, ["-ffp-contract=off"], []),
}
# The icpx driver link carries the precision pair for the SPIR-V JIT image. The
# MSVC build (ADR-1364) links with link.exe, and its explicit device link, which
# generates every image, takes the whole line; so does the probe's in the tests.
SYCL_LINK_ARGS = (
    "sycl_link_args = sycl_msvc_device_link ? ['/IGNORE:4078'] : (['-fsycl'] + sycl_fp32_prec_args)"
)
SYCL_DEVICE_LINK = "command : [icpx] + sycl_device_link_args + sycl_strict_fp_args"
SYCL_FEATURE_TAIL = "sycl_feature_tail_args = ['-std=c++20'] + sycl_strict_fp_args"

COMPILER_MATRIX = {
    "gcc": (
        "linux",
        [],
        ["-ffp-contract=off"],
        ["-Xcompiler=-ffp-contract=off"],
    ),
    "clang": (
        "linux",
        [],
        ["-ffp-contract=off"],
        ["-Xcompiler=-ffp-contract=off"],
    ),
    "intel-llvm": (
        "linux",
        ["-fp-model=precise"],
        ["-fp-model=precise", "-ffp-contract=off"],
        ["-Xcompiler=-ffp-contract=off"],
    ),
    "msvc": ("windows", [], ["/fp:precise"], ["-Xcompiler=/fp:precise"]),
    "intel-llvm-cl": (
        "windows",
        ["/fp:precise"],
        ["/fp:precise", "/Qfma-"],
        ["-Xcompiler=/fp:precise"],
    ),
    "clang-cl": (
        "windows",
        [],
        ["/clang:-ffp-contract=off"],
        ["-Xcompiler=/fp:precise"],
    ),
}

STRICT_TARGETS = (
    "arm64_fp_lib",
    "arm64_adm_dwt2_neon_lib",
    "arm64_ssim_neon_lib",
    "arm64_ssimulacra2_lib",
    "arm64_ssimulacra2_sve2_lib",
    "arm64_moment_sve2_lib",
    "x86_ssim_avx2_lib",
    "x86_psnr_hvs_avx2_lib",
    "x86_ms_ssim_decimate_avx2_lib",
    "x86_ssimulacra2_avx2_lib",
    "x86_float_adm_avx2_lib",
    "x86_speed_matmul_avx2_lib",
    "x86_ssimulacra2_avx512_lib",
    "x86_float_adm_avx512_lib",
    "x86_speed_matmul_avx512_lib",
    "libvmaf_psnr_hvs_scalar_static_lib",
    "libvmaf_ssimulacra2_static_lib",
    "libvmaf_y_funque_plus_static_lib",
)


def _meson_command() -> list[str]:
    """Resolve Meson from this interpreter, then fall back to PATH."""
    try:
        spec = importlib.util.find_spec("mesonbuild.mesonmain")
    except (ImportError, ValueError):
        spec = None
    if spec is not None:
        return [sys.executable, "-m", "mesonbuild.mesonmain"]
    on_path = shutil.which("meson")
    return [] if on_path is None else [on_path]


MESON_COMMAND = _meson_command()


def _marked_block(begin: str, end: str, what: str) -> str:
    source = SOURCE_MESON.read_text(encoding="utf-8")
    try:
        start = source.index(begin)
        stop = source.index(end, start) + len(end)
    except ValueError as exc:
        raise AssertionError(f"{what} policy markers are missing") from exc
    return source[start:stop]


def _policy_block() -> str:
    block = _marked_block(POLICY_BEGIN, POLICY_END, "strict-FP compiler")
    return block.replace(
        "_strict_fp_compiler_id = cc.get_id()",
        "_strict_fp_compiler_id = strict_fp_fixture_compiler_id",
    ).replace(
        "host_machine.system()",
        "strict_fp_fixture_system",
    )


def _meson_list(values: list[str]) -> str:
    return "[" + ", ".join(repr(value) for value in values) + "]"


def _meson_setup(case: unittest.TestCase, name: str, fixture: str) -> None:
    """Configure a throwaway Meson project; its assert() calls are the checks."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "meson.build").write_text(fixture, encoding="utf-8")
        result = subprocess.run(  # noqa: S603 -- Meson is resolved locally.
            [*MESON_COMMAND, "setup", str(root / "build"), str(root)],
            capture_output=True,
            check=False,
            encoding="utf-8",
        )
        case.assertEqual(
            result.returncode,
            0,
            msg=f"{name}:\n{result.stdout}\n{result.stderr}",
        )


def _target_block(source: str, target: str) -> str:
    start = source.index(f"{target} = static_library(")
    definition = source.index("static_library(", start)
    next_target = source.find("static_library(", definition + len("static_library("))
    return source[start : len(source) if next_target == -1 else next_target]


class StrictFpCompilerArgsTest(unittest.TestCase):
    @unittest.skipUnless(MESON_COMMAND, "Meson is not installed")
    def test_compiler_matrix_executes_shipped_policy(self) -> None:
        policy = _policy_block()
        for compiler_id, (
            system,
            model_args,
            strict_args,
            cuda_host_args,
        ) in COMPILER_MATRIX.items():
            with self.subTest(compiler_id=compiler_id):
                fixture = textwrap.dedent(
                    f"""\
                    project('strict-fp-args-{compiler_id}', 'c')
                    strict_fp_fixture_compiler_id = '{compiler_id}'
                    strict_fp_fixture_system = '{system}'

                    {policy}

                    assert(vmaf_fp_model_args == {_meson_list(model_args)},
                           'wrong general FP model arguments for {compiler_id}')
                    assert(vmaf_strict_fp_args == {_meson_list(strict_args)},
                           'wrong strict FP arguments for {compiler_id}')
                    assert(vmaf_cuda_host_strict_fp_args == {_meson_list(cuda_host_args)},
                           'wrong CUDA host FP arguments for {compiler_id}')
                    """
                )
                _meson_setup(self, compiler_id, fixture)

    def test_all_strict_consumers_use_shared_policy(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        for target in STRICT_TARGETS:
            with self.subTest(target=target):
                block = _target_block(source, target)
                self.assertIn("vmaf_strict_fp_args", block)
                self.assertNotIn("['-ffp-contract=off']", block)

        tests = TEST_MESON.read_text(encoding="utf-8")
        self.assertIn("_simd_strict_fp_args = vmaf_strict_fp_args", tests)
        self.assertIn(
            "'ssimulacra2_blur' : vmaf_cuda_host_strict_fp_args + ['--fmad=false']",
            source,
        )
        self.assertIn(
            "'ssimulacra2_device' : vmaf_cuda_host_strict_fp_args + ['--fmad=false']",
            source,
        )
        self.assertIn(
            "'float_adm_score' : vmaf_cuda_host_strict_fp_args + ['--fmad=false']",
            source,
        )

    @unittest.skipUnless(MESON_COMMAND, "Meson is not installed")
    def test_sycl_policy_executes_per_toolchain(self) -> None:
        policy = _marked_block(SYCL_POLICY_BEGIN, SYCL_POLICY_END, "SYCL strict-FP")
        for toolchain, (is_acpp, strict_args, prec_args) in SYCL_MATRIX.items():
            with self.subTest(toolchain=toolchain):
                fixture = "\n".join(
                    (
                        f"project('sycl-strict-fp-{toolchain}', 'c')",
                        f"is_sycl_acpp = {'true' if is_acpp else 'false'}",
                        policy,
                        f"assert(sycl_strict_fp_args == {_meson_list(strict_args)},",
                        f"       'wrong SYCL strict FP arguments for {toolchain}')",
                        f"assert(sycl_fp32_prec_args == {_meson_list(prec_args)},",
                        f"       'wrong SYCL fp32 precision arguments for {toolchain}')",
                        "",
                    )
                )
                _meson_setup(self, toolchain, fixture)

    def test_sycl_feature_tus_and_link_share_policy(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        # Every feature TU takes the policy through the shared tail, and each
        # link that generates device images carries the precision pair.
        self.assertEqual(source.count(SYCL_FEATURE_TAIL), 1)
        self.assertEqual(source.count(SYCL_LINK_ARGS), 1)
        self.assertEqual(source.count("link_args : sycl_link_args"), 1)
        self.assertLess(source.index(SYCL_POLICY_END), source.index(SYCL_LINK_ARGS))
        self.assertEqual(source.count(SYCL_DEVICE_LINK), 1)
        self.assertEqual(TEST_MESON.read_text(encoding="utf-8").count(SYCL_DEVICE_LINK), 1)
        # One definition site: the icpx and acpp branches inside the markers.
        policy = _marked_block(SYCL_POLICY_BEGIN, SYCL_POLICY_END, "SYCL strict-FP")
        self.assertEqual(source.count("sycl_strict_fp_args = "), 2)
        self.assertEqual(policy.count("sycl_strict_fp_args = "), 2)
        # No subset list beside it (ADR-1358 / ADR-1363 names folded into it).
        for retired in ("sycl_exact_fp_args", "sycl_exact_fp_sources", "sycl_speed_strict_fp_args"):
            self.assertNotIn(retired, source)


if __name__ == "__main__":
    unittest.main()
