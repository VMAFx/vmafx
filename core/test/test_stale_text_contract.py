#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Texts that describe behaviour must match the code they describe.

The 2026-10-03 documentation audit found help strings, header comments, option
descriptions, CI comments and a perf-gate page that said something the code did
not do. Each class below pins one of them to the code, so the text and the
behaviour cannot drift apart again. Where the text can be checked against the
behaviour itself (the VMAF_SYCL_NO_GRAPH advice), the test runs the code.

Device-free: reads sources, and compiles one small C++ translation unit.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class CliHelpDefaults(unittest.TestCase):
    """Defect 9: HIP and Metal are opt-in, so their help must not say `auto`."""

    def test_hip_and_metal_device_help_say_opt_in(self) -> None:
        # The usage text is generated from core/api/vmafx.toml (RC4 WP8).
        usage = read("core/tools/cli_options.gen.inc")
        for backend, flag in (("HIP", "hip"), ("Metal", "metal")):
            line = re.search(rf"--{flag}_device \$unsigned:[^\n]*\n[^\n]*\n", usage)
            self.assertIsNotNone(line, flag)
            text = line.group(0)
            self.assertNotIn("default: auto", text, flag)
            self.assertIn(f"opt-in: {backend} is off", text, flag)
            self.assertIn(f"--backend {flag}", text, flag)

    def test_the_code_really_skips_the_backend_without_the_flag(self) -> None:
        # The help is right only while the init functions keep returning early.
        runner = read("core/tools/vmaf.cpp")
        self.assertIn("if (c->hip_device < 0 || c->no_hip)", runner)
        self.assertIn("if (c->metal_device < 0 || c->no_metal)", runner)

    def test_the_cli_reference_has_no_note_apologising_for_the_help(self) -> None:
        self.assertNotIn("`--help` says HIP and Metal default", read("docs/usage/cli.md"))


class VplDefaultModel(unittest.TestCase):
    """Defect 10: vmaf_vpl's help and header named a model its code does not default to."""

    def test_default_comes_from_the_one_definition(self) -> None:
        source = read("core/tools/vmaf_vpl.c")
        self.assertNotIn("0.6.1", source)
        self.assertIn(".model_name = VMAF_DEFAULT_MODEL_VERSION", source)
        self.assertRegex(source, r'\(default: " VMAF_DEFAULT_MODEL_VERSION\s*"\)')


HARNESS = r"""
#include <cstdarg>
#include <cstdio>
#include "sycl/dispatch_strategy.h"
#include "log.h"
extern "C" void vmaf_log(enum VmafLogLevel, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fputs("LOG:", stdout);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}
int main()
{
    for (int i = 0; i < 3; ++i)
        std::printf("STRATEGY:%d\n",
                    (int)vmaf_sycl_select_strategy("float_vif", nullptr, 1920, 1080, false));
    return 0;
}
"""


