#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Lock strict floating-point flags to each compiler's argument syntax.

Covers the host policy (C / SIMD / CUDA host), since ADR-1367 the SYCL device
policy (the one flag line every SYCL feature TU and the SYCL link get), and
since ADR-1403 the CUDA device policy: the one flag list every CUDA fatbin
gets, with no per-kernel copy or exception.
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
CUDA_POLICY_BEGIN = "# BEGIN VMAF CUDA device strict FP policy"
CUDA_POLICY_END = "# END VMAF CUDA device strict FP policy"

# ADR-1403: device compiler -> (enable_nvcc, host strict args, the list every
# fatbin takes). nvcc: its host pass on the C library's model, then no FMA
# contraction on the device. clang's CUDA driver has no --fmad and no
# -Xcompiler; -ffp-contract=off covers its device code.
CUDA_DEVICE_MATRIX = {
    "nvcc": (
        True,
        ["-Xcompiler=-ffp-contract=off"],
        ["-Xcompiler=-ffp-contract=off", "--fmad=false"],
    ),
    "nvcc-windows": (
        True,
        ["-Xcompiler=/fp:precise"],
        ["-Xcompiler=/fp:precise", "--fmad=false"],
    ),
    "clang": (False, ["-Xcompiler=-ffp-contract=off"], ["-ffp-contract=off"]),
}
# The fatbin custom_target appends the shared list to every kernel's command.
CUDA_FATBIN_COMMAND = (
    "] + nvcc_thread_flags + cuda_flags + cuda_device_strict_fp_args + nvcc_ccbin_flags"
)
CUDA_FP_FLAG_WORDS = ("fmad", "ffp-contract", "fp:", "fast_math", "fast-math", "ffast")

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
    "x86_avx2_static_lib",
    "x86_ssim_avx2_lib",
    "x86_psnr_hvs_avx2_lib",
    "x86_ms_ssim_decimate_avx2_lib",
    "x86_ssimulacra2_avx2_lib",
    "x86_float_adm_avx2_lib",
    "x86_speed_matmul_avx2_lib",
    "x86_avx512_static_lib",
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


def _meson_code(source: str) -> str:
    """The Meson source without its comments, so prose cannot satisfy a check."""
    return "\n".join(line.split("#", 1)[0] for line in source.splitlines())


