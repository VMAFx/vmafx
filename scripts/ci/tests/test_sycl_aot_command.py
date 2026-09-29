# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Contract tests for Meson compiler-command policy."""

import importlib.util
import re
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]
MESON_SOURCE = ROOT / "core" / "src" / "meson.build"
GENERATOR_SOURCE = ROOT / "scripts" / "ci" / "gen-sycl-compile-commands.py"


def load_generator() -> ModuleType:
    spec = importlib.util.spec_from_file_location("gen_sycl_compile_commands", GENERATOR_SOURCE)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {GENERATOR_SOURCE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MesonCommandContractTest(unittest.TestCase):
    def test_language_standards_are_meson_builtin_options(self) -> None:
        source = (ROOT / "core" / "meson.build").read_text(encoding="utf-8")

        # 'none' has to stay last in the c_std list: Meson's intel-llvm-cl
        # backend advertises only c89/c99/c11, so a list without a value every
        # backend accepts aborts configure on the Windows MSVC+SYCL leg. Every
        # MSVC-syntax driver is handed /std:clatest separately, so landing on
        # 'none' there still compiles in the newest C mode.
        self.assertIn("'c_std=c23,c2x,c17,none'", source)
        self.assertIn("'cpp_std=c++26,c++23,c++latest'", source)
        self.assertNotRegex(source, r"add_project_arguments\([^\n]*(?:-std=|_std_(?:args|flag))")

    def test_device_list_is_scoped_to_spir64_gen(self) -> None:
        source = MESON_SOURCE.read_text(encoding="utf-8")
        match = re.search(
            r"# AOT path:.*?sycl_icpx_aot_base_args\s*=\s*\[(?P<body>.*?)\]",
            source,
            flags=re.DOTALL,
        )
        if match is None:
            self.fail("missing SYCL AOT base argument list")
        body = match.group("body")
        arguments = re.findall(r"'([^']*)'", body)

        self.assertIn("-fsycl-targets=spir64_gen,spir64", arguments)
        self.assertNotIn("-Xs", arguments)
        # The backend selector is last so the device list appended at every use
        # site lands directly after it.
        self.assertEqual(arguments[-1], "-Xsycl-target-backend=spir64_gen")
        uses = re.findall(r"sycl_icpx_aot_base_args\s*\+\s*\[\s*'([^']*)'", source)
        self.assertGreaterEqual(len(uses), 2, "toolchain and per-TU device lists")
        self.assertTrue(all(use == "-device " for use in uses), uses)

    def test_aot_images_are_generated_at_compile_time(self) -> None:
        # ADR-1360: with relocatable device code the ocloc step runs at the link,
        # and the links pass only -fsycl, so the spir64_gen images were dropped.
        source = MESON_SOURCE.read_text(encoding="utf-8")
        match = re.search(
            r"sycl_icpx_aot_base_args\s*=\s*\[(?P<body>.*?)\]", source, flags=re.DOTALL
        )
        if match is None:
            self.fail("missing SYCL AOT base argument list")
        self.assertIn("'-fno-sycl-rdc'", match.group("body"))
        link = re.search(
            r"sycl_dependency \+= declare_dependency\((?P<body>.*?)\n    \)",
            source,
            flags=re.DOTALL,
        )
        if link is None:
            self.fail("missing sycl_dependency declaration")
        self.assertIn("link_args : sycl_link_args,", link.group("body"))
        # Every driver-linked build keeps `-fsycl` on the link; only MSVC's
        # link.exe, which cannot use it, swaps it for the device link (ADR-1364).
        self.assertIn(
            "sycl_link_args = sycl_msvc_device_link ? ['/IGNORE:4078'] : ['-fsycl']", source
        )
        self.assertIn("find_program('ocloc', required : false)", source)
        self.assertIn("files('sycl/check_aot_image.py')", source)

    def test_msvc_build_registers_images_through_one_device_link(self) -> None:
        # ADR-1364: link.exe never wraps device code, so an MSVC build compiles
        # relocatable device code, device-links every SYCL object once and
        # anchors the result so link.exe pulls it out of vmaf.lib.
        source = MESON_SOURCE.read_text(encoding="utf-8")
        self.assertIn(
            "sycl_msvc_device_link = not is_sycl_acpp and cc.get_argument_syntax() == 'msvc'",
            source,
        )
        self.assertIn("'-fsycl', '-fsycl-link'", source)
        self.assertIn("input : common_sycl_objects + sycl_feature_objects", source)
        self.assertIn("files('sycl/coff_add_anchor.py')", source)
        self.assertIn("'--symbol', 'vmaf_sycl_device_images'", source)
        self.assertIn("-DVMAF_SYCL_MSVC_DEVICE_LINK=1", source)
        common = (ROOT / "core" / "src" / "sycl" / "common.cpp").read_text(encoding="utf-8")
        self.assertIn('#pragma comment(linker, "/include:vmaf_sycl_device_images")', common)

    def test_tidy_database_strips_target_scoped_aot_argument(self) -> None:
        module = load_generator()
        ninja = """\
build src/example.o: CUSTOM_COMMAND ../src/example_sycl.cpp | /opt/intel/oneapi/compiler/latest/bin/icpx
 COMMAND = /opt/intel/oneapi/compiler/latest/bin/icpx -fsycl -fsycl-targets=spir64_gen,spir64 -Xsycl-target-backend=spir64_gen '-device$ dg2-g10,mtl-h' -DHAVE_SYCL -c ../src/example_sycl.cpp -o src/example.o
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "build.ninja"
            path.write_text(ninja, encoding="utf-8")
            entries = module.parse_ninja_sycl_commands(path)

        self.assertEqual(len(entries), 1)
        command = entries[0]["command"]
        self.assertNotIn("-Xsycl-target-backend", command)
        self.assertNotIn("-device", command)
        self.assertNotIn("-fsycl", command)
        self.assertIn("-DHAVE_SYCL", command)


if __name__ == "__main__":
    unittest.main()