class SyclNoGraphDeprecation(unittest.TestCase):
    """Defect 11: the deprecation message must name a setting that works."""

    DIRECT, GRAPH = "0", "1"

    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            raise RuntimeError("no C++ compiler on PATH; the test builds dispatch_strategy.cpp")
        cls.tmp = tempfile.TemporaryDirectory()
        work = Path(cls.tmp.name)
        (work / "main.cpp").write_text(HARNESS, encoding="utf-8")
        cls.binary = work / "harness"
        sources = ("core/src/sycl/dispatch_strategy.cpp", "core/src/gpu_dispatch_env.cpp")
        command = [
            compiler,
            "-std=c++23",
            f"-I{ROOT / 'core/include'}",
            f"-I{ROOT / 'core/src'}",
            f"-I{ROOT / 'core'}",
            str(work / "main.cpp"),
            *(str(ROOT / source) for source in sources),
            "-o",
            str(cls.binary),
        ]
        # the resolved host compiler and fixed arguments, no shell
        subprocess.run(  # noqa: S603
            command, check=True, capture_output=True, text=True, timeout=300
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, **env: str) -> tuple[list[str], list[str]]:
        clean = {k: v for k, v in os.environ.items() if not k.startswith("VMAF_")}
        out = subprocess.run(  # noqa: S603 -- the harness this test just built, no shell
            [str(self.binary)],
            env={**clean, **env},
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        ).stdout.splitlines()
        logs = [line for line in out if line.startswith("LOG:")]
        strategies = [line.split(":")[1] for line in out if line.startswith("STRATEGY:")]
        return logs, strategies

    def test_no_graph_warns_once_and_forces_direct(self) -> None:
        logs, strategies = self.run_harness(VMAF_SYCL_NO_GRAPH="1")
        self.assertEqual(strategies, [self.DIRECT] * 3)
        self.assertEqual(len(logs), 1, "the warning must be printed once per process")

    def test_the_advice_in_the_message_works(self) -> None:
        logs, _ = self.run_harness(VMAF_SYCL_NO_GRAPH="1")
        message = logs[0]
        self.assertNotIn("USE_GRAPH=false", message)
        advised = re.search(r"VMAF_SYCL_DISPATCH=<feature>:(\w+)", message)
        self.assertIsNotNone(advised, message)
        self.assertEqual(advised.group(1), "direct")
        # Unforced, a 1080p frame takes graph replay; the advised setting changes that.
        _, default = self.run_harness()
        _, advice = self.run_harness(VMAF_SYCL_DISPATCH="float_vif:direct")
        self.assertEqual(default, [self.GRAPH] * 3)
        self.assertEqual(advice, [self.DIRECT] * 3)

    def test_the_old_advice_changes_nothing(self) -> None:
        # What the message used to recommend: no effect, so it was no migration path.
        logs, strategies = self.run_harness(VMAF_SYCL_USE_GRAPH="false")
        self.assertEqual(logs, [])
        self.assertEqual(strategies, [self.GRAPH] * 3)

    def test_a_per_feature_setting_beats_the_deprecated_one(self) -> None:
        _, strategies = self.run_harness(
            VMAF_SYCL_NO_GRAPH="1", VMAF_SYCL_DISPATCH="float_vif:graph"
        )
        self.assertEqual(strategies, [self.GRAPH] * 3)


class TadDisabledText(unittest.TestCase):
    """Defect 18: without enable_rust_features the extractor does not exist."""

    def test_comments_do_not_promise_enosys_for_a_disabled_build(self) -> None:
        meson = read("core/src/meson.build")
        self.assertNotIn("--feature tad returns -ENOSYS", meson)
        self.assertNotIn("(they get -ENOSYS stubs instead)", meson)
        tad = read("core/src/feature/tad_rust.c")
        self.assertNotIn("vmaf_fex_tad is still registered", tad)
        self.assertIn("problem loading feature extractor: tad", tad)

    def test_the_build_graph_really_omits_the_tu_and_the_registration(self) -> None:
        meson = read("core/src/meson.build")
        self.assertRegex(
            meson,
            r"rust_tad_direct_sources = \[\]\nif is_rust_enabled\n"
            r"\s+rust_tad_direct_sources = \[feature_src_dir \+ 'tad_rust.c'\]",
        )
        registry = read("core/src/feature/feature_extractor.cpp")
        self.assertIn("#if HAVE_RUST_TAD\n    /* ADR-0707: TAD Rust pilot", registry)


class MesonOptionDescriptions(unittest.TestCase):
    """Defect 19: option descriptions must not call a live backend a scaffold."""

    STALE = (
        "until the runtime PR lands",
        "the scaffold has no SDK requirement",
        "first eight feature kernels",
        "T8-1 through T8-1k",
        "remaining metrics land as follow-up kernel batches",
        "scaffold / T5-2",
    )

    @staticmethod
    def description(options: str, name: str) -> str:
        block = re.search(rf"option\('{name}',.*?description: '([^']*)'\)", options, re.S)
        assert block is not None, name
        return block.group(1)

    def test_no_description_carries_a_retired_status(self) -> None:
        options = read("core/meson_options.txt")
        for name in ("enable_hip", "enable_hipcc", "enable_metal", "enable_mcp"):
            text = self.description(options, name)
            for stale in self.STALE:
                self.assertNotIn(stale, text, f"{name}: {stale}")

    def test_the_descriptions_state_what_the_options_do(self) -> None:
        options = read("core/meson_options.txt")
        self.assertIn("-ENOSYS", self.description(options, "enable_hip"))
        self.assertIn("enable_hipcc=true", self.description(options, "enable_hip"))
        self.assertIn("init()", self.description(options, "enable_hipcc"))
        self.assertIn(
            "Serves list_features and compute_vmaf", self.description(options, "enable_mcp")
        )

    def test_the_autodispatch_option_and_its_build_comment_cite_one_adr(self) -> None:
        # ADR-0623 added the option; ADR-0642 (an unrelated AI-defaults ADR) was cited by mistake.
        option = self.description(
            read("core/meson_options.txt"), "enable_float_vif_hip_autodispatch"
        )
        self.assertIn("ADR-0623", option)
        self.assertIn("ADR-0623: gate VMAF_FEATURE_EXTRACTOR_HIP", read("core/src/meson.build"))
        self.assertIn("autodispatch", read("docs/adr/0623-scaffold-audit-p2-half-finished.md"))


