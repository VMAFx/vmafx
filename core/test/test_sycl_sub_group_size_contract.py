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
# The fork's spellings of a required sub-group size: macros and the kernel shape,
# whose second argument is the register file size.
MACROS = (
    re.compile(rf"\bVMAF_SYCL_REQD_SG_SIZE\(\s*{NAME}\s*\)"),
    re.compile(rf"\bVMAF_SYCL_REQD_SG_ATTR\(\s*{NAME}\s*\)"),
    re.compile(rf"\bVMAF_SYCL_FUNCTOR_SG_SIZE\(\s*{NAME}\s*\)"),
    re.compile(rf"\bVMAF_SYCL_FUNCTOR_SG_ATTR\(\s*{NAME}\s*\)"),
)
SHAPE = re.compile(rf"\bVmafSyclKernelShape<\s*{NAME}\s*,\s*{NAME}\s*>")
REQUESTS = (*MACROS, SHAPE)

# A functor's kernel-shape base. Its first argument is read by _shape_size; a
# use this does not match is reported, not skipped (_unread_shapes).
CLASS_SHAPE = re.compile(
    r"\b(?:class|struct)\s+([A-Za-z_0-9]+)\s*:[^{]*?"
    r"\bVmafSyclKernelShape<\s*([^,<>]+?)\s*,\s*([^>]+)>",
    re.S,
)
SHAPE_USE = re.compile(r"\bVmafSyclKernelShape\s*<")
# A size from a constexpr function on one name, as vif_hori_sg_size(SCALE).
SIZE_CALL = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)\s*\(\s*[A-Za-z_0-9]+\s*\)")
CALL_OPERATOR = re.compile(r"\boperator\s*\(\s*\)")
FUNCTOR_ATTR = re.compile(
    rf"(?:\bVMAF_SYCL_FUNCTOR_SG_SIZE\(\s*{NAME}\s*\)|"
    rf"\[\[\s*VMAF_SYCL_FUNCTOR_SG_ATTR\(\s*{NAME}\s*\)\s*\]\])"
)
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


def _class_body(code: str, start: int) -> str:
    """The brace-balanced body of the class whose head ends at `start`."""
    brace = code.find("{", start)
    if brace == -1:
        return ""
    depth = 1
    idx = brace + 1
    while idx < len(code) and depth > 0:
        depth += (code[idx] == "{") - (code[idx] == "}")
        idx += 1
    return code[brace:idx]


def _operator_attrs(body: str) -> list[str | None]:
    """Per call operator of the class body, the size its VMAF_SYCL_FUNCTOR_SG_*
    names, or None; a body without one reads as one operator without it."""
    attrs: list[str | None] = []
    for op_match in CALL_OPERATOR.finditer(body):
        decl = body[: op_match.start()]
        prev = max(decl.rfind(";"), decl.rfind("}"), decl.rfind("{"))
        attr = FUNCTOR_ATTR.search(decl[prev + 1 :])
        attrs.append((attr.group(1) or attr.group(2)) if attr else None)
    return attrs or [None]


def _shape_size(code: str, spelled: str, everywhere: str) -> tuple[str, int | None]:
    """How the contract reads a kernel shape's first argument.

    ("value", size) for a literal or a constant, size None for a template
    parameter; ("call", None) for a constexpr function of the file on one
    name, whose value only the header's static_assert can check; ("unread",
    None) for anything else, which fails the contract.
    """
    if re.fullmatch(NAME, spelled):
        try:
            return "value", _value(code, spelled, everywhere)
        except ValueError:
            return "unread", None
    call = SIZE_CALL.fullmatch(spelled)
    if call and re.search(rf"\bconstexpr\s+int\s+{call.group(1)}\s*\(", code):
        return "call", None
    return "unread", None


def _operator_failures(
    head: str, attrs: list[str | None], size: int | None, compare: bool, code: str, everywhere: str
) -> list[str]:
    """Call operators that drop the shape's size, or state another one."""
    failures: list[str] = []
    for attr_sg in attrs:
        if attr_sg is None:
            failures.append(
                f"{head} carries no VMAF_SYCL_FUNCTOR_SG_* "
                "(sycl_compat.h: lost on non-properties toolchains)"
            )
            continue
        if not compare:
            continue
        try:
            stated = _value(code, attr_sg, everywhere)
        except ValueError:
            continue  # reported by _request_failures as not a constant
        if stated != size:
            failures.append(f"{head} states sub-group size {attr_sg}")
    return failures


