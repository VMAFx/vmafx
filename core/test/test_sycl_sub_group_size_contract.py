#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A SYCL kernel may only require a sub-group size every AOT target accepts (ADR-1468).

The default build compiles every kernel ahead of time for the 19 targets of
``sycl_icpx_aot_targets``. Xe2 targets (lnl-m, bmg-g21, bmg-g31) accept the
sub-group sizes 16 and 32 and not 8, so one kernel that requires 8 fails its
translation unit for them, and the dev container image did not build. Lane
builds compile for one device or for none, so nothing local saw it.

Device-free and compiler-free. It holds:

- the default target list to targets whose accepted sizes are known
  (``sycl_aot_targets.SIZES_BY_FAMILY``, measured with ocloc);
- ``sycl_compat.h``'s compile-time check (``VmafSyclSubGroupSize``) to the
  sizes every default target accepts;
- every size a SYCL source requires, through the fork's macros and kernel
  shape or as a named constant (of the file, or a namespace-qualified one of
  any SYCL source), to that set; a kernel shape may require none (size 0)
  only with the large register file (ADR-1501);
- the sources to the fork's macros: a raw attribute or property would pass
  the header's check by.

``test_sycl_aot_default_targets.py`` (suite ``sycl-aot``) compiles every
translation unit for the full list with ocloc.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import sycl_aot_targets as aot
import test_sycl_aot_default_targets as aot_build

ROOT = Path(__file__).resolve().parents[2]
COMPAT = ROOT / "core" / "src" / "feature" / "sycl" / "sycl_compat.h"
SOURCE_GLOBS = (
    "core/src/sycl/*.cpp",
    "core/src/sycl/*.h",
    "core/src/feature/sycl/*.cpp",
    "core/src/feature/sycl/*.h",
    "core/test/*sycl*.cpp",
    "core/test/*sycl*.h",
)

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A literal, a constant or a namespace-qualified constant.
NAME = r"([A-Za-z_0-9]+(?:::[A-Za-z_][A-Za-z_0-9]*)*)"
# The fork's three spellings of a required sub-group size: two macros and
# the kernel shape, whose second argument is the register file size.
MACROS = (
    re.compile(rf"\bVMAF_SYCL_REQD_SG_SIZE\(\s*{NAME}\s*\)"),
    re.compile(rf"\bVMAF_SYCL_FUNCTOR_SG_SIZE\(\s*{NAME}\s*\)"),
)
SHAPE = re.compile(rf"\bVmafSyclKernelShape<\s*{NAME}\s*,\s*{NAME}\s*>")
REQUESTS = (*MACROS, SHAPE)
# What the macros expand to: outside sycl_compat.h it would skip the check.
RAW = re.compile(r"reqd_sub_group_size\s*\(|\bsub_group_size\s*<")
ASSERTED = re.compile(r"static_assert\(\s*N == (\d+) \|\| N == (\d+)\s*,")


def _code(text: str) -> str:
    return COMMENT.sub(" ", text)


def _sources() -> dict[str, str]:
    found: dict[str, str] = {}
    for pattern in SOURCE_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            found[path.relative_to(ROOT).as_posix()] = path.read_text(encoding="utf-8")
    return found


def _value(code: str, name: str, everywhere: str = "") -> int | None:
    """An integer literal, a constant of the file (of any source when the name
    is namespace-qualified), or None for a template parameter."""
    if name.isdigit():
        return int(name)
    bare = name.rsplit("::", 1)[-1]
    for text in (code, everywhere if "::" in name else ""):
        constant = re.search(rf"\bconstexpr\s+int\s+{bare}\s*=\s*(\d+)\s*;", text)
        if constant:
            return int(constant.group(1))
    if re.search(rf"\btemplate\s*<[^>]*\bint\s+{bare}\b", code):
        return None
    raise ValueError(name)


def _size_failure(name: str, size: int | None, allowed: set[int]) -> list[str]:
    if size is None or size in allowed:
        return []
    return [
        f"{name}: requires sub-group size {size}; every default AOT "
        f"target accepts only {sorted(allowed)}"
    ]


