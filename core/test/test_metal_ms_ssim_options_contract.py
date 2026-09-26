#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device-free source and contract tests for Metal float_ms_ssim options and score parity."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
METAL_MS_SSIM = REPO_ROOT / "core/src/feature/metal/float_ms_ssim_metal.mm"
METAL_MS_SSIM_KERNEL = REPO_ROOT / "core/src/feature/metal/float_ms_ssim.metal"
CPU_MS_SSIM = REPO_ROOT / "core/src/feature/float_ms_ssim.c"
SYCL_MS_SSIM = REPO_ROOT / "core/src/feature/sycl/integer_ms_ssim_sycl.cpp"
CUDA_MS_SSIM = REPO_ROOT / "core/src/feature/cuda/integer_ms_ssim_cuda.c"
HIP_MS_SSIM = REPO_ROOT / "core/src/feature/hip/integer_ms_ssim_hip.c"
DISPATCH_STRATEGY = REPO_ROOT / "core/src/metal/dispatch_strategy.c"
METAL_PARITY_TEST = REPO_ROOT / "core/test/test_metal_float_ms_ssim_parity.c"
OPTION_SEMANTICS = REPO_ROOT / "core/src/feature/metal/float_ms_ssim_option_semantics.h"
OPTION_SEMANTICS_TEST = REPO_ROOT / "core/test/test_metal_ms_ssim_option_semantics.c"
MESON_TESTS = REPO_ROOT / "core/test/meson.build"
METAL_AGENTS = REPO_ROOT / "core/src/feature/metal/AGENTS.md"
METRICS_DOC = REPO_ROOT / "docs/metrics/ms-ssim.md"
RESEARCH_DOC = REPO_ROOT / "docs/research/2110-metal-ms-ssim-option-parity-2026-09-25.md"
CHANGELOG_FRAGMENT = (
    REPO_ROOT / "changelog.d/added/T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07.md"
)