class PictureAllocHeader(unittest.TestCase):
    """Defect 20: the header described a 32-byte stride and an uninitialised buffer."""

    def test_header_states_the_implemented_layout(self) -> None:
        header = read("core/include/libvmaf/picture.h")
        alloc = header[header.index("@brief Allocate a planar picture buffer") :]
        alloc = alloc[: alloc.index("VMAF_EXPORT int vmaf_picture_alloc")]
        self.assertNotIn("32-byte", alloc)
        self.assertNotIn("uninitialised", alloc)
        self.assertIn("64 samples", alloc)
        self.assertIn("zero-filled", alloc)

    def test_the_header_and_the_implementation_agree(self) -> None:
        source = read("core/src/picture.c")
        self.assertIn("#define DATA_ALIGN 64", source)
        self.assertIn("memset(data, 0, pic_size)", source)

    def test_the_api_page_has_no_note_contradicting_the_header(self) -> None:
        page = read("docs/api/pictures.md")
        self.assertNotIn("says strides round to a", page)


class McpHeaderTransport(unittest.TestCase):
    """Defect 21: the stdio transport is newline-delimited, not LSP-framed."""

    def test_header_describes_newline_delimited_stdio(self) -> None:
        header = read("core/include/libvmaf/libvmaf_mcp.h")
        self.assertNotIn("LSP-framed", header)
        self.assertIn("newline-delimited JSON-RPC on a caller-supplied fd pair", header)
        self.assertNotIn("lands via T5-2b in a", header)
        self.assertNotIn("while the runtime\n *         is still unwired", header)

    def test_the_transport_really_frames_by_newline(self) -> None:
        source = read("core/src/mcp/transport_stdio.c")
        self.assertIn("newline-delimited JSON-RPC", source)
        self.assertNotIn("Content-Length", source)


class BackendHeaderSymbols(unittest.TestCase):
    """Defect 22: headers must not name Vulkan symbols or accessors that do not exist."""

    FILES = (
        "core/include/libvmaf/libvmaf_hip.h",
        "core/include/libvmaf/libvmaf_metal.h",
        "core/src/hip/common.h",
        "core/src/metal/common.h",
    )

    def test_no_file_names_a_vulkan_symbol_or_scaffold(self) -> None:
        for relative in self.FILES:
            self.assertNotRegex(read(relative), r"(?i)vulkan", relative)

    def test_every_backticked_function_exists_in_the_tree(self) -> None:
        defined = "\n".join(
            path.read_text(encoding="utf-8", errors="replace")
            for pattern in ("*.c", "*.cpp", "*.h", "*.mm")
            for path in (ROOT / "core").rglob(pattern)
            if not any(part.startswith("build") for part in path.relative_to(ROOT).parts)
        )
        for relative in self.FILES:
            for name in set(re.findall(r"`(vmaf_[a-z0-9_]+)\(\)`", read(relative))):
                self.assertRegex(defined, rf"\b{name}\s*\(", f"{relative} names missing {name}()")


class CiTexts(unittest.TestCase):
    """Defects 37, 38: CI comments must name the real pin and the real rule."""

    def test_cuda_comments_follow_the_pin_not_a_version_number(self) -> None:
        workflow = read(".github/workflows/libvmaf-build-matrix.yml")
        pin = re.search(r'CUDA_VERSION="([0-9.]+)"', read("build-config.env"))
        self.assertIsNotNone(pin)
        for stale in (
            "Pinned to CUDA 13.3.1",
            "Same pin as the Linux CUDA legs: CUDA 13.3.1",
            "CUDA 13.3.1, the repository pin",
        ):
            self.assertNotIn(stale, workflow)
        self.assertIn("CUDA_VERSION in build-config.env", workflow)

    def test_the_surface_gate_cites_hard_rule_11_and_adr_0409(self) -> None:
        script = read("scripts/ci/ffmpeg-patches-surface-check.sh")
        self.assertNotIn("§12 r14", script)
        self.assertNotIn("ADR-" + "0186", script)
        self.assertNotIn("ADR-" + "0356", script)
        self.assertIn("hard rule 11", script)
        self.assertIn("ADR-0409", script)
        rules = read("docs/development/agent-hard-rules.md")
        self.assertRegex(rules, r"11\. Every PR that touches a libvmaf public surface")