def _request_failures(name: str, code: str, everywhere: str, allowed: set[int]) -> list[str]:
    failures: list[str] = []
    for macro in MACROS:
        for spelled in macro.findall(code):
            if spelled in ("N", "SG") and name.endswith("sycl_compat.h"):
                continue  # the macros' own parameters
            failures += _size_failure(name, _value(code, spelled, everywhere), allowed)
    for spelled, grf in SHAPE.findall(code):
        size = _value(code, spelled, everywhere)
        if size == 0 and _value(code, grf, everywhere) not in (None, 256):
            failures.append(
                f"{name}: a kernel shape without a required sub-group size needs "
                "the large register file (GRF 256, ADR-1501)"
            )
        elif size != 0:
            failures += _size_failure(name, size, allowed)
    return failures


def _source_failures(sources: dict[str, str], allowed: set[int]) -> list[str]:
    failures: list[str] = []
    everywhere = "\n".join(_code(text) for text in sources.values())
    for name, text in sources.items():
        code = _code(text)
        if not name.endswith("sycl_compat.h") and RAW.search(code):
            failures.append(
                f"{name}: a raw sub-group size attribute or property; use the "
                "macros of sycl_compat.h, which check the size"
            )
        try:
            failures += _request_failures(name, code, everywhere, allowed)
        except ValueError as error:
            failures.append(f"{name}: sub-group size `{error}` is not a constant here")
    return failures


def _contract_failures(options: str, compat: str, sources: dict[str, str]) -> list[str]:
    targets = aot.default_targets(options)
    unknown = [target for target in targets if aot.supported_sizes(target) is None]
    if unknown or not targets:
        return [
            f"core/meson_options.txt: no measured sub-group sizes for {unknown or 'an empty list'}; "
            "measure with sycl_aot_targets.ocloc_accepts() and add the family"
        ]
    allowed = aot.common_sizes(targets)
    failures: list[str] = []
    asserted = ASSERTED.search(_code(compat))
    if not asserted or {int(asserted.group(1)), int(asserted.group(2))} != allowed:
        failures.append(
            f"sycl_compat.h: VmafSyclSubGroupSize must accept exactly {sorted(allowed)}, "
            "the sizes every default AOT target takes"
        )
    for piece in (
        "[[sycl::reqd_sub_group_size(VmafSyclSubGroupSize<N>::value)]]",
        "struct VmafSyclShapeSubGroup : VmafSyclSubGroupSize<SG_SIZE>",
        "static constexpr int sub_group_size = VmafSyclShapeSubGroup<SG_SIZE>::value;",
        "static_assert(SG_SIZE != 0 || GRF_SIZE == 256,",
    ):
        if piece not in compat:
            failures.append(f"sycl_compat.h: `{piece}` is gone; a size is no longer checked")
    return failures + _source_failures(sources, allowed)


def _live() -> tuple[str, str, dict[str, str]]:
    return (
        aot.OPTIONS.read_text(encoding="utf-8"),
        COMPAT.read_text(encoding="utf-8"),
        _sources(),
    )