def _cuda_device_policy_failures(source: str) -> list[str]:
    """ADR-1403: one FP flag list for every CUDA fatbin, defined once."""
    failures: list[str] = []
    try:
        policy = _meson_code(
            source[source.index(CUDA_POLICY_BEGIN) : source.index(CUDA_POLICY_END)]
        )
    except ValueError:
        return ["the CUDA device strict FP policy markers are missing"]
    code = _meson_code(source)
    if code.count("cuda_device_strict_fp_args = ") != 2 or (
        policy.count("cuda_device_strict_fp_args = ") != 2
    ):
        failures.append("cuda_device_strict_fp_args is defined outside its policy block")
    if code.count("'--fmad=false'") != 1 or policy.count("'--fmad=false'") != 1:
        failures.append("--fmad=false is spelled outside the shared CUDA device list")
    if "--fmad=true" in code:
        failures.append("a CUDA kernel re-enables FMA contraction")
    if code.count(CUDA_FATBIN_COMMAND) != 1:
        failures.append("the fatbin command does not take cuda_device_strict_fp_args")
    extra = code[code.index("cuda_cu_extra_flags = {") :]
    extra = extra[: extra.index("}") + 1]
    for word in CUDA_FP_FLAG_WORDS:
        if word in extra:
            failures.append(f"cuda_cu_extra_flags carries a per-kernel FP flag ({word})")
    return failures


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
    """The `target = static_library(...)` call, up to its closing parenthesis.

    Not "up to the next static_library(": the last x86 library is followed by
    the whole CUDA section, whose own flag lists are not this target's.
    """
    start = source.index(f"{target} = static_library(")
    depth = 0
    for position in range(source.index("(", start), len(source)):
        depth += {"(": 1, ")": -1}.get(source[position], 0)
        if depth == 0:
            return source[start : position + 1]
    raise AssertionError(f"{target}: unbalanced static_library() call")


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

    @unittest.skipUnless(MESON_COMMAND, "Meson is not installed")
    def test_cuda_device_policy_executes_per_compiler(self) -> None:
        policy = _marked_block(CUDA_POLICY_BEGIN, CUDA_POLICY_END, "CUDA device strict-FP")
        policy = policy.replace("get_option('enable_nvcc')", "cuda_fixture_enable_nvcc")
        self.assertNotIn("get_option(", _meson_code(policy))
        for compiler, (enable_nvcc, host_args, device_args) in CUDA_DEVICE_MATRIX.items():
            with self.subTest(compiler=compiler):
                fixture = "\n".join(
                    (
                        f"project('cuda-device-strict-fp-{compiler}', 'c')",
                        f"cuda_fixture_enable_nvcc = {'true' if enable_nvcc else 'false'}",
                        f"vmaf_cuda_host_strict_fp_args = {_meson_list(host_args)}",
                        policy,
                        f"assert(cuda_device_strict_fp_args == {_meson_list(device_args)},",
                        f"       'wrong CUDA device strict FP arguments for {compiler}')",
                        "",
                    )
                )
                _meson_setup(self, compiler, fixture)

    def test_every_cuda_fatbin_takes_the_shared_device_policy(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        self.assertEqual(_cuda_device_policy_failures(source), [])
        # The policy is defined before the fatbin targets that consume it.
        self.assertLess(source.index(CUDA_POLICY_END), source.index(CUDA_FATBIN_COMMAND))

    def test_clang_cuda_branch_defines_every_fatbin_flag_list(self) -> None:
        # The fatbin command concatenates lists that only the nvcc branch used
        # to assign, so -Denable_nvcc=false failed at configure time.
        code = _meson_code(SOURCE_MESON.read_text(encoding="utf-8"))
        nvcc_branch = code.index("if get_option('enable_nvcc')")
        clang_branch = code.index("nvcc_exe = find_program('clang')", nvcc_branch)
        clang_end = code.index("message('CUDA gencode", clang_branch)
        for name in ("nvcc_thread_flags", "nvcc_ccbin_flags", "nvcc_host_includes"):
            with self.subTest(name=name):
                self.assertIn(f"{name} = ", code[nvcc_branch:clang_branch])
                self.assertIn(f"{name} = []", code[clang_branch:clang_end])
                self.assertIn(name, code[code.index("foreach name, _cu : cuda_cu_sources") :])

    def test_per_kernel_cuda_fp_flag_is_detected(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        for planted in (
            "cuda_cu_extra_flags = {'psnr_score' : ['--fmad=true']}",
            "cuda_cu_extra_flags = {'speed_score' : ['--fmad=false']}",
            "cuda_cu_extra_flags = {'ciede_score' : ['--use_fast_math']}",
        ):
            with self.subTest(planted=planted):
                edited = source.replace("cuda_cu_extra_flags = {}", planted, 1)
                self.assertNotEqual(edited, source)
                self.assertNotEqual(_cuda_device_policy_failures(edited), [])

    def test_fatbin_without_the_shared_cuda_policy_is_detected(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        edited = source.replace("cuda_flags + cuda_device_strict_fp_args +", "cuda_flags +", 1)
        self.assertNotEqual(edited, source)
        failures = _cuda_device_policy_failures(edited)
        self.assertTrue(any("fatbin command" in item for item in failures), failures)

    def test_second_cuda_device_policy_definition_is_detected(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        edited = source.replace(
            CUDA_POLICY_END, CUDA_POLICY_END + "\n    cuda_device_strict_fp_args = []", 1
        )
        failures = _cuda_device_policy_failures(edited)
        self.assertTrue(any("outside its policy block" in item for item in failures), failures)

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