def _functor_failures(name: str, code: str, everywhere: str) -> list[str]:
    """Functors whose non-zero kernel-shape size a call operator does not state.

    Without grf_size_properties.hpp the shape base is empty, so only the
    attribute on each call operator keeps the size (sycl_compat.h). A shape
    the contract cannot read fails instead of passing unchecked.
    """
    failures: list[str] = []
    for m in CLASS_SHAPE.finditer(code):
        cls_name, shape_sg = m.group(1), m.group(2)
        kind, size = _shape_size(code, shape_sg, everywhere)
        shape = f"{name}: {cls_name} derives VmafSyclKernelShape<{shape_sg}, ...>"
        if kind == "unread":
            failures.append(
                f"{shape}, a size that is no literal, constant or constexpr function of the file"
            )
            continue
        if size == 0:
            continue
        attrs = _operator_attrs(_class_body(code, m.end()))
        failures += _operator_failures(
            f"{shape} but its operator()", attrs, size, kind == "value", code, everywhere
        )
    if len(SHAPE_USE.findall(code)) != len(CLASS_SHAPE.findall(code)):
        failures.append(
            f"{name}: a VmafSyclKernelShape use that is not the base of a class or "
            "struct this contract reads"
        )
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
        failures += _functor_failures(name, code, everywhere)
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
        "sycl::reqd_sub_group_size(VmafSyclSubGroupSize<N>::value)",
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
            "sycl::reqd_sub_group_size(VmafSyclSubGroupSize<N>::value)",
            "sycl::reqd_sub_group_size(N)",
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

    def test_functor_missing_attribute_on_walk_kernel_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        self.assertIn("[[VMAF_SYCL_FUNCTOR_SG_ATTR(16)]]", sources[name])
        sources[name] = sources[name].replace("[[VMAF_SYCL_FUNCTOR_SG_ATTR(16)]]", "", 1)
        self._detected(
            _contract_failures(options, compat, sources),
            "WalkKernel derives VmafSyclKernelShape<16, ...> but its operator() carries no VMAF_SYCL_FUNCTOR_SG_*",
        )

    def test_functor_missing_attribute_on_planted_functor_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        sources[
            name
        ] += "\nstruct X : public VmafSyclKernelShape<16, 0> { void operator()(sycl::id<1>) const {} };\n"
        self._detected(
            _contract_failures(options, compat, sources),
            "X derives VmafSyclKernelShape<16, ...> but its operator() carries no VMAF_SYCL_FUNCTOR_SG_*",
        )

    def test_functor_shape_size_0_needs_no_attribute(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        sources[
            name
        ] += "\nstruct Z : public VmafSyclKernelShape<0, 256> { void operator()(sycl::id<1>) const {} };\n"
        self.assertEqual(_contract_failures(options, compat, sources), [])

    def test_functor_mismatched_attribute_size_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        sources[
            name
        ] += "\nstruct Y : public VmafSyclKernelShape<16, 0> { VMAF_SYCL_FUNCTOR_SG_SIZE(32) void operator()(sycl::id<1>) const {} };\n"
        self._detected(
            _contract_failures(options, compat, sources),
            "Y derives VmafSyclKernelShape<16, ...> but its operator() states sub-group size 32",
        )

    def test_function_call_shape_without_attribute_is_detected(self) -> None:
        # integer_vif_sycl.cpp's horizontal kernel takes its size from a constexpr function.
        options, compat, sources = _live()
        name = "core/src/feature/sycl/integer_vif_sycl.cpp"
        hori = "VMAF_SYCL_FUNCTOR_SG_SIZE(VIF_SG_SIZE) void operator()"
        self.assertIn(hori, sources[name])
        sources[name] = sources[name].replace(hori, "void operator()", 1)
        self._detected(
            _contract_failures(options, compat, sources),
            "IntegerVifHoriKernel derives VmafSyclKernelShape<vif_hori_sg_size(SCALE), ...> "
            "but its operator() carries no VMAF_SYCL_FUNCTOR_SG_*",
        )

    def test_unannotated_overload_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        sources[name] += (
            "\nstruct W : public VmafSyclKernelShape<16, 0> {"
            " VMAF_SYCL_FUNCTOR_SG_SIZE(16) void operator()(sycl::id<1>) const {}"
            " void operator()(sycl::item<1>) const {} };\n"
        )
        self._detected(
            _contract_failures(options, compat, sources),
            "W derives VmafSyclKernelShape<16, ...> but its operator() carries no "
            "VMAF_SYCL_FUNCTOR_SG_*",
        )

    def test_unreadable_shape_size_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        sources[name] += (
            "\nstruct V : public VmafSyclKernelShape<pick(A, B), 0> {"
            " void operator()(sycl::id<1>) const {} };\n"
        )
        self._detected(
            _contract_failures(options, compat, sources),
            "V derives VmafSyclKernelShape<pick(A, ...>, a size that is no literal",
        )

    def test_shape_outside_a_class_base_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        sources[name] += (
            "\nstruct U : public VmafSyclKernelShape<Pick<3>::value, 0> {"
            " void operator()(sycl::id<1>) const {} };\n"
        )
        self._detected(
            _contract_failures(options, compat, sources),
            "test_sycl_ordered_sum_probe.cpp: a VmafSyclKernelShape use that is not the base",
        )

    def test_size_8_in_an_attr_macro_is_detected(self) -> None:
        options, compat, sources = _live()
        name = "core/test/test_sycl_ordered_sum_probe.cpp"
        self.assertIn("VMAF_SYCL_FUNCTOR_SG_ATTR(16)", sources[name])
        sources[name] = sources[name].replace(
            "VMAF_SYCL_FUNCTOR_SG_ATTR(16)", "VMAF_SYCL_FUNCTOR_SG_ATTR(8)", 1
        )
        self._detected(_contract_failures(options, compat, sources), "requires sub-group size 8")


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
