#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Lock strict floating-point flags to each compiler's argument syntax.

Covers the host policy (C / SIMD / CUDA host), since ADR-1367 the SYCL device
policy (the one flag line every SYCL feature TU and the SYCL link get), since
ADR-1403 the CUDA device policy: the one flag list every CUDA fatbin gets,
with no per-kernel copy or exception, and since ADR-1461 the project-wide
floor: the host policy is a project argument, no target turns contraction back
on after it, and the compile commands of the build this test runs in end on
the strict flag for every C and C++ translation unit.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SOURCE_MESON = Path(__file__).resolve().parents[1] / "src" / "meson.build"
TEST_MESON = Path(__file__).resolve().with_name("meson.build")
TOOLS_MESON = Path(__file__).resolve().parents[1] / "tools" / "meson.build"
METAL_MESON = Path(__file__).resolve().parents[1] / "src" / "metal" / "meson.build"
POLICY_BEGIN = "# BEGIN VMAF strict FP compiler-argument policy"
POLICY_END = "# END VMAF strict FP compiler-argument policy"
SYCL_POLICY_BEGIN = "# BEGIN VMAF SYCL strict FP policy"
SYCL_POLICY_END = "# END VMAF SYCL strict FP policy"
# The policy block's last statement: _meson_code() strips the END marker.
POLICY_END_CODE = "vmaf_strict_fp_args = ['/clang:-ffp-contract=off']"
CUDA_POLICY_BEGIN = "# BEGIN VMAF CUDA device strict FP policy"
CUDA_POLICY_END = "# END VMAF CUDA device strict FP policy"
LIBM_POLICY_BEGIN = "# BEGIN VMAF host math library link policy"
LIBM_POLICY_END = "# END VMAF host math library link policy"
LIBM_LINK_ARGUMENTS = (
    "add_project_link_arguments(vmaf_c_host_libm_link_args, language : 'c')",
    "add_project_link_arguments(vmaf_cpp_host_libm_link_args, language : 'cpp')",
)
# ADR-1495: (C compiler id, C++ compiler id) -> (C link args, C++ link args).
# Each language's own compiler decides: Meson applies project link arguments
# by the language a target links with. Windows icx-cl is not covered.
NO_IMF = ["-no-intel-lib=libimf"]
LIBM_MATRIX: dict[tuple[str, str], tuple[list[str], list[str]]] = {
    ("intel-llvm", "intel-llvm"): (NO_IMF, NO_IMF),
    ("gcc", "intel-llvm"): ([], NO_IMF),
    ("intel-llvm", "gcc"): (NO_IMF, []),
    ("gcc", "gcc"): ([], []),
    ("clang", "clang"): ([], []),
    ("intel-llvm-cl", "intel-llvm-cl"): ([], []),
    ("msvc", "msvc"): ([], []),
}

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
CUDA_FATBIN_COMMAND = "] + nvcc_thread_flags + nvcc_werror_flags + cuda_flags + cuda_device_strict_fp_args + nvcc_ccbin_flags"
CUDA_FP_FLAG_WORDS = ("fmad", "ffp-contract", "fp:", "fast_math", "fast-math", "ffast")