class FuzzReadme(unittest.TestCase):
    """Defect 39: the fuzz README may only describe files and flags that exist."""

    README = "core/test/fuzz/README.md"

    def test_no_invented_helper_or_directory(self) -> None:
        text = read(self.README)
        self.assertNotIn("known_assert_in_input", text)
        self.assertNotIn("cli_parse_known_crashes", text)
        self.assertNotIn("enable_vulkan", text)
        self.assertFalse((ROOT / "core/test/fuzz/cli_parse_known_crashes").exists())
        self.assertNotIn("known_assert_in_input", read("core/test/fuzz/fuzz_cli_parse.c"))

    def test_every_repo_path_the_readme_names_exists(self) -> None:
        text = read(self.README)
        for name in re.findall(
            r"`((?:[a-z_0-9]+_(?:corpus|known_crashes)/)|(?:fuzz_[a-z_0-9]+\.c))`", text
        ):
            self.assertTrue((ROOT / "core/test/fuzz" / name.rstrip("/")).exists(), name)
        for link in re.findall(r"\]\(([^)#]+)(?:#[^)]*)?\)", text):
            if "://" not in link:
                self.assertTrue((ROOT / "core/test/fuzz" / link).resolve().exists(), link)

    def test_the_build_commands_match_the_nightly_job(self) -> None:
        text = read(self.README)
        workflow = read(".github/workflows/fuzz.yml")
        self.assertIn("meson setup build-fuzz core", text)
        self.assertIn("meson setup build-fuzz core", workflow)
        self.assertIn("ninja -C build-fuzz test/fuzz/", text)
        self.assertNotIn("build-fuzz/core/test/fuzz", text)


class PerfGatePage(unittest.TestCase):
    """Defect 40: the page must say the gate is not wired while nothing wires it."""

    SCRIPTS = ("check-regression.py", "bench-multi-resolution.sh")

    def callers(self) -> list[str]:
        found = []
        roots = [ROOT / ".github", ROOT / "Makefile", ROOT / "lefthook.yml"]
        for root in roots:
            paths = [root] if root.is_file() else sorted(root.rglob("*")) if root.exists() else []
            for path in paths:
                if path.is_file() and path.suffix in {".yml", ".yaml", ".sh", "", ".mk", ".py"}:
                    text = path.read_text(encoding="utf-8", errors="replace")
                    if any(script in text for script in self.SCRIPTS):
                        found.append(str(path.relative_to(ROOT)))
        return found

    def test_page_says_not_wired_and_names_the_owner(self) -> None:
        page = read("docs/development/perf-gate.md")
        self.assertIn("not wired into CI", page)
        self.assertIn("RC8", page)
        self.assertIn("ADR-1490", page)
        for stale in (
            "perf-regression-current-run",
            "`perf-regression` CI job",
            "Current mode: advisory",
        ):
            self.assertNotIn(stale, page)

    def test_nothing_calls_the_gate_yet(self) -> None:
        # When someone wires the gate, this fails and the page above must be rewritten with it.
        self.assertEqual(self.callers(), [])


class StateLedgerMetalRow(unittest.TestCase):
    """Defect 41: the Metal gate row must agree with the gate's own table."""

    def test_the_gate_has_a_metal_backend(self) -> None:
        gate = read("scripts/ci/cross_backend_parity_gate.py")
        self.assertRegex(gate, r'"metal": "_metal"')
        self.assertRegex(gate, r'"metal": "--metal_device"')

    def test_the_row_does_not_say_it_has_none(self) -> None:
        row = next(
            line
            for line in read("docs/state.md").splitlines()
            if line.startswith("| **T-GATE-NO-METAL-BACKEND-2026-10-02**")
        )
        self.assertNotIn("has no `metal` backend", row)
        self.assertNotIn("runs `cpu`, `cuda`, `sycl` and `hip` (`BACKEND_SUFFIX`)", row)
        self.assertIn("`metal` entries of `BACKEND_SUFFIX`", row)


if __name__ == "__main__":
    unittest.main()
