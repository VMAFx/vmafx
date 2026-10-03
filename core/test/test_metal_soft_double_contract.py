#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the Metal soft-double headers to the SYCL ones and to Metal (ADR-1498).

``core/src/feature/metal/metal_soft_double.h`` and ``metal_soft_signed.h``
are ``core/src/feature/sycl/sycl_soft_double.h`` and ``sycl_soft_signed.h``
statement for statement, spelled in what Metal Shading Language, C and C++
share, so the exact Metal twins can run the CPU's fp64 operations in 64-bit
integers as their SYCL twins do. Two things can break that without a
compiler noticing on this host:

* a construct Metal does not have (``double``, ``long long``, a ``ULL``
  literal, ``std::``, a pointer or reference parameter without an address
  space, a ``static`` local, a 128-bit type, a 64-bit ``mulhi``, an include
  the kernel build cannot resolve) -- the macOS job would fail, or worse, a
  libm call would compile and round differently on the GPU;
* the two copies drifting apart: a SYCL operation the Metal header lacks or
  spells with another signature, a constant with another value, a function
  whose loop bound, clamp, ternaries or shifts differ.

The second is checked through the name mapping documented at the top of each
Metal header: ``vmaf_sycl_soft::f`` -> ``vmaf_mtl_f``, ``struct S`` ->
``VmafMtlS``, ``kName`` -> ``VMAF_MTL_SOFT_NAME``. Every SYCL function must
exist in the paired Metal header with the mapped signature and the same
numeric literals, ``if`` / ``for`` / ``return`` / ``?`` / shift counts; every
struct with the same fields; every constant with the same value.

