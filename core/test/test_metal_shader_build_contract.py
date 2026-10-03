#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every Metal kernel is built for the macOS the tester bundle runs on (ADR-1496).

`core/src/metal/meson.build` compiles each `core/src/feature/metal/*.metal`
with `xcrun metal -c`. Without `-mmacosx-version-min` the offline compiler
stamps the `.air` with the build machine's SDK version, and a metallib stamped
newer than the host's macOS refuses to load, so every Metal twin would fail
init on a tester's Mac with an older macOS than the hosted runner's SDK. The
kernels take one argument list with the language revision and the deployment
target, and the target is the one the macOS tester bundle builds its C code
for (`MACOSX_DEPLOYMENT_TARGET` in `scripts/ci/build-macos-tester-bundle.sh`).

Since ADR-1498 the kernels also take one strict floating-point list, defined
once: `-fno-fast-math -ffp-contract=off` (the Metal compiler's default is fast
math, and its safe mode still contracts within a statement), and no other
floating-point flag; and the include directories of the shared arithmetic.

No kernel and no header a kernel includes declares a variable, parameter or
field with the name of a Metal Shading Language type or address space
(`half`, `device`, `thread`, ...: Specification 4.1, Tables 2.1 and section
4): a C or C++ host build accepts such a name, the Metal compiler does not,
and no Linux lane runs the Metal compiler.

Device-free: reads the build files and the kernel sources only.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
METAL_MESON = ROOT / "core" / "src" / "metal" / "meson.build"
KERNEL_DIR = ROOT / "core" / "src" / "feature" / "metal"
BUNDLE_SCRIPT = ROOT / "scripts" / "ci" / "build-macos-tester-bundle.sh"

# macOS major version -> Metal language revision of that release (Metal Shading
# Language Specification 4.1, section 1.6.10).
METAL_REVISION_FOR_MACOS = {"13": "metal3.0", "14": "metal3.1", "15": "metal3.2"}

TARGET_ARGS = re.compile(r"metal_shader_target_args = \[([^\]]*)\]")
KERNEL_LIST = re.compile(r"metal_kernel_names = \[(.*?)\]", re.S)
DEPLOYMENT = re.compile(r"^export MACOSX_DEPLOYMENT_TARGET=(\d+)\.(\d+)$", re.M)


def meson_code() -> str:
    """meson.build without comments."""
    return "\n".join(line.split("#", 1)[0] for line in METAL_MESON.read_text().splitlines())


def target_args(code: str) -> list[str]:
    match = TARGET_ARGS.search(code)
    assert match, "core/src/metal/meson.build: no metal_shader_target_args list"
    return re.findall(r"'([^']+)'", match.group(1))


def kernel_names(code: str) -> list[str]:
    match = KERNEL_LIST.search(code)
    assert match, "core/src/metal/meson.build: no metal_kernel_names list"
    return re.findall(r"'(\w+)'", match.group(1))


POLICY_BEGIN = "# BEGIN VMAF Metal shader strict FP policy"
POLICY_END = "# END VMAF Metal shader strict FP policy"
STRICT_FP = "metal_shader_strict_fp_args = ['-fno-fast-math', '-ffp-contract=off']"
LOOSE_FP = re.compile(
    r"-ffast-math|-fmetal-math-mode=(fast|relaxed)|-ffp-contract=(fast|on)|fp32-functions=fast"
)


def strict_fp_failures(text: str) -> list[str]:
    """ADR-1498: one strict list between the markers, no loosening flag."""
    if POLICY_BEGIN not in text or POLICY_END not in text:
        return ["the Metal shader strict FP policy markers are missing"]
    block = text[text.index(POLICY_BEGIN) : text.index(POLICY_END)]
    failures = []
    if STRICT_FP not in block:
        failures.append("the policy block does not define the strict list")
    code = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
    if code.count("metal_shader_strict_fp_args =") != 1:
        failures.append("the strict list is defined more than once")
    if LOOSE_FP.search(code):
        failures.append("a flag that turns fast math or contraction back on")
    return failures


# Names that are type names or keywords in Metal Shading Language but plain
# identifiers in C and C++.
MSL_NAMES = (
    "half|bfloat|device|constant|thread|threadgroup|threadgroup_imageblock|"
    "ray_data|object_data|kernel|vertex|fragment"
)
DECLARED_TYPE = (
    r"(?:bool|char|short|int|long|float|unsigned|signed|uint|ushort|uchar|ulong|"
    r"size_t|u?int(?:8|16|32|64)_t|vmaf_mtl_\w+|VmafMtl\w+)"
)
MSL_NAME_DECLARED = re.compile(rf"\b{DECLARED_TYPE}\s*[*&]?\s+({MSL_NAMES})\b")
MSL_NAME_MEMBER = re.compile(rf"(?:\.|->)\s*({MSL_NAMES})\b")
QUOTED_INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
INCLUDE_DIRS = (KERNEL_DIR, ROOT / "core" / "src" / "feature", ROOT / "core" / "src")