# ADR-1367: toolchain -> (is_sycl_acpp, sycl_strict_fp_args, sycl_fp32_prec_args).
# icpx: fast model off, then contraction off (the order is load-bearing:
# -fp-model=precise implies -ffp-contract=on), then correctly rounded fp32
# division and square root. AdaptiveCpp accepts only contraction-off.
SYCL_PREC = ["-foffload-fp32-prec-div", "-foffload-fp32-prec-sqrt"]
# ADR-2170: `-fno-fast-math -fcomplex-arithmetic=full` sit between the model and the contraction
# flag, so icpx does not report `-ffp-contract=off` as overriding `-fp-model=precise`.
ICX_RESET = ["-fno-fast-math", "-fcomplex-arithmetic=full"]
SYCL_MATRIX = {
    "icpx": (False, ["-fp-model=precise", *ICX_RESET, "-ffp-contract=off", *SYCL_PREC], SYCL_PREC),
    "acpp": (True, ["-ffp-contract=off"], []),
}
# The MSVC build (ADR-1364) compiles and device-links its SYCL TUs with icpx too, so it
# takes the same reset (ADR-2170). Toolchain -> sycl_strict_fp_args.
SYCL_MSVC_DRIVER = {
    "icx-cl": ["-fp-model=precise", *ICX_RESET, "-ffp-contract=off", *SYCL_PREC],
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
        ["-fp-model=precise", *ICX_RESET, "-ffp-contract=off"],
        ["-Xcompiler=-ffp-contract=off"],
    ),
    "msvc": ("windows", [], ["/fp:precise"], ["-Xcompiler=/fp:precise"]),
    "intel-llvm-cl": (
        "windows",
        ["/fp:precise"],
        [
            "/fp:precise",
            "/clang:-fno-fast-math",
            "/clang:-fcomplex-arithmetic=full",
            "/clang:-ffp-contract=off",
        ],
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


# ADR-1461: the host policy applied to every C and C++ translation unit.
PROJECT_ARGUMENT = "add_project_arguments(vmaf_strict_fp_args, language : ['c', 'cpp'])"
FIRST_TARGET = re.compile(
    r"\b(?:static_library|shared_library|both_libraries|library|executable)\("
)
# Anything in a target's own list that would decide contraction after the
# project argument. vmaf_fp_model_args is on it because icx's
# `-fp-model=precise` implies -ffp-contract=on.
CONTRACTION_WORDS = (
    "vmaf_fp_model_args",
    "-ffp-contract=on",
    "-ffp-contract=fast",
    "-ffast-math",
    "-Ofast",
    "-fp-model=fast",
    "/fp:fast",
    "/fp:contract",
    "/Qfma'",
)
# Every flag on a compile command that sets the contraction state, in order.
FP_FLAG = re.compile(
    r"(?<![\w/=.-])(/clang:-ffp-contract=\w+|-ffp-contract=\w+|-fp-model[= ]\w+"
    r"|-ffast-math|-Ofast|/fp:\w+|/Qfma-?)(?![\w-])"
)
# What a strict command must end its FP flags with, per compiler id. MSVC's
# /fp:precise is also its default, so a command without any FP flag would
# still be strict there; it is required anyway, the policy is explicit.
STRICT_LAST_FLAG = {
    "gcc": "-ffp-contract=off",
    "clang": "-ffp-contract=off",
    "intel-llvm": "-ffp-contract=off",
    "msvc": "/fp:precise",
    "intel-llvm-cl": "/clang:-ffp-contract=off",
    "clang-cl": "/clang:-ffp-contract=off",
}
C_FAMILY_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")
# Set by the test() call in core/test/meson.build.
BUILD_ROOT_ENV = "VMAF_STRICT_FP_BUILD_ROOT"


POLICY_DEFINITIONS = 2  # definitions of cuda_device_strict_fp_args (clang and nvcc spelling)


def _project_floor_failures(source: str, tests: str, tools: str, metal: str) -> list[str]:
    """ADR-1461: one project argument, above the first target, never undone."""
    failures: list[str] = []
    code = _meson_code(source)
    if code.count(PROJECT_ARGUMENT) != 1:
        return ["the strict policy is not the project argument for C and C++, exactly once"]
    floor = code.index(PROJECT_ARGUMENT)
    if floor < code.index(POLICY_END_CODE):
        failures.append("the project argument precedes the policy that defines it")
    first_target = FIRST_TARGET.search(code)
    if first_target is None or first_target.start() < floor:
        failures.append("a build target is declared above the project argument")
    policy_end = code.index(POLICY_END_CODE)
    after = {
        "core/src/meson.build": code[policy_end:],
        "core/test/meson.build": _meson_code(tests),
        "core/tools/meson.build": _meson_code(tools),
    }
    for name, text in after.items():
        for word in CONTRACTION_WORDS:
            if word in text:
                failures.append(f"{name} names {word} outside the policy block")
    if "] + vmaf_strict_fp_args" not in _meson_code(metal):
        failures.append("the Metal Obj-C++ arguments do not take vmaf_strict_fp_args")
    return failures


def _command_fp_failure(command: str, compiler_id: str) -> str:
    """Why `command` is not a strict compile command, or the empty string."""
    wanted = STRICT_LAST_FLAG[compiler_id]
    flags = [match.group(1) for match in FP_FLAG.finditer(command)]
    if wanted not in flags:
        return f"{wanted} is missing"
    if flags[-1] != wanted:
        return f"{flags[-1]} follows {wanted}"
    return ""


def _build_fp_failures(build_root: Path, source_root: Path) -> list[str] | None:
    """Check the compile database of a configured build; None when there is none."""
    database = build_root / "compile_commands.json"
    compilers = build_root / "meson-info" / "intro-compilers.json"
    if not database.is_file() or not compilers.is_file():
        return None
    compiler_id = json.loads(compilers.read_text(encoding="utf-8"))["host"]["c"]["id"]
    if compiler_id not in STRICT_LAST_FLAG:
        return None
    failures: list[str] = []
    checked = 0
    for entry in json.loads(database.read_text(encoding="utf-8")):
        source = Path(entry["directory"], entry["file"]).resolve()
        if source.suffix not in C_FAMILY_SUFFIXES or "subprojects" in source.parts:
            continue
        command = entry.get("command") or " ".join(entry.get("arguments", []))
        checked += 1
        why = _command_fp_failure(command, compiler_id)
        if why:
            try:
                shown = source.relative_to(source_root)
            except ValueError:
                shown = source
            failures.append(f"{shown}: {why}")
    if checked == 0:
        failures.append("the compile database holds no C or C++ translation unit")
    return failures


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
    if code.count("cuda_device_strict_fp_args = ") != POLICY_DEFINITIONS or (
        policy.count("cuda_device_strict_fp_args = ") != POLICY_DEFINITIONS
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
                fixture = textwrap.dedent(f"""\
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
                    """)
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

    def test_strict_policy_is_the_project_floor(self) -> None:
        self.assertEqual(self._floor_failures(SOURCE_MESON.read_text(encoding="utf-8")), [])

    def _floor_failures(self, source: str, tests: str | None = None) -> list[str]:
        return _project_floor_failures(
            source,
            TEST_MESON.read_text(encoding="utf-8") if tests is None else tests,
            TOOLS_MESON.read_text(encoding="utf-8"),
            METAL_MESON.read_text(encoding="utf-8"),
        )

    def test_missing_or_misplaced_project_floor_is_detected(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        removed = source.replace(PROJECT_ARGUMENT, "", 1)
        self.assertTrue(any("exactly once" in item for item in self._floor_failures(removed)))
        # Moved below the first library: Meson would refuse it at configure
        # time, and before that this check does.
        first = FIRST_TARGET.search(_meson_code(removed))
        assert first is not None
        anchor = _meson_code(removed)[first.start() : first.start() + 40]
        position = removed.index(anchor) + len(anchor)
        late = removed[:position] + "\n" + PROJECT_ARGUMENT + "\n" + removed[position:]
        self.assertTrue(
            any("above the project argument" in item for item in self._floor_failures(late))
        )

    def test_target_that_undoes_the_floor_is_detected(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        tests = TEST_MESON.read_text(encoding="utf-8")
        # The pre-ADR-1461 feature library: the model after the common flags.
        planted = source.replace(
            "    c_args : vmaf_cflags_common,\n    cpp_args : vmaf_cppflags_common,\n",
            "    c_args : vmaf_cflags_common + vmaf_fp_model_args,\n"
            "    cpp_args : vmaf_cppflags_common,\n",
            1,
        )
        self.assertNotEqual(planted, source)
        self.assertTrue(any("vmaf_fp_model_args" in item for item in self._floor_failures(planted)))
        for word in ("'-ffp-contract=fast'", "'-ffast-math'", "'/fp:fast'"):
            with self.subTest(word=word):
                edited = tests + f"\nplanted_args = [{word}]\n"
                failures = self._floor_failures(source, edited)
                self.assertTrue(any("core/test/meson.build" in item for item in failures), failures)
        # A comment naming the words is prose, not a flag.
        self.assertEqual(self._floor_failures(source, tests + "\n# -ffast-math\n"), [])

    def test_compile_command_fp_flags_are_read_in_order(self) -> None:
        base = "cc -Isrc -std=c23 -O3 -D_GNU_SOURCE {} -o x.o -c x.c"
        # positive: the flag alone, twice, and after an Intel model
        for compiler_id, flags in (
            ("gcc", "-ffp-contract=off"),
            ("clang", "-ffp-contract=off -mavx2 -mfma -ffp-contract=off"),
            (
                "intel-llvm",
                "-fp-model=precise -fno-fast-math -fcomplex-arithmetic=full -ffp-contract=off",
            ),
            ("msvc", "/fp:precise"),
            (
                "intel-llvm-cl",
                "/fp:precise /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full"
                " /clang:-ffp-contract=off",
            ),
            ("clang-cl", "/clang:-ffp-contract=off"),
        ):
            with self.subTest(compiler_id=compiler_id, flags=flags):
                self.assertEqual(_command_fp_failure(base.format(flags), compiler_id), "")
        # negative: absent, undone by a later flag, and the icx order hazard
        for compiler_id, flags, why in (
            ("gcc", "", "is missing"),
            ("clang", "-ffp-contract=off -ffp-contract=fast", "follows"),
            ("clang", "-ffp-contract=off -ffast-math", "follows"),
            ("intel-llvm", "-fp-model=precise -ffp-contract=off -fp-model=precise", "follows"),
            ("intel-llvm-cl", "/clang:-ffp-contract=off /fp:precise", "follows"),
            ("intel-llvm-cl", "/fp:precise /Qfma-", "is missing"),
            ("clang-cl", "-ffp-contract=off", "is missing"),
        ):
            with self.subTest(compiler_id=compiler_id, flags=flags):
                self.assertIn(why, _command_fp_failure(base.format(flags), compiler_id))
        # boundary: a path or a define that only contains a flag's spelling
        self.assertIn(
            "is missing",
            _command_fp_failure(
                base.format("-DNOTE=x-ffp-contract=off -Idir/-ffp-contract=off"), "gcc"
            ),
        )

    def test_this_build_compiles_every_c_and_cpp_unit_without_contraction(self) -> None:
        # core/test/meson.build hands the build directory to the test.
        build_root = os.environ.get(BUILD_ROOT_ENV)
        if not build_root:
            self.skipTest(f"{BUILD_ROOT_ENV} is not set: no build directory to read")
        failures = _build_fp_failures(Path(build_root), SOURCE_MESON.parents[1])
        # Told where the build is, the test must find something to check.
        self.assertIsNotNone(failures, f"no compile database for a known compiler in {build_root}")
        assert failures is not None
        self.assertEqual(failures[:20], [], f"{len(failures)} translation units")

    def test_meson_hands_the_build_directory_to_this_test(self) -> None:
        tests = _meson_code(TEST_MESON.read_text(encoding="utf-8"))
        registration = tests[tests.index("test('test_strict_fp_compiler_args'") :]
        registration = registration[: registration.index("\n)") + 2]
        self.assertIn(f"'{BUILD_ROOT_ENV}=' + meson.project_build_root()", registration)

    @unittest.skipUnless(MESON_COMMAND, "Meson is not installed")
    def test_host_libm_link_policy_executes_per_compiler_pair(self) -> None:
        policy = _marked_block(LIBM_POLICY_BEGIN, LIBM_POLICY_END, "host math library link")
        policy = policy.replace("cc.get_id()", "libm_fixture_c_id").replace(
            "cxx.get_id()", "libm_fixture_cpp_id"
        )
        for (c_id, cpp_id), (c_args, cpp_args) in LIBM_MATRIX.items():
            with self.subTest(c_id=c_id, cpp_id=cpp_id):
                fixture = "\n".join(
                    (
                        f"project('host-libm-{c_id}-{cpp_id}', 'c')",
                        f"libm_fixture_c_id = '{c_id}'",
                        f"libm_fixture_cpp_id = '{cpp_id}'",
                        policy,
                        f"assert(vmaf_c_host_libm_link_args == {_meson_list(c_args)},",
                        f"       'wrong C host libm link arguments for {c_id}')",
                        f"assert(vmaf_cpp_host_libm_link_args == {_meson_list(cpp_args)},",
                        f"       'wrong C++ host libm link arguments for {cpp_id}')",
                        "",
                    )
                )
                _meson_setup(self, f"{c_id}/{cpp_id}", fixture)

    def test_host_libm_link_policy_reaches_every_link(self) -> None:
        # ADR-1495: project link arguments, once each, after the policy and
        # above the first target (Meson refuses them after one).
        code = _meson_code(SOURCE_MESON.read_text(encoding="utf-8"))
        first_target = FIRST_TARGET.search(code)
        assert first_target is not None
        policy_end = code.index("vmaf_cpp_host_libm_link_args = ['-no-intel-lib=libimf']")
        for line in LIBM_LINK_ARGUMENTS:
            with self.subTest(line=line):
                self.assertEqual(code.count(line), 1)
                self.assertLess(policy_end, code.index(line))
                self.assertLess(code.index(line), first_target.start())
        # No target or test links Intel's math library back in by name.
        tests = _meson_code(TEST_MESON.read_text(encoding="utf-8"))
        tools = _meson_code(TOOLS_MESON.read_text(encoding="utf-8"))
        for name, text in (("src", code), ("test", tests), ("tools", tools)):
            with self.subTest(file=f"core/{name}/meson.build"):
                self.assertNotIn("-limf", text)
                self.assertNotIn("find_library('imf'", text)

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
                        "sycl_msvc_device_link = false",
                        policy,
                        f"assert(sycl_strict_fp_args == {_meson_list(strict_args)},",
                        f"       'wrong SYCL strict FP arguments for {toolchain}')",
                        f"assert(sycl_fp32_prec_args == {_meson_list(prec_args)},",
                        f"       'wrong SYCL fp32 precision arguments for {toolchain}')",
                        "",
                    )
                )
                _meson_setup(self, toolchain, fixture)
        for toolchain, strict_args in SYCL_MSVC_DRIVER.items():
            with self.subTest(toolchain=toolchain):
                fixture = "\n".join(
                    (
                        f"project('sycl-strict-fp-{toolchain}', 'c')",
                        "is_sycl_acpp = false",
                        "sycl_msvc_device_link = true",
                        policy,
                        f"assert(sycl_strict_fp_args == {_meson_list(strict_args)},",
                        f"       'wrong SYCL strict FP arguments for {toolchain}')",
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
        # The tests' probes (ADR-1367, ADR-1422, ADR-1432, ADR-1436) link their
        # own device images, one per probe: every such link carries the policy
        # too. No count is pinned, so a new probe needs no edit here.
        tests = TEST_MESON.read_text(encoding="utf-8")
        self.assertGreaterEqual(tests.count(SYCL_DEVICE_LINK), 2)
        self.assertEqual(tests.count("sycl_device_link_args"), tests.count(SYCL_DEVICE_LINK))
        # One definition site: the icpx and acpp branches inside the markers.
        policy = _marked_block(SYCL_POLICY_BEGIN, SYCL_POLICY_END, "SYCL strict-FP")
        self.assertEqual(source.count("sycl_strict_fp_args = "), 2)
        self.assertEqual(policy.count("sycl_strict_fp_args = "), 2)
        # No subset list beside it (ADR-1358 / ADR-1363 names folded into it).
        for retired in ("sycl_exact_fp_args", "sycl_exact_fp_sources", "sycl_speed_strict_fp_args"):
            self.assertNotIn(retired, source)

    @unittest.skipUnless(shutil.which("icx"), "icx is not on PATH (source oneAPI setvars.sh)")
    def test_icx_strict_policy_draws_no_overriding_option_warning(self) -> None:
        """ADR-2170: the strict spelling compiles clean under -Werror; the old one does not."""
        icx = shutil.which("icx")
        assert icx is not None
        strict = COMPILER_MATRIX["intel-llvm"][2]
        old = ["-fp-model=precise", "-ffp-contract=off"]
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "probe.c"
            src.write_text("float f(float a, float b, float c) { return a * b + c; }\n")
            obj = Path(tmp) / "probe.o"

            def compile_with(flags: list[str]) -> subprocess.CompletedProcess[str]:
                return subprocess.run(  # noqa: S603 -- icx is resolved on PATH above.
                    [icx, "-O2", "-Werror", *flags, "-c", str(src), "-o", str(obj)],
                    capture_output=True,
                    text=True,
                    check=False,
                )

            ok = compile_with(strict)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            # Negative case: the check can fail. The two-flag spelling is reported as an override.
            bad = compile_with(old)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("-Woverriding-option", bad.stderr)
            # No contraction: a * b + c is two instructions with the strict spelling.
            disasm = subprocess.run(  # noqa: S603 -- icx is resolved on PATH above.
                [icx, "-O2", "-mavx2", "-mfma", *strict, "-S", "-o", "-", str(src)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(disasm.returncode, 0, disasm.stderr)
            self.assertNotIn("vfmadd", disasm.stdout)


if __name__ == "__main__":
    unittest.main()