def function_body(source: str, signature: str) -> str:
    """Return one complete function body, preserving call order."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def production_wiring_problems(metal_source: str) -> list[str]:
    """Validate that the Metal host uses each executable semantic seam."""
    problems: list[str] = []
    required_wiring = (
        '#include "float_ms_ssim_option_semantics.h"',
        "vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt)",
        "vmaf_metal_ms_ssim_plane_dimensions(pix_fmt, 1u, w, h, &chroma_w, &chroma_h)",
        "vmaf_metal_ms_ssim_min_luma_dimensions(",
        "vmaf_metal_ms_ssim_max_db(s->clip_db, bpc, w, h)",
    )
    for required in required_wiring:
        if required not in metal_source:
            problems.append(f"Metal source missing semantic wiring: {required}")

    reducer = function_body(metal_source, "static int reduce_plane_means")
    finite_guard = "vmaf_feature_validate_finite_scores_named"
    if finite_guard not in reducer:
        problems.append("Metal reducer does not validate every L/C/S atom")
    elif reducer.index(finite_guard) > reducer.index("pow("):
        problems.append("Metal reducer validates L/C/S atoms only after pow")
    elif "atoms, 3u" not in reducer:
        problems.append("Metal reducer validates fewer than the three L/C/S atoms")

    atom_array = re.search(
        r"const VmafNamedScore atoms\[\]\s*=\s*\{(?P<body>.*?)\};",
        reducer,
        flags=re.DOTALL,
    )
    atom_mappings: list[tuple[str, str]] = []
    if atom_array:
        atom_mappings = [
            (name, value.strip())
            for name, value in re.findall(
                r'\{\s*\.name\s*=\s*"([^"]+)"\s*,\s*\.value\s*=\s*([^}]+)\}',
                atom_array.group("body"),
            )
        ]
    expected_atom_mappings = [
        ("float_ms_ssim_l", "l_means[i]"),
        ("float_ms_ssim_c", "c_means[i]"),
        ("float_ms_ssim_s", "s_means[i]"),
    ]
    if atom_mappings != expected_atom_mappings:
        problems.append("Metal reducer does not bind exact L/C/S names to their mean values")

    validator = function_body(metal_source, "static int validate_dimensions")
    if "if (s->n_planes > 1u)" not in validator:
        problems.append("Metal validator gates chroma dimensions on the raw option")
    elif validator.index("s->n_planes =") > validator.index("if (s->n_planes > 1u)"):
        problems.append("Metal validator checks active planes before resolving pixel format")

    return problems


def option_ownership_problems(parity_source: str) -> list[str]:
    """Validate independent option construction and consumption ownership."""
    problems: list[str] = []
    for signature in (
        "static char *setup_cpu_float_ms_ssim",
        "static char *setup_metal_float_ms_ssim",
    ):
        setup = function_body(parity_source, signature)
        if "make_options(options, opts)" not in setup:
            problems.append(f"{signature} does not construct fresh options")
        if "vmaf_use_feature" not in setup:
            problems.append(f"{signature} does not pass options to its consumer")
        elif "*opts = NULL;" not in setup:
            problems.append(f"{signature} does not relinquish consumed options")
        elif setup.index("*opts = NULL;") < setup.index("vmaf_use_feature"):
            problems.append(f"{signature} relinquishes options before consumption")
        if "vmaf_feature_dictionary_free" in setup:
            problems.append(f"{signature} frees options after consumption")

    for signature in (
        "static char *run_cpu_float_ms_ssim_opts",
        "static char *run_metal_float_ms_ssim_opts",
    ):
        runner_header = function_body(parity_source, signature).split("{", 1)[0]
        if "VmafFeatureDictionary" in runner_header:
            problems.append(f"{signature} accepts a caller-owned dictionary")
    return problems


def metal_wiring_problems(metal_source: str, parity_source: str) -> list[str]:
    """Return all production and parity ownership contract violations."""
    return production_wiring_problems(metal_source) + option_ownership_problems(parity_source)


def option_semantics_oracle_problems(semantics_test_source: str) -> list[str]:
    """Bind the exact dB-ceiling oracle without floating equality or tolerances."""
    problems: list[str] = []
    expected_assertion = "double_bits_equal(clipped, 105.0)"
    if expected_assertion not in semantics_test_source:
        problems.append("Metal dB-ceiling oracle does not require exact bit identity")
    if re.search(r"\bclipped\s*(?:==|!=)|(?:==|!=)\s*clipped\b", semantics_test_source):
        problems.append("Metal dB-ceiling oracle compares floating values directly")

    helper = function_body(semantics_test_source, "static bool double_bits_equal")
    for required in ("uint64_t lhs_bits", "uint64_t rhs_bits", "memcpy", "lhs_bits == rhs_bits"):
        if required not in helper:
            problems.append(f"Metal dB-ceiling bit oracle is missing: {required}")
    return problems


def option_initializer(source: str, option: str) -> str:
    """Return the balanced initializer containing one named option."""
    matches = list(re.finditer(rf'\.name\s*=\s*"{re.escape(option)}"', source))
    if len(matches) != 1:
        raise AssertionError(f"expected one {option!r} option, found {len(matches)}")

    start = source.rfind("{", 0, matches[0].start())
    if start < 0:
        raise AssertionError(f"{option!r} has no initializer start")

    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"{option!r} has no initializer end")


class MetalMsSsimOptionsContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.metal_src = METAL_MS_SSIM.read_text(encoding="utf-8")
        cls.metal_kernel_src = METAL_MS_SSIM_KERNEL.read_text(encoding="utf-8")
        cls.cpu_src = CPU_MS_SSIM.read_text(encoding="utf-8")
        cls.sycl_src = SYCL_MS_SSIM.read_text(encoding="utf-8")
        cls.cuda_src = CUDA_MS_SSIM.read_text(encoding="utf-8")
        cls.hip_src = HIP_MS_SSIM.read_text(encoding="utf-8")
        cls.dispatch_src = DISPATCH_STRATEGY.read_text(encoding="utf-8")
        cls.parity_test_src = METAL_PARITY_TEST.read_text(encoding="utf-8")
        cls.semantics_src = OPTION_SEMANTICS.read_text(encoding="utf-8")
        cls.semantics_test_src = OPTION_SEMANTICS_TEST.read_text(encoding="utf-8")
        cls.meson_src = MESON_TESTS.read_text(encoding="utf-8")
        cls.exact_boundary_docs = {
            path: path.read_text(encoding="utf-8")
            for path in (METAL_AGENTS, METRICS_DOC, RESEARCH_DOC, CHANGELOG_FRAGMENT)
        }

    def test_production_and_parity_wiring(self) -> None:
        self.assertEqual(metal_wiring_problems(self.metal_src, self.parity_test_src), [])

    def test_kernel_cites_its_actual_port_decision(self) -> None:
        header = "\n".join(self.metal_kernel_src.splitlines()[:12])
        self.assertIn("T8-2b / " + "ADR-" + "0490", header)
        self.assertNotIn("ADR-" + "0488", header)

    def test_operator_docs_name_the_exact_ceil_subsampled_boundary(self) -> None:
        for path, text in self.exact_boundary_docs.items():
            with self.subTest(path=path.relative_to(REPO_ROOT)):
                self.assertIn("351x351", text)

    def test_metal_options_match_cpu_and_gpu_siblings(self) -> None:
        expected_options = ("enable_lcs", "enable_db", "clip_db", "enable_chroma")
        for opt_name in expected_options:
            with self.subTest(option=opt_name):
                init = option_initializer(self.metal_src, opt_name)
                self.assertRegex(init, r"\.type\s*=\s*VMAF_OPT_TYPE_BOOL\b")
                self.assertRegex(init, r"\.default_val(\.b|\s*=\s*\{\s*\.b)\s*=\s*false")

    def test_all_sibling_backends_expose_exact_options(self) -> None:
        for opt_name in ("enable_lcs", "enable_db", "clip_db"):
            with self.subTest(option=opt_name):
                self.assertIn(f'"{opt_name}"', option_initializer(self.metal_src, opt_name))
                self.assertIn(f'"{opt_name}"', option_initializer(self.cpu_src, opt_name))
                self.assertIn(f'"{opt_name}"', option_initializer(self.sycl_src, opt_name))
                self.assertIn(f'"{opt_name}"', option_initializer(self.cuda_src, opt_name))
                self.assertIn(f'"{opt_name}"', option_initializer(self.hip_src, opt_name))

        # enable_chroma is exposed by CPU, SYCL, HIP, and Metal
        self.assertIn('"enable_chroma"', option_initializer(self.metal_src, "enable_chroma"))
        self.assertIn('"enable_chroma"', option_initializer(self.cpu_src, "enable_chroma"))
        self.assertIn('"enable_chroma"', option_initializer(self.sycl_src, "enable_chroma"))
        self.assertIn('"enable_chroma"', option_initializer(self.hip_src, "enable_chroma"))

    def test_provided_features_includes_chroma_channels(self) -> None:
        match = re.search(
            r"static\s+const\s+char\s*\*(\s*const\s*)?provided_features\[\]\s*=\s*\{([^}]+)\};",
            self.metal_src,
        )
        self.assertIsNotNone(match, "provided_features array missing in Metal source")
        features_block = match.group(2)
        for expected in ("float_ms_ssim", "float_ms_ssim_cb", "float_ms_ssim_cr"):
            self.assertIn(f'"{expected}"', features_block)

    def test_dispatch_strategy_table_includes_chroma_features(self) -> None:
        for expected in (
            "float_ms_ssim_metal",
            "float_ms_ssim",
            "float_ms_ssim_cb",
            "float_ms_ssim_cr",
        ):
            self.assertIn(f'"{expected}"', self.dispatch_src)

    def test_option_semantics_have_an_always_built_executable_oracle(self) -> None:
        self.assertIn("vmaf_metal_ms_ssim_max_db", self.semantics_src)
        self.assertIn("vmaf_metal_ms_ssim_active_planes", self.semantics_src)
        self.assertIn("vmaf_metal_ms_ssim_plane_dimensions", self.semantics_src)
        self.assertEqual(option_semantics_oracle_problems(self.semantics_test_src), [])
        self.assertIn("width == 176u && height == 177u", self.semantics_test_src)
        self.assertIn("width == 351u && height == 351u", self.semantics_test_src)
        self.assertIn("test_metal_ms_ssim_option_semantics = executable", self.meson_src)

    def test_sycl_option_help_matches_live_semantics(self) -> None:
        clip_db = option_initializer(self.sycl_src, "clip_db")
        self.assertIn("geometry-derived ceiling", clip_db)
        self.assertNotIn("clip linear", clip_db)

        enable_chroma = option_initializer(self.sycl_src, "enable_chroma")
        self.assertIn("dispatch for all active planes", enable_chroma)
        self.assertNotIn("Vulkan", enable_chroma)
        self.assertNotIn("v1 kernel", enable_chroma)

    def test_chroma_min_dim_check_enforces_176(self) -> None:
        # 5 scales with 11x11 filter require minimum dimension 176:
        # scale 0: >= 176, scale 1: >= 88, scale 2: >= 44, scale 3: >= 22, scale 4: >= 11
        # YUV420P uses ceil-halving, so 351x351 is the exact luma minimum that
        # yields 176x176 chroma. If either chroma axis is < 176, init rejects it.
        self.assertIn("check_chroma_min_dim", self.metal_src)
        self.assertIn(
            "min_dim = (unsigned)MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1u)", self.metal_src
        )
        self.assertIn("-EINVAL", self.metal_src)

    def test_emitter_wires_enable_db_and_max_db_for_luma_and_chroma(self) -> None:
        # Luma emission via vmaf_ms_ssim_emit_scores with s->enable_db, s->max_db
        self.assertIn("vmaf_ms_ssim_emit_scores", self.metal_src)
        self.assertIn("plane_scores[0], s->enable_db, s->max_db", self.metal_src)

        # Chroma emission via vmaf_ssim_emit_score_named with s->enable_db, s->max_db
        self.assertIn("vmaf_ssim_emit_score_named", self.metal_src)
        self.assertIn("plane_scores[plane], s->enable_db, s->max_db", self.metal_src)

        # Score validation via vmaf_ssim_prepare_score_named
        self.assertIn("vmaf_ssim_prepare_score_named", self.metal_src)

    def test_mutation_missing_atom_guard_is_rejected(self) -> None:
        mutated = self.metal_src.replace(
            "vmaf_feature_validate_finite_scores_named", "removed_finite_atom_guard", 1
        )
        self.assertTrue(metal_wiring_problems(mutated, self.parity_test_src))

    def test_mutation_unbounded_db_ceiling_is_rejected(self) -> None:
        mutated = self.metal_src.replace(
            "vmaf_metal_ms_ssim_max_db(s->clip_db, bpc, w, h)", "INFINITY", 1
        )
        self.assertTrue(metal_wiring_problems(mutated, self.parity_test_src))

    def test_mutation_luma_only_plane_count_is_rejected(self) -> None:
        mutated = self.metal_src.replace(
            "vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt)", "1u", 1
        )
        self.assertTrue(metal_wiring_problems(mutated, self.parity_test_src))

    def test_mutation_reused_option_dictionary_is_rejected(self) -> None:
        cpu_setup = function_body(self.parity_test_src, "static char *setup_cpu_float_ms_ssim")
        mutated_setup = cpu_setup.replace("make_options(options, opts)", "0", 1)
        mutated = self.parity_test_src.replace(cpu_setup, mutated_setup, 1)
        self.assertTrue(metal_wiring_problems(self.metal_src, mutated))

    def test_mutation_post_consumption_double_free_is_rejected(self) -> None:
        cpu_setup = function_body(self.parity_test_src, "static char *setup_cpu_float_ms_ssim")
        mutated_setup = cpu_setup.replace(
            "*opts = NULL;",
            "(void)vmaf_feature_dictionary_free(opts);\n    *opts = NULL;",
            1,
        )
        mutated = self.parity_test_src.replace(cpu_setup, mutated_setup, 1)
        self.assertTrue(metal_wiring_problems(self.metal_src, mutated))

    def test_mutation_yuv400_uses_raw_chroma_option_is_rejected(self) -> None:
        mutated = self.metal_src.replace("if (s->n_planes > 1u)", "if (s->enable_chroma)", 1)
        self.assertTrue(metal_wiring_problems(mutated, self.parity_test_src))

    def test_mutation_partial_atom_validation_is_rejected(self) -> None:
        mutated = self.metal_src.replace("atoms, 3u", "atoms, 1u", 1)
        self.assertTrue(metal_wiring_problems(mutated, self.parity_test_src))

    def test_mutation_float_equality_in_oracle_is_rejected(self) -> None:
        mutated = self.semantics_test_src.replace(
            "double_bits_equal(clipped, 105.0)", "clipped == 105.0", 1
        )
        self.assertTrue(option_semantics_oracle_problems(mutated))

    def test_mutation_each_atom_value_substitution_is_rejected(self) -> None:
        substitutions = (
            ("L", ".value = l_means[i]", ".value = c_means[i]"),
            ("C", ".value = c_means[i]", ".value = s_means[i]"),
            ("S", ".value = s_means[i]", ".value = l_means[i]"),
        )
        for atom, original, replacement in substitutions:
            with self.subTest(atom=atom):
                mutated = self.metal_src.replace(original, replacement, 1)
                self.assertTrue(metal_wiring_problems(mutated, self.parity_test_src))

    def test_mutation_each_atom_name_substitution_is_rejected(self) -> None:
        substitutions = (
            ("L", '"float_ms_ssim_l"', '"float_ms_ssim_c"'),
            ("C", '"float_ms_ssim_c"', '"float_ms_ssim_s"'),
            ("S", '"float_ms_ssim_s"', '"float_ms_ssim_l"'),
        )
        for atom, original, replacement in substitutions:
            with self.subTest(atom=atom):
                mutated = self.metal_src.replace(original, replacement, 1)
                self.assertTrue(metal_wiring_problems(mutated, self.parity_test_src))

    def test_mutation_each_atom_removal_is_rejected(self) -> None:
        atoms = (
            ("L", '{.name = "float_ms_ssim_l", .value = l_means[i]},'),
            ("C", '{.name = "float_ms_ssim_c", .value = c_means[i]},'),
            ("S", '{.name = "float_ms_ssim_s", .value = s_means[i]},'),
        )
        for atom, initializer in atoms:
            with self.subTest(atom=atom):
                mutated = self.metal_src.replace(initializer, "", 1)
                self.assertTrue(metal_wiring_problems(mutated, self.parity_test_src))

    def test_nasa_rule4_function_loc_limit(self) -> None:
        # Every function in float_ms_ssim_metal.mm must satisfy LOC <= 60
        lines = self.metal_src.splitlines()
        in_func = False
        func_start = 0
        func_name = ""
        brace_depth = 0
        seen_open = False

        for idx, line in enumerate(lines, 1):
            trimmed = line.strip()
            if (
                not in_func
                and re.match(
                    r"^(static\s+)?(int|void|float|double|const\s+char\s*\*)\b.*\(", trimmed
                )
                and not trimmed.endswith(";")
            ):
                in_func = True
                func_start = idx
                func_name = trimmed.split("(")[0].strip()
                brace_depth = 0
                seen_open = False

            if in_func:
                brace_depth += line.count("{") - line.count("}")
                if "{" in line:
                    seen_open = True
                if seen_open and brace_depth == 0:
                    loc = idx - func_start + 1
                    self.assertLessEqual(
                        loc,
                        60,
                        f"Function {func_name} at lines {func_start}-{idx} exceeds NASA Rule 4 limit (60 LOC): {loc} lines",
                    )
                    in_func = False


if __name__ == "__main__":
    unittest.main()