def kernel_sources() -> dict[str, str]:
    """Every .metal file, every Metal header and every quoted header they
    include, transitively."""
    pending = sorted(KERNEL_DIR.glob("*.metal")) + sorted(KERNEL_DIR.glob("*.h"))
    sources: dict[str, str] = {}
    while pending:
        path = pending.pop().resolve()
        key = str(path.relative_to(ROOT))
        if key in sources:
            continue
        sources[key] = path.read_text(encoding="utf-8")
        for name in QUOTED_INCLUDE.findall(sources[key]):
            found = [d / name for d in (path.parent, *INCLUDE_DIRS) if (d / name).is_file()]
            pending.extend(found[:1])
    return sources


def msl_name_failures(sources: dict[str, str]) -> list[str]:
    """A declaration or member access with a name MSL reserves."""
    failures = []
    for name, text in sorted(sources.items()):
        code = COMMENT.sub(" ", text)
        for pattern in (MSL_NAME_DECLARED, MSL_NAME_MEMBER):
            failures += [f"{name}: `{m.group(0).strip()}`" for m in pattern.finditer(code)]
    return failures


def target_failures(args: list[str], bundle_major: str) -> list[str]:
    """Why the target arguments do not match the bundle's macOS floor."""
    failures = []
    wanted_min = f"-mmacosx-version-min={bundle_major}.0"
    if wanted_min not in args:
        failures.append(f"{wanted_min} is missing (the bundle builds for macOS {bundle_major})")
    wanted_std = f"-std={METAL_REVISION_FOR_MACOS.get(bundle_major, '?')}"
    if wanted_std not in args:
        failures.append(f"{wanted_std} is missing")
    return failures


class MetalShaderBuildContract(unittest.TestCase):
    def test_every_kernel_file_is_built(self) -> None:
        on_disk = sorted(path.stem for path in KERNEL_DIR.glob("*.metal"))
        self.assertEqual(sorted(kernel_names(meson_code())), on_disk)

    def test_every_kernel_command_takes_the_target_arguments(self) -> None:
        code = meson_code()
        loop = code[code.index("foreach _k : metal_kernel_names") :]
        loop = loop[: loop.index("endforeach")]
        flat = " ".join(loop.split())
        self.assertIn("'metal'] + metal_shader_target_args", flat)
        # ADR-1498: the strict FP list and the include directories.
        self.assertIn("+ metal_shader_strict_fp_args + metal_shader_include_args", flat)
        # No kernel is compiled outside the loop, with arguments of its own.
        self.assertEqual(code.count("'macosx', 'metal'"), 1)

    def test_strict_fp_policy_is_defined_once(self) -> None:
        self.assertEqual(strict_fp_failures(METAL_MESON.read_text()), [])

    def test_a_loosened_or_second_fp_list_is_detected(self) -> None:
        text = METAL_MESON.read_text()
        self.assertTrue(strict_fp_failures(text.replace("'-ffp-contract=off'", "'-ffp-contract=fast'")))
        self.assertTrue(strict_fp_failures(text + "\nx = ['-ffast-math']\n"))
        self.assertTrue(strict_fp_failures(text.replace(POLICY_BEGIN, "# moved")))

    def test_no_source_uses_a_name_msl_reserves(self) -> None:
        sources = kernel_sources()
        self.assertIn("core/src/feature/metal/metal_soft_double.h", sources)
        self.assertIn("core/src/feature/ff_pair.h", sources)
        self.assertEqual(msl_name_failures(sources), [])

    def test_a_reserved_name_is_detected(self) -> None:
        planted = {
            "a.h": "VMAF_MTL_FUNC bool f(vmaf_mtl_u64 kept, bool half) { return half; }",
            "b.h": "typedef struct S { bool sticky; uint32_t device; } S;",
            "c.h": "VMAF_MTL_FUNC bool g(VmafMtlShifted s) { return s.half; }",
            "d.h": "/* bool half */ const bool halfway = true;",
        }
        failures = msl_name_failures(planted)
        self.assertEqual([f.split(":")[0] for f in failures], ["a.h", "b.h", "c.h"])

    def test_target_is_the_bundle_floor(self) -> None:
        match = DEPLOYMENT.search(BUNDLE_SCRIPT.read_text())
        self.assertIsNotNone(match, "the bundle script sets no MACOSX_DEPLOYMENT_TARGET")
        self.assertEqual(target_failures(target_args(meson_code()), match.group(1)), [])

    def test_a_missing_or_wrong_target_is_detected(self) -> None:
        self.assertEqual(len(target_failures(["-std=metal3.1"], "14")), 1)
        self.assertEqual(
            len(target_failures(["-std=metal3.2", "-mmacosx-version-min=14.0"], "14")), 1
        )
        self.assertEqual(target_failures(["-std=metal3.1", "-mmacosx-version-min=14.0"], "14"), [])


if __name__ == "__main__":
    unittest.main()