class SyclSubGroupSizeContract(unittest.TestCase):
    def _detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(*_live()), [])

    def test_the_default_list_is_the_measured_one(self) -> None:
        targets = aot.default_targets()
        self.assertEqual(len(targets), 19)
        self.assertEqual(aot.common_sizes(targets), {16, 32})
        self.assertEqual(aot.common_sizes(["dg2-g11", "tgllp"]), {8, 16, 32})
        self.assertEqual(aot.supported_sizes("bmg-g21"), (16, 32))
        self.assertIsNone(aot.supported_sizes("pvc"))

    def test_the_scan_sees_the_kernels(self) -> None:
        # Guards the scan itself: a pattern that matched nothing would pass.
        code = "\n".join(_code(text) for text in _sources().values())
        requests = sum(len(request.findall(code)) for request in REQUESTS)
        self.assertGreater(requests, 30)

    def test_size_8_in_a_lambda_is_detected(self) -> None:
        # The kernel that stopped the container build (float_motion_sycl.cpp).
        options, compat, sources = _live()
        name = "core/src/feature/sycl/float_motion_sycl.cpp"
        self.assertIn("VMAF_SYCL_REQD_SG_SIZE(16)", sources[name])
        sources[name] = sources[name].replace(
            "VMAF_SYCL_REQD_SG_SIZE(16)", "VMAF_SYCL_REQD_SG_SIZE(8)", 1
        )
        self._detected(_contract_failures(options, compat, sources), "requires sub-group size 8")

    def test_size_8_in_a_named_constant_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/src/feature/sycl/ssimulacra2_sycl.cpp"
        self.assertIn("constexpr int SS2S_WALK_SG = 16;", sources[name])
        sources[name] = sources[name].replace(
            "constexpr int SS2S_WALK_SG = 16;", "constexpr int SS2S_WALK_SG = 8;", 1
        )
        self._detected(_contract_failures(options, compat, sources), "requires sub-group size 8")

    def test_size_8_in_a_kernel_shape_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        self.assertIn("VmafSyclKernelShape<16, 0>", sources[name])
        sources[name] = sources[name].replace(
            "VmafSyclKernelShape<16, 0>", "VmafSyclKernelShape<8, 0>", 1
        )
        self._detected(_contract_failures(options, compat, sources), "requires sub-group size 8")

    def test_qualified_constant_is_resolved(self) -> None:
        # float_adm_sycl.cpp and its probe name the shape of sycl_float_adm_math.h.
        options, compat, sources = _live()
        name = "core/src/feature/sycl/sycl_float_adm_math.h"
        self.assertIn("inline constexpr int kTermsSubGroup = 0;", sources[name])
        sources[name] = sources[name].replace(
            "inline constexpr int kTermsSubGroup = 0;",
            "inline constexpr int kTermsSubGroup = 8;",
            1,
        )
        self._detected(_contract_failures(options, compat, sources), "requires sub-group size 8")

    def test_no_size_without_the_large_register_file_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/src/feature/sycl/sycl_float_adm_math.h"
        self.assertIn("inline constexpr int kTermsGrf = 256;", sources[name])
        sources[name] = sources[name].replace(
            "inline constexpr int kTermsGrf = 256;", "inline constexpr int kTermsGrf = 0;", 1
        )
        self._detected(_contract_failures(options, compat, sources), "large register file")

    def test_unchecked_shape_size_is_detected(self) -> None:
        options, compat, sources = _live()
        compat = compat.replace("VmafSyclShapeSubGroup<SG_SIZE>::value", "SG_SIZE", 1)
        self._detected(_contract_failures(options, compat, sources), "no longer checked")

    def test_raw_attribute_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/src/feature/sycl/float_psnr_sycl.cpp"
        sources[name] += "\nstatic void k() { auto f = [] [[sycl::reqd_sub_group_size(8)]] {}; }\n"
        self._detected(_contract_failures(options, compat, sources), "raw sub-group size")

    def test_raw_property_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/src/feature/sycl/float_psnr_sycl.cpp"
        sources[name] += "\nauto p = syclex::properties{syclex::sub_group_size<8>};\n"
        self._detected(_contract_failures(options, compat, sources), "raw sub-group size")

    def test_widened_header_check_is_detected(self) -> None:
        options, compat, sources = _live()
        self.assertIn("N == 16 || N == 32", compat)
        compat = compat.replace("N == 16 || N == 32", "N == 8 || N == 16", 1)
        self._detected(_contract_failures(options, compat, sources), "must accept exactly")

    def test_unchecked_macro_is_detected(self) -> None:
        options, compat, sources = _live()
        compat = compat.replace(
            "[[sycl::reqd_sub_group_size(VmafSyclSubGroupSize<N>::value)]]",
            "[[sycl::reqd_sub_group_size(N)]]",
        )
        self._detected(_contract_failures(options, compat, sources), "no longer checked")

    def test_unmeasured_target_is_detected(self) -> None:
        options, compat, sources = _live()
        self.assertIn("lnl-m,bmg-g21", options)
        options = options.replace("lnl-m,bmg-g21", "lnl-m,xyz-9,bmg-g21", 1)
        self._detected(_contract_failures(options, compat, sources), "no measured sub-group sizes")

    def test_a_list_without_xe2_would_allow_8(self) -> None:
        # The set follows the list: it is not a constant of this test.
        options, compat, sources = _live()
        options = options.replace(",lnl-m,bmg-g21,bmg-g31", "", 1)
        self._detected(
            _contract_failures(options, compat, sources), "must accept exactly [8, 16, 32]"
        )