Device-free: reads the sources only. ``test_metal_soft_double`` checks the
arithmetic on the host; the device run of the twins that use the headers
checks it on an Apple GPU.
"""

from __future__ import annotations

import ast
import math
import operator
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"
PAIRS = {
    "metal_soft_double.h": "sycl_soft_double.h",
    "metal_soft_signed.h": "sycl_soft_signed.h",
}

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
MATH_FUNCTIONS = (
    "log",
    "log2",
    "log10",
    "exp",
    "exp2",
    "exp10",
    "pow",
    "powr",
    "cbrt",
    "sqrt",
    "rsqrt",
    "fma",
    "fabs",
    "ldexp",
    "frexp",
    "floor",
    "ceil",
    "rint",
    "round",
    "trunc",
    "sin",
    "cos",
)
# Constructs Metal Shading Language, C or the host builds do not share.
FORBIDDEN = (
    (re.compile(r"\bdouble\b"), "uses double"),
    (re.compile(r"\blong\s+long\b"), "uses long long"),
    (re.compile(r"\b(?:0[xX][0-9A-Fa-f]+|\d+)(?:[uU]?[lL]+|[lL]+[uU])\b"), "has an L/LL literal"),
    (re.compile(r"\bstd::"), "uses std::"),
    (re.compile(r"\bsycl::"), "uses sycl::"),
    (re.compile(r"__int128|\b__uint128"), "uses a 128-bit type"),
    (re.compile(r"\bmulhi\b|\bmul_hi\b|__umulh|_umul128"), "uses a 64-bit high multiply"),
    (re.compile(r"\bnamespace\b"), "uses a namespace"),
    (re.compile(r"\bconstexpr\b"), "uses constexpr"),
    (re.compile(r"\btemplate\b"), "uses a template"),
    (re.compile(r"\bauto\b"), "uses auto"),
    (re.compile(r"\bstatic\b"), "uses static"),
    (re.compile(r"\binline\b"), "uses inline outside VMAF_MTL_FUNC"),
    (re.compile(r"[{,]\s*\.[A-Za-z_]\w*\s*="), "uses a designated initializer"),
    (re.compile(r"\bas_type\b|\bmemcpy\b|bit_cast"), "bit-casts outside VMAF_MTL_F2U/U2F"),
    (re.compile(r"\bclz\s*\(|__builtin_clz"), "counts zeros outside VMAF_MTL_CLZ64"),
    # A math-library function is not correctly rounded on the device (Metal
    # Shading Language Specification 4.1, Table 8.1), and none is needed.
    (re.compile(rf"\b(?:{'|'.join(MATH_FUNCTIONS)})f?\s*\("), "calls the math library"),
)
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"][^>"]+[>"])', re.M)
ALLOWED_INCLUDES = {'"metal_portable.h"', '"metal_soft_double.h"'}
C_ONLY_GUARD = "#if !defined(__METAL_VERSION__) && !defined(__cplusplus)\n#include <stdbool.h>"

SYCL_FUNC = re.compile(
    r"(?:^|\n)\s*(?:inline|VMAF_SYCL_ALWAYS_INLINE)\s+(?!constexpr)(\w+)\s+(\w+)\s*\(([^)]*)\)\s*\{"
)
METAL_FUNC = re.compile(r"(?:^|\n)\s*VMAF_MTL_FUNC\s+(\w+)\s+(\w+)\s*\(([^)]*)\)\s*\{")
SYCL_STRUCT = re.compile(r"\bstruct\s+(\w+)\s*\{([^}]*)\};")
METAL_STRUCT = re.compile(r"\btypedef\s+struct\s+(\w+)\s*\{([^}]*)\}\s*(\w+)\s*;")
SYCL_CONST = re.compile(r"\binline\s+constexpr\s+\w+\s+(k\w+)\s*=\s*([^;]+);")
METAL_MACRO = re.compile(r"^\s*#\s*define\s+(\w+)\s+(.+)$", re.M)
BINARY = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.LShift: operator.lshift,
    ast.RShift: operator.rshift,
    ast.BitOr: operator.or_,
    ast.BitAnd: operator.and_,
}
LITERAL = re.compile(r"\b(0[xX][0-9A-Fa-f]+|\d+\.\d*|\d+)[uUfF]*\b")

# The Metal headers' constructors, which stand for designated initializers
# and sycl::clz on 32 bits: the only functions without a SYCL counterpart.
METAL_ONLY = {"vmaf_mtl_soft_make", "vmaf_mtl_u128_make", "vmaf_mtl_shifted_make"}
METAL_ONLY |= {"vmaf_mtl_soft_clz32"}
TYPES = {
    "uint64_t": "vmaf_mtl_u64",
    "int64_t": "vmaf_mtl_i64",
    "uint32_t": "vmaf_mtl_u32",
    "int32_t": "vmaf_mtl_i32",
    "int": "int",
    "bool": "bool",
    "float": "float",
}


def _code(text: str) -> str:
    """Source without comments; every newline kept."""
    return COMMENT.sub(lambda m: " " + "\n" * m.group(0).count("\n"), text)


def _sources() -> dict[str, str]:
    sources = {name: (FEATURE / "metal" / name).read_text(encoding="utf-8") for name in PAIRS}
    for name in PAIRS.values():
        sources[name] = (FEATURE / "sycl" / name).read_text(encoding="utf-8")
    return sources


def _metal_type(sycl_type: str) -> str:
    if sycl_type in TYPES:
        return TYPES[sycl_type]
    return "VmafMtl" + sycl_type


def _metal_constant(name: str) -> str:
    words = re.findall(r"[A-Z][a-z0-9]*", name[1:])
    return "VMAF_MTL_SOFT_" + "_".join(word.upper() for word in words)


def _body(code: str, start: int) -> str:
    """The brace-matched block whose opening brace ends at `start`."""
    depth = 0
    for index in range(start - 1, len(code)):
        depth += {"{": 1, "}": -1}.get(code[index], 0)
        if depth == 0:
            return code[start - 1 : index + 1]
    return ""


def _functions(code: str, pattern: re.Pattern[str]) -> dict[str, tuple[str, str, str]]:
    """name -> (return type, parameters, body)."""
    return {
        m.group(2): (m.group(1), " ".join(m.group(3).split()), _body(code, m.end()))
        for m in pattern.finditer(code)
    }


def _value(literal: str) -> float:
    return float(literal) if "." in literal else float(int(literal, 0))


def _fingerprint(body: str) -> tuple[object, ...]:
    """What a statement-for-statement port keeps: every numeric literal and
    the count of branches, loops, returns, selects and shifts."""
    literals = sorted(_value(m.group(1)) for m in LITERAL.finditer(body))
    counts = [len(re.findall(rf"\b{word}\b", body)) for word in ("if", "for", "return")]
    return (literals, counts, body.count("?"), body.count("<<"), body.count(">>"))


def _mapped_parameters(parameters: str) -> str:
    mapped = []
    for parameter in filter(None, (p.strip() for p in parameters.split(","))):
        *kind, name = parameter.split()
        mapped.append(f"{_metal_type(' '.join(kind))} {name}")
    return ", ".join(mapped)


def _subset_failures(name: str, text: str) -> list[str]:
    code = _code(text)
    failures = [f"{name} {what}" for pattern, what in FORBIDDEN if pattern.search(code)]
    for include in INCLUDE.findall(code):
        if include == "<stdbool.h>" and C_ONLY_GUARD in code:
            continue
        if include not in ALLOWED_INCLUDES:
            failures.append(f"{name} includes {include}")
    for function, (_, parameters, _) in _functions(code, METAL_FUNC).items():
        if "*" in parameters or "&" in parameters:
            failures.append(f"{name}: {function}() takes a pointer or reference")
    return failures


def _function_failures(metal: str, sycl: str) -> list[str]:
    ours = _functions(_code(metal), METAL_FUNC)
    theirs = _functions(_code(sycl), SYCL_FUNC)
    failures = []
    for function, (result, parameters, body) in theirs.items():
        name = "vmaf_mtl_" + function
        if name not in ours:
            failures.append(f"{function}() has no {name}()")
            continue
        signature = (_metal_type(result), _mapped_parameters(parameters))
        if signature != ours[name][:2]:
            failures.append(f"{name}() has another signature than {function}()")
        elif _fingerprint(body) != _fingerprint(ours[name][2]):
            failures.append(f"{name}() is not {function}() statement for statement")
    expected = {"vmaf_mtl_" + f for f in theirs} | METAL_ONLY
    failures += [f"{name}() has no SYCL counterpart" for name in set(ours) - expected]
    return failures


def _fields(block: str, mapped: bool) -> list[str]:
    """`type name` per field; a SYCL field's type mapped to its Metal type."""
    fields = []
    for declaration in filter(None, (d.strip() for d in block.split(";"))):
        *kind, name = declaration.split()
        spelled = " ".join(kind)
        fields.append(f"{_metal_type(spelled) if mapped else spelled} {name}")
    return fields


