#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Deterministic regression contract for CUDA fatbin and HIP HSACO header dependencies (ADR-1320)."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE_SRC = ROOT / "core" / "src"
MESON_BUILD = CORE_SRC / "meson.build"
BUILD_DIR = Path(os.environ.get("VMAFX_DEVICE_DEP_BUILD_DIR", ROOT / "build")).resolve()

EXPECTED_CUDA_TARGET_COUNT = 22
EXPECTED_HIP_TARGET_COUNT = 22


def run_ninja(ninja_exe: str, args: list[str], cwd: str | Path) -> subprocess.CompletedProcess[str]:
    """Run the PATH-resolved Ninja binary with static, bounded test arguments."""
    # ADR-1320: shutil.which() resolves the executable; callers supply only
    # fixed test arguments and a test-owned or configured build directory.
    return subprocess.run(  # noqa: S603
        [ninja_exe, *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=True,
        timeout=10,
    )


def refresh_ninja_manifest(ninja_exe: str) -> None:
    """Regenerate stale Meson metadata before inspecting or dry-running it."""
    run_ninja(ninja_exe, ["-C", str(BUILD_DIR), "build.ninja"], ROOT)


def meson_file_list(content: str, variable: str) -> set[Path]:
    """Return source-tree paths declared by one Meson files() list."""
    match = re.search(rf"{variable} = files\((.*?)\)", content, re.DOTALL)
    if match is None:
        return set()
    return {
        (CORE_SRC / rel_path).resolve()
        for rel_path in re.findall(r"['\"]([^'\"]+\.(?:h|cuh|hip))['\"]", match.group(1))
    }


def meson_kernel_sources(content: str, variable: str, suffix: str) -> list[Path]:
    """Return device sources registered in a Meson kernel-target dictionary."""
    match = re.search(rf"{variable} = \{{(.*?)^    \}}", content, re.DOTALL | re.MULTILINE)
    if match is None:
        return []
    paths = re.findall(rf"feature_src_dir \+ '([^']+\.{suffix})'", match.group(1))
    return [(CORE_SRC / "feature" / rel_path).resolve() for rel_path in paths]


def repo_include_closure(sources: list[Path], include_dirs: list[Path]) -> set[Path]:
    """Resolve the quoted, repository-local include closure for source files."""
    pending = [source.resolve() for source in sources]
    seen: set[Path] = set()
    while pending:
        source = pending.pop()
        if source in seen:
            continue
        seen.add(source)
        for include in re.findall(
            r'^\s*#\s*include\s*"([^"]+)"', source.read_text(encoding="utf-8"), re.MULTILINE
        ):
            candidates = [
                source.parent / include,
                *(directory / include for directory in include_dirs),
            ]
            resolved = next(
                (candidate.resolve() for candidate in candidates if candidate.exists()), None
            )
            if resolved is not None and (resolved == ROOT or ROOT in resolved.parents):
                pending.append(resolved)
    return seen - {source.resolve() for source in sources}


class DeviceTargetHeaderDependencyContractTest(unittest.TestCase):
    """Verifies durable dependency tracking for CUDA fatbins and HIP HSACO targets."""

    def test_reproduce_red_untracked_target_ignores_header_changes(self) -> None:
        """RED proof: an un-tracked custom target does not rebuild when an included header changes."""
        ninja_exe = shutil.which("ninja")
        if not ninja_exe:
            self.skipTest("ninja not available on PATH")

        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            source_file = tmppath / "kernel.cu"
            header_file = tmppath / "integer_adm_cuda.h"
            output_file = tmppath / "kernel.fatbin"
            ninja_file = tmppath / "build.ninja"

            source_file.write_text('#include "integer_adm_cuda.h"\n', encoding="utf-8")
            header_file.write_text("struct AdmFixedParametersCuda { int x; };\n", encoding="utf-8")

            # Defective rule: only input kernel.cu is declared; no depfile, no depend_files.
            ninja_content = (
                "rule compile\n"
                "  command = touch $out\n"
                f"build {output_file.name}: compile {source_file.name}\n"
            )
            ninja_file.write_text(ninja_content, encoding="utf-8")

            # Initial build
            res = run_ninja(ninja_exe, [], tmpdir)
            self.assertTrue(output_file.exists())

            # Header-only change: modify header mtime to the future
            future_mtime = time.time() + 10.0
            os.utime(header_file, (future_mtime, future_mtime))

            # Incremental build fails to notice header change -> RED reproduction
            res = run_ninja(ninja_exe, [], tmpdir)
            self.assertIn("ninja: no work to do.", res.stdout)

    def test_green_tracked_depend_files_triggers_rebuild(self) -> None:
        """GREEN proof: explicit depend_files causes Ninja to trigger an incremental rebuild."""
        ninja_exe = shutil.which("ninja")
        if not ninja_exe:
            self.skipTest("ninja not available on PATH")

        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            source_file = tmppath / "kernel.cu"
            header_file = tmppath / "integer_adm_cuda.h"
            output_file = tmppath / "kernel.fatbin"
            ninja_file = tmppath / "build.ninja"

            source_file.write_text('#include "integer_adm_cuda.h"\n', encoding="utf-8")
            header_file.write_text("struct AdmFixedParametersCuda { int x; };\n", encoding="utf-8")

            # Fixed rule: header is listed as an order-independent dependency (| header)
            ninja_content = (
                "rule compile\n"
                "  command = touch $out\n"
                f"build {output_file.name}: compile {source_file.name} | {header_file.name}\n"
            )
            ninja_file.write_text(ninja_content, encoding="utf-8")

            # Initial build
            res = run_ninja(ninja_exe, [], tmpdir)
            self.assertTrue(output_file.exists())

            # Header-only change: advance header mtime
            future_mtime = time.time() + 10.0
            os.utime(header_file, (future_mtime, future_mtime))

            # Incremental dry-run or build detects rebuild -> GREEN
            res = run_ninja(ninja_exe, ["-n"], tmpdir)
            self.assertNotIn("ninja: no work to do.", res.stdout)
            self.assertIn(output_file.name, res.stdout)

    def test_green_tracked_compiler_depfile_triggers_rebuild(self) -> None:
        """GREEN proof: compiler-emitted depfile causes Ninja to trigger an incremental rebuild."""
        ninja_exe = shutil.which("ninja")
        if not ninja_exe:
            self.skipTest("ninja not available on PATH")

        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            source_file = tmppath / "kernel.cu"
            header_file = tmppath / "integer_adm_cuda.h"
            output_file = tmppath / "kernel.fatbin"
            dep_file = tmppath / "kernel.fatbin.d"
            ninja_file = tmppath / "build.ninja"

            source_file.write_text('#include "integer_adm_cuda.h"\n', encoding="utf-8")
            header_file.write_text("struct AdmFixedParametersCuda { int x; };\n", encoding="utf-8")

            # Fixed rule with depfile
            ninja_content = (
                "rule compile_dep\n"
                f"  depfile = {dep_file.name}\n"
                f'  command = echo "{output_file.name}: {header_file.name}" > {dep_file.name} && touch $out\n'
                f"build {output_file.name}: compile_dep {source_file.name}\n"
            )
            ninja_file.write_text(ninja_content, encoding="utf-8")

            # Initial build (generates output and depfile)
            res = run_ninja(ninja_exe, [], tmpdir)
            self.assertTrue(output_file.exists())
            self.assertTrue(dep_file.exists())

            # Header-only change: advance header mtime
            future_mtime = time.time() + 10.0
            os.utime(header_file, (future_mtime, future_mtime))

            # Incremental dry-run detects rebuild via depfile -> GREEN
            res = run_ninja(ninja_exe, ["-n"], tmpdir)
            self.assertNotIn("ninja: no work to do.", res.stdout)
            self.assertIn(output_file.name, res.stdout)

    def test_meson_build_cuda_and_hip_declarations(self) -> None:
        """Static verification that core/src/meson.build configures durable dependencies."""
        content = MESON_BUILD.read_text(encoding="utf-8")

        # 1. Verify CUDA shared header list declaration
        self.assertIn("cuda_kernel_shared_headers = files(", content)
        self.assertIn("'feature/cuda/integer_adm_cuda.h'", content)
        self.assertIn("'cuda/common.h'", content)
        self.assertIn("'cuda/cuda_helper.cuh'", content)

        # 2. Verify HIP shared header list declaration
        self.assertIn("hip_kernel_shared_headers = files(", content)
        self.assertIn("'feature/hip/integer_adm_hip.h'", content)
        self.assertIn("'hip/common.h'", content)

        # 3. Verify depend_files wiring on custom targets
        self.assertIn("depend_files : cuda_kernel_shared_headers", content)
        self.assertIn("depend_files: hip_kernel_shared_headers", content)
        self.assertIn("cuda_kernel_shared_headers += [config_h_target]", content)

        # 4. Verify Windows depfile conditional
        self.assertIn(
            "host_machine.system() == 'windows' ? '' : '@0@.fatbin.d'.format(name)", content
        )

        # 5. Verify all declared shared headers physically exist on disk
        cuda_headers_match = re.search(
            r"cuda_kernel_shared_headers = files\((.*?)\)", content, re.DOTALL
        )
        self.assertIsNotNone(cuda_headers_match)
        cuda_header_lines = [
            m.group(1)
            for m in re.finditer(r"['\"]([^'\"]+\.(?:h|cuh))['\"]", cuda_headers_match.group(1))
        ]
        self.assertGreaterEqual(len(cuda_header_lines), 20)
        for rel_path in cuda_header_lines:
            header_path = CORE_SRC / rel_path
            self.assertTrue(
                header_path.exists(), f"CUDA shared header does not exist: {header_path}"
            )

        hip_headers_match = re.search(
            r"hip_kernel_shared_headers = files\((.*?)\)", content, re.DOTALL
        )
        self.assertIsNotNone(hip_headers_match)
        hip_header_lines = [
            m.group(1)
            for m in re.finditer(r"['\"]([^'\"]+\.(?:h|hip))['\"]", hip_headers_match.group(1))
        ]
        self.assertGreaterEqual(len(hip_header_lines), 20)
        for rel_path in hip_header_lines:
            header_path = CORE_SRC / rel_path
            self.assertTrue(
                header_path.exists(), f"HIP shared header does not exist: {header_path}"
            )

    def test_cuda_windows_fallback_covers_repo_include_closure(self) -> None:
        """CUDA's no-depfile Windows path must list every repo-local included header."""
        content = MESON_BUILD.read_text(encoding="utf-8")
        sources = meson_kernel_sources(content, "cuda_cu_sources", "cu")
        self.assertEqual(len(sources), EXPECTED_CUDA_TARGET_COUNT)
        include_dirs = [
            CORE_SRC,
            CORE_SRC / "feature",
            CORE_SRC / "cuda",
            ROOT / "core" / "include",
        ]
        closure = repo_include_closure(sources, include_dirs)
        declared = meson_file_list(content, "cuda_kernel_shared_headers")
        missing = sorted(path.relative_to(ROOT) for path in closure - declared)
        self.assertEqual(
            missing, [], f"CUDA depend_files misses repo-local include closure: {missing}"
        )

    def test_hip_explicit_headers_cover_repo_include_closure(self) -> None:
        """HIP's explicit dependencies must cover its repo-local include closure."""
        content = MESON_BUILD.read_text(encoding="utf-8")
        sources = meson_kernel_sources(content, "hip_kernel_sources", "hip")
        self.assertEqual(len(sources), EXPECTED_HIP_TARGET_COUNT)
        include_dirs = [CORE_SRC, CORE_SRC / "feature", CORE_SRC / "hip", ROOT / "core" / "include"]
        closure = repo_include_closure(sources, include_dirs)
        declared = meson_file_list(content, "hip_kernel_shared_headers")
        missing = sorted(path.relative_to(ROOT) for path in closure - declared)
        self.assertEqual(
            missing, [], f"HIP depend_files misses repo-local include closure: {missing}"
        )

    def test_live_ninja_manifest_header_dependencies(self) -> None:
        """Verifies build/build.ninja binds shared headers to fatbin and hsaco targets."""
        ninja_exe = shutil.which("ninja")
        build_ninja = BUILD_DIR / "build.ninja"
        if not build_ninja.exists():
            self.skipTest("build/build.ninja not present (skipping live manifest test)")
        if ninja_exe:
            refresh_ninja_manifest(ninja_exe)

        content = build_ninja.read_text(encoding="utf-8")

        # Verify whichever device backends this build configured. CPU-only
        # builds intentionally contain neither target family.
        cuda_fatbin_matches = re.findall(
            r"build src/([a-zA-Z0-9_]+)\.fatbin:\s+CUSTOM_COMMAND_DEP\s+([^|\n]+)\|\s+([^\n]+)",
            content,
        )
        hip_hsaco_matches = re.findall(
            r"build src/([a-zA-Z0-9_]+)\.hsaco:\s+CUSTOM_COMMAND_DEP\s+([^|\n]+)\|\s+([^\n]+)",
            content,
        )
        if not cuda_fatbin_matches and not hip_hsaco_matches:
            self.skipTest("build/build.ninja has no CUDA or HIP device targets")

        if cuda_fatbin_matches:
            self.assertEqual(len(cuda_fatbin_matches), EXPECTED_CUDA_TARGET_COUNT)
            for target_name, _inputs, dependencies in cuda_fatbin_matches:
                self.assertIn(
                    "integer_adm_cuda.h",
                    dependencies,
                    f"CUDA target {target_name} missing integer_adm_cuda.h: {dependencies}",
                )

        if hip_hsaco_matches:
            self.assertEqual(len(hip_hsaco_matches), EXPECTED_HIP_TARGET_COUNT)
            for target_name, _inputs, dependencies in hip_hsaco_matches:
                self.assertIn(
                    "integer_adm_hip.h",
                    dependencies,
                    f"HIP target {target_name} missing integer_adm_hip.h: {dependencies}",
                )

    def test_live_incremental_rebuild_dry_run_on_header_touch(self) -> None:
        """Verifies Ninja plans an incremental rebuild when a shared header is modified."""
        ninja_exe = shutil.which("ninja")
        build_ninja = BUILD_DIR / "build.ninja"
        if not ninja_exe or not build_ninja.exists():
            self.skipTest("ninja or build/build.ninja not available")

        refresh_ninja_manifest(ninja_exe)
        manifest = build_ninja.read_text(encoding="utf-8")
        has_cuda = "build src/adm_cm.fatbin:" in manifest
        has_hip = "build src/adm_cm.hsaco:" in manifest
        if not has_cuda and not has_hip:
            self.skipTest("build/build.ninja has no CUDA or HIP device targets")

        cuda_header = CORE_SRC / "feature" / "cuda" / "integer_adm_cuda.h"
        hip_header = CORE_SRC / "feature" / "hip" / "integer_adm_hip.h"
        self.assertTrue(cuda_header.exists())
        self.assertTrue(hip_header.exists())

        orig_cuda_stat = cuda_header.stat()
        orig_hip_stat = hip_header.stat()

        try:
            future_mtime = time.time() + 10.0
            if has_cuda:
                os.utime(cuda_header, (future_mtime, future_mtime))
                res = run_ninja(
                    ninja_exe,
                    ["-C", str(BUILD_DIR), "-n", "src/adm_cm.fatbin"],
                    ROOT,
                )
                self.assertNotIn("ninja: no work to do.", res.stdout)
                self.assertIn("cu_ptx_target_adm_cm", res.stdout)

            if has_hip:
                os.utime(hip_header, (future_mtime, future_mtime))
                res = run_ninja(
                    ninja_exe,
                    ["-C", str(BUILD_DIR), "-n", "src/adm_cm.hsaco"],
                    ROOT,
                )
                self.assertNotIn("ninja: no work to do.", res.stdout)
                self.assertIn("hip_hsaco_adm_cm", res.stdout)
        finally:
            # Restore exact timestamps so this probe does not perturb later builds.
            os.utime(cuda_header, ns=(orig_cuda_stat.st_atime_ns, orig_cuda_stat.st_mtime_ns))
            os.utime(hip_header, ns=(orig_hip_stat.st_atime_ns, orig_hip_stat.st_mtime_ns))


if __name__ == "__main__":
    unittest.main()