# A JIT-only and an ahead-of-time unit as meson writes them into build.ninja.
JIT_NINJA = """build src/float_motion_sycl.o: CUSTOM_COMMAND_DEP ../core/src/./feature/sycl/float_motion_sycl.cpp | /opt/intel/oneapi/compiler/2026.0/bin/icpx
 DEPFILE = src/float_motion_sycl.o.d
 COMMAND = /opt/icpx -DHAVE_SYCL -c -fsycl -std=c++20 -I/b/src -MD -MF src/float_motion_sycl.o.d ../core/src/./feature/sycl/float_motion_sycl.cpp -o src/float_motion_sycl.o
 description = Generating$ src/sycl_feature_float_motion_sycl

build src/x.o: CUSTOM_COMMAND ../core/src/x.cpp | /usr/bin/g++
 COMMAND = g++ -c ../core/src/x.cpp -o src/x.o
"""
AOT_NINJA = """build src/common.o: CUSTOM_COMMAND ../core/src/./sycl/common.cpp | /opt/intel/oneapi/compiler/2026.0/bin/icpx
 COMMAND = /opt/icpx -c -fsycl -fno-sycl-rdc --offload-compress -fsycl-targets=spir64_gen,spir64 -Xsycl-target-backend=spir64_gen '-device$ dg2-g11,tgllp' -std=c++20 ../core/src/./sycl/common.cpp -o src/common.o
"""


class SyclAotCommandRewrite(unittest.TestCase):
    """The build-level test's reading of build.ninja, without a compiler."""

    def test_units_are_the_icpx_statements(self) -> None:
        found = aot_build.units(JIT_NINJA)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][0], "src/float_motion_sycl.o")
        self.assertEqual(found[0][1], "../core/src/./feature/sycl/float_motion_sycl.cpp")
        self.assertEqual(aot_build.units(""), [])

    def test_configured_targets(self) -> None:
        self.assertEqual(aot_build.configured_targets(aot_build.units(JIT_NINJA)[0][2]), [])
        self.assertEqual(
            aot_build.configured_targets(aot_build.units(AOT_NINJA)[0][2]), ["dg2-g11", "tgllp"]
        )

    def test_jit_command_becomes_the_aot_command(self) -> None:
        argv = aot_build.for_targets(
            aot_build.units(JIT_NINJA)[0][2], ["dg2-g11", "lnl-m"], "/t/0.o"
        )
        start = argv.index("-fsycl")
        self.assertEqual(argv[start : start + 6], [*aot_build.AOT_ARGS, "-device dg2-g11,lnl-m"])
        self.assertEqual(argv[-2:], ["-o", "/t/0.o"])
        self.assertNotIn("-MD", argv)
        self.assertNotIn("src/float_motion_sycl.o.d", argv)
        self.assertNotIn("src/float_motion_sycl.o", argv)
        self.assertIn("../core/src/./feature/sycl/float_motion_sycl.cpp", argv)

    def test_aot_command_gets_the_new_list_once(self) -> None:
        argv = aot_build.for_targets(aot_build.units(AOT_NINJA)[0][2], ["lnl-m"], "/t/1.o")
        self.assertEqual([a for a in argv if a.startswith("-device ")], ["-device lnl-m"])
        for argument in aot_build.AOT_ARGS:
            self.assertEqual(argv.count(argument), 1, argument)


if __name__ == "__main__":
    unittest.main()
