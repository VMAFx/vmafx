#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Lock strict floating-point flags to each compiler's argument syntax."""

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


def _policy_block() -> str:
    source = SOURCE_MESON.read_text(encoding="utf-8")
    try:
        start = source.index(POLICY_BEGIN)
        end = source.index(POLICY_END, start) + len(POLICY_END)
    except ValueError as exc:
        raise AssertionError("strict-FP compiler policy markers are missing") from exc
    block = source[start:end]
    return block.replace(
        "_strict_fp_compiler_id = cc.get_id()",
        "_strict_fp_compiler_id = strict_fp_fixture_compiler_id",
    ).replace(
        "host_machine.system()",
        "strict_fp_fixture_system",
    )


def _meson_list(values: list[str]) -> str:
    return "[" + ", ".join(repr(value) for value in values) + "]"


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
            with self.subTest(compiler_id=compiler_id), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
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
                (root / "meson.build").write_text(fixture, encoding="utf-8")
                result = subprocess.run(  # noqa: S603 -- Meson is resolved locally.
                    [*MESON_COMMAND, "setup", str(root / "build"), str(root)],
                    capture_output=True,
                    check=False,
                    encoding="utf-8",
                )
                self.assertEqual(
                    result.returncode,
                    0,
                    msg=f"{compiler_id}:\n{result.stdout}\n{result.stderr}",
                )

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
            "'float_adm_score' : vmaf_cuda_host_strict_fp_args + ['--fmad=false']",
            source,
        )


if __name__ == "__main__":
    unittest.main()