def _struct_failures(metal: str, sycl: str) -> list[str]:
    ours = {m.group(1): (m.group(2), m.group(3)) for m in METAL_STRUCT.finditer(_code(metal))}
    failures = []
    for m in SYCL_STRUCT.finditer(_code(sycl)):
        name = "VmafMtl" + m.group(1)
        if name not in ours or ours[name][1] != name:
            failures.append(f"struct {m.group(1)} has no typedef struct {name}")
        elif _fields(m.group(2), True) != _fields(ours[name][0], False):
            failures.append(f"{name} has other fields than {m.group(1)}")
    return failures


def _substituted(word: str, names: dict[str, str]) -> str:
    return f"({names[word]})" if word in names else word


def _evaluate(expression: str, names: dict[str, str]) -> float:
    """A constant expression of either header, with the other constants of
    `names` substituted; integer and float suffixes and casts removed."""
    text = expression
    for _ in range(4):
        text = re.sub(r"\b[A-Za-z_]\w*\b", lambda m: _substituted(m.group(0), names), text)
    text = re.sub(r"\b(?:u?int(?:32|64)_t|VMAF_MTL_[UI]64)\s*[({]([^)}]*)[)}]", r"(\1)", text)
    text = re.sub(r"\b(0[xX][0-9A-Fa-f]+)[uU]\b", r"\1", text)
    text = re.sub(r"\b(\d+\.\d*)[fF]\b|\b(\d+)[uU]\b", lambda m: m.group(1) or m.group(2), text)
    return _arithmetic(text)


def _arithmetic(text: str) -> float:
    """The value of an expression of numbers, + - * << >> | & and unary
    minus, evaluated from the leaves up (no eval, no recursion); NaN for
    anything else."""
    try:
        tree = ast.parse(text, mode="eval").body
    except SyntaxError:
        return math.nan
    values: dict[int, float] = {}
    # Breadth-first order reversed: every child before its parent.
    for node in reversed(list(ast.walk(tree))):
        if isinstance(node, (ast.operator, ast.unaryop)):
            continue
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
            values[id(node)] = node.value
        elif isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            values[id(node)] = -values[id(node.operand)]
        elif isinstance(node, ast.BinOp) and type(node.op) in BINARY:
            values[id(node)] = BINARY[type(node.op)](values[id(node.left)], values[id(node.right)])
        else:
            return math.nan
    return values[id(tree)]


