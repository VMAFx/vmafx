# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Contract tests for Meson compiler-command policy."""

import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MESON_SOURCE = ROOT / "core" / "src" / "meson.build"
GENERATOR_SOURCE = ROOT / "scripts" / "ci" / "gen-sycl-compile-commands.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("gen_sycl_compile_commands", GENERATOR_SOURCE)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {GENERATOR_SOURCE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MesonCommandContractTest(unittest.TestCase):
    def test_language_standards_are_meson_builtin_options(self) -> None:
        source = (ROOT / "core" / "meson.build").read_text(encoding="utf-8")

        self.assertIn("'c_std=c23,c2x,c17'", source)
        self.assertIn("'cpp_std=c++26,c++23,c++latest'", source)
        self.assertNotRegex(source, r"add_project_arguments\([^\n]*(?:-std=|_std_(?:args|flag))")

    def test_device_list_is_scoped_to_spir64_gen(self) -> None:
        source = MESON_SOURCE.read_text(encoding="utf-8")
        match = re.search(
            r"# AOT path:.*?sycl_toolchain_args\s*=\s*\[(?P<body>.*?)\]",
            source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(match, "missing SYCL toolchain argument list")
        body = match.group("body")

        self.assertIn("'-fsycl-targets=spir64_gen,spir64'", body)
        self.assertIn("'-Xsycl-target-backend=spir64_gen'", body)
        self.assertNotIn("'-Xs'", body)
        self.assertRegex(
            body,
            r"'-Xsycl-target-backend=spir64_gen'\s*,\s*'-device '\s*\+\s*icpx_aot_targets",
        )

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