def _constant_failures(sources: dict[str, str]) -> list[str]:
    sycl = {}
    metal = {}
    for ours, theirs in PAIRS.items():
        sycl.update(dict(SYCL_CONST.findall(_code(sources[theirs]))))
        metal.update(dict(METAL_MACRO.findall(_code(sources[ours]))))
    failures = []
    for name, expression in sycl.items():
        macro = _metal_constant(name)
        if macro not in metal:
            failures.append(f"{name} has no {macro}")
        elif _evaluate(expression, sycl) != _evaluate(metal[macro], metal):
            failures.append(f"{macro} is not the value of {name}")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures = _constant_failures(sources)
    for ours, theirs in PAIRS.items():
        failures += _subset_failures(ours, sources[ours])
        failures += _function_failures(sources[ours], sources[theirs])
        failures += _struct_failures(sources[ours], sources[theirs])
    return failures


class MetalSoftDoubleContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_every_sycl_operation_is_mapped(self) -> None:
        # The mapping must see the whole SYCL surface, not an empty match.
        sources = _sources()
        names = set(_functions(_code(sources["sycl_soft_signed.h"]), SYCL_FUNC))
        names |= set(_functions(_code(sources["sycl_soft_double.h"]), SYCL_FUNC))
        self.assertGreaterEqual(len(names), 31)
        self.assertIn("soft_div_digits", names)
        self.assertIn("u128_mul", names)

    def test_double_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_double.h",
            "    const vmaf_mtl_u64 low = mant & 7u;",
            "    const double unused = 0.0;\n    const vmaf_mtl_u64 low = mant & 7u;",
        )
        self._assert_detected(failures, "uses double")

    def test_ull_literal_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_double.h",
            "#define VMAF_MTL_SOFT_DOUBLE_TOP (VMAF_MTL_U64(1) << 52)",
            "#define VMAF_MTL_SOFT_DOUBLE_TOP (1ULL << 52)",
        )
        self._assert_detected(failures, "L/LL literal")

    def test_long_long_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            "    const vmaf_mtl_i64 divisor = (vmaf_mtl_i64)den;",
            "    const long long divisor = (long long)den;",
        )
        self._assert_detected(failures, "uses long long")

    def test_mulhi_is_detected(self) -> None:
        # The SYCL twin wrote the product out because a 64-bit mul_hi was
        # wrong on a device; Metal's mulhi() is not trusted either.
        failures = self._edited(
            "metal_soft_double.h",
            "    const vmaf_mtl_u64 high = a_hi * b_hi;",
            "    const vmaf_mtl_u64 high = metal::mulhi(a, b);",
        )
        self._assert_detected(failures, "64-bit high multiply")

    def test_128_bit_type_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_double.h",
            "    const vmaf_mtl_u64 mask = 0xFFFFFFFFu;",
            "    const unsigned __int128 wide = a;\n    const vmaf_mtl_u64 mask = 0xFFFFFFFFu;",
        )
        self._assert_detected(failures, "128-bit type")

    def test_pointer_parameter_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_double.h",
            "vmaf_mtl_soft_less(VmafMtlSoftDouble a, VmafMtlSoftDouble b)",
            "vmaf_mtl_soft_less(thread VmafMtlSoftDouble *a, VmafMtlSoftDouble b)",
        )
        self._assert_detected(failures, "takes a pointer or reference")

    def test_reference_parameter_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            "vmaf_mtl_signed_abs(VmafMtlSoftSigned a)",
            "vmaf_mtl_signed_abs(const VmafMtlSoftSigned &a)",
        )
        self._assert_detected(failures, "takes a pointer or reference")

    def test_static_local_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            "    const bool zero = mant == 0u;",
            "    static const bool once = true;\n    const bool zero = mant == 0u;",
        )
        self._assert_detected(failures, "uses static")

    def test_foreign_include_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            '#include "metal_soft_double.h"',
            '#include "metal_soft_double.h"\n#include <metal_stdlib>',
        )
        self._assert_detected(failures, "includes <metal_stdlib>")

    def test_unguarded_stdbool_is_detected(self) -> None:
        # <stdbool.h> exists for host C only; MSL has no such header.
        failures = self._edited(
            "metal_soft_double.h",
            "#if !defined(__METAL_VERSION__) && !defined(__cplusplus)\n#include <stdbool.h>",
            "#if 1\n#include <stdbool.h>",
        )
        self._assert_detected(failures, "includes <stdbool.h>")

    def test_designated_initializer_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_double.h",
            "    const VmafMtlSoftDouble value = {mant, exp};",
            "    const VmafMtlSoftDouble value = {.mant = mant, .exp = exp};",
        )
        self._assert_detected(failures, "designated initializer")

    def test_math_library_call_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            "(float)(vmaf_mtl_u32)(den >> VMAF_MTL_SOFT_BELOW_FLOAT);",
            "(float)(vmaf_mtl_u32)(den >> VMAF_MTL_SOFT_BELOW_FLOAT) * exp2f(0.0f);",
        )
        self._assert_detected(failures, "calls the math library")
        failures = self._edited(
            "metal_soft_double.h",
            "    const vmaf_mtl_u64 half = VMAF_MTL_U64(1) << 28;",
            "    const vmaf_mtl_u64 half = (vmaf_mtl_u64)metal::sqrt(16.0f);",
        )
        self._assert_detected(failures, "calls the math library")

    def test_missing_operation_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            "vmaf_mtl_soft_div_digits(VmafMtlSoftDouble a, VmafMtlSoftDouble b)\n{",
            "vmaf_mtl_soft_div_radix(VmafMtlSoftDouble a, VmafMtlSoftDouble b)\n{",
        )
        self._assert_detected(failures, "soft_div_digits() has no vmaf_mtl_soft_div_digits()")
        self._assert_detected(failures, "vmaf_mtl_soft_div_radix() has no SYCL counterpart")

    def test_new_sycl_operation_is_detected(self) -> None:
        # A SYCL operation added later must reach the Metal header too.
        failures = self._edited(
            "sycl_soft_signed.h",
            "} // namespace vmaf_sycl_soft",
            "inline SoftSigned signed_half(SoftSigned a)\n{\n    return a;\n}\n\n"
            "} // namespace vmaf_sycl_soft",
        )
        self._assert_detected(failures, "signed_half() has no vmaf_mtl_signed_half()")

    def test_signature_change_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_double.h",
            "vmaf_mtl_u128_shl(vmaf_mtl_u64 value, vmaf_mtl_u32 shift)",
            "vmaf_mtl_u128_shl(vmaf_mtl_u64 value, vmaf_mtl_u64 shift)",
        )
        self._assert_detected(failures, "vmaf_mtl_u128_shl() has another signature")

    def test_loop_bound_change_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_double.h",
            "    for (int step = 0; step < 56; step++) {",
            "    for (int step = 0; step < 55; step++) {",
        )
        self._assert_detected(failures, "vmaf_mtl_soft_div() is not soft_div()")

    def test_dropped_clamp_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            "    const vmaf_mtl_u32 shift = distance < 59u ? distance : 59u;\n"
            "    const vmaf_mtl_u64 small_wide = small_mant << VMAF_MTL_SOFT_GUARD_BITS;",
            "    const vmaf_mtl_u32 shift = distance;\n"
            "    const vmaf_mtl_u64 small_wide = small_mant << VMAF_MTL_SOFT_GUARD_BITS;",
        )
        self._assert_detected(failures, "vmaf_mtl_signed_add() is not signed_add()")

    def test_dropped_shift_guard_is_detected(self) -> None:
        # Metal masks a shift count to six bits; the guard is what makes a
        # shift by 64 or more zero, as in the SYCL header.
        failures = self._edited(
            "metal_soft_double.h",
            "    const vmaf_mtl_u64 lo = shift < 64u ? value << shift : VMAF_MTL_U64(0);",
            "    const vmaf_mtl_u64 lo = value << shift;",
        )
        self._assert_detected(failures, "vmaf_mtl_u128_shl() is not u128_shl()")

    def test_constant_value_change_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            "#define VMAF_MTL_SOFT_QUIET_NAN_BITS (VMAF_MTL_U64(0x7FF8u) << 48)",
            "#define VMAF_MTL_SOFT_QUIET_NAN_BITS (VMAF_MTL_U64(0x7FF0u) << 48)",
        )
        self._assert_detected(failures, "VMAF_MTL_SOFT_QUIET_NAN_BITS is not the value")

    def test_struct_field_change_is_detected(self) -> None:
        failures = self._edited(
            "metal_soft_signed.h",
            "    vmaf_mtl_u32 negative;\n} VmafMtlSoftSigned;",
            "    bool negative;\n} VmafMtlSoftSigned;",
        )
        self._assert_detected(failures, "VmafMtlSoftSigned has other fields")

    def test_std_and_namespace_are_detected(self) -> None:
        failures = self._edited(
            "metal_soft_double.h",
            "/* A positive fp64 value, mant * 2^exp with mant in [2^52, 2^53). */",
            "namespace vmaf_mtl_soft { using std::uint64_t; }",
        )
        self._assert_detected(failures, "uses a namespace")
        self._assert_detected(failures, "uses std::")


if __name__ == "__main__":
    unittest.main()
