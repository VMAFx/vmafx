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
BUILD_DIR = ROOT / "build"

EXPECTED_CUDA_TARGET_COUNT = 22
EXPECTED_HIP_TARGET_COUNT = 22


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
            res = subprocess.run([ninja_exe], cwd=tmpdir, capture_output=True, text=True, check=True)
            self.assertTrue(output_file.exists())

            # Header-only change: modify header mtime to the future
            future_mtime = time.time() + 10.0
            os.utime(header_file, (future_mtime, future_mtime))

            # Incremental build fails to notice header change -> RED reproduction
            res = subprocess.run([ninja_exe], cwd=tmpdir, capture_output=True, text=True, check=True)
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
            res = subprocess.run([ninja_exe], cwd=tmpdir, capture_output=True, text=True, check=True)
            self.assertTrue(output_file.exists())

            # Header-only change: advance header mtime
            future_mtime = time.time() + 10.0
            os.utime(header_file, (future_mtime, future_mtime))

            # Incremental dry-run or build detects rebuild -> GREEN
            res = subprocess.run([ninja_exe, "-n"], cwd=tmpdir, capture_output=True, text=True, check=True)
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
                f"  command = echo \"{output_file.name}: {header_file.name}\" > {dep_file.name} && touch $out\n"
                f"build {output_file.name}: compile_dep {source_file.name}\n"
            )
            ninja_file.write_text(ninja_content, encoding="utf-8")

            # Initial build (generates output and depfile)
            res = subprocess.run([ninja_exe], cwd=tmpdir, capture_output=True, text=True, check=True)
            self.assertTrue(output_file.exists())
            self.assertTrue(dep_file.exists())

            # Header-only change: advance header mtime
            future_mtime = time.time() + 10.0
            os.utime(header_file, (future_mtime, future_mtime))

            # Incremental dry-run detects rebuild via depfile -> GREEN
            res = subprocess.run([ninja_exe, "-n"], cwd=tmpdir, capture_output=True, text=True, check=True)
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

        # 4. Verify Windows depfile conditional
        self.assertIn("host_machine.system() == 'windows' ? '' : '@0@.fatbin.d'.format(name)", content)

        # 5. Verify all declared shared headers physically exist on disk
        cuda_headers_match = re.search(
            r"cuda_kernel_shared_headers = files\((.*?)\)", content, re.DOTALL
        )
        self.assertIsNotNone(cuda_headers_match)
        cuda_header_lines = [
            m.group(1) for m in re.finditer(r"['\"]([^'\"]+\.(?:h|cuh))['\"]", cuda_headers_match.group(1))
        ]
        self.assertGreaterEqual(len(cuda_header_lines), 20)
        for rel_path in cuda_header_lines:
            header_path = CORE_SRC / rel_path
            self.assertTrue(header_path.exists(), f"CUDA shared header does not exist: {header_path}")

        hip_headers_match = re.search(
            r"hip_kernel_shared_headers = files\((.*?)\)", content, re.DOTALL
        )
        self.assertIsNotNone(hip_headers_match)
        hip_header_lines = [
            m.group(1) for m in re.finditer(r"['\"]([^'\"]+\.(?:h|hip))['\"]", hip_headers_match.group(1))
        ]
        self.assertGreaterEqual(len(hip_header_lines), 20)
        for rel_path in hip_header_lines:
            header_path = CORE_SRC / rel_path
            self.assertTrue(header_path.exists(), f"HIP shared header does not exist: {header_path}")

    def test_live_ninja_manifest_header_dependencies(self) -> None:
        """Verifies build/build.ninja binds shared headers to fatbin and hsaco targets."""
        build_ninja = BUILD_DIR / "build.ninja"
        if not build_ninja.exists():
            self.skipTest("build/build.ninja not present (skipping live manifest test)")

        content = build_ninja.read_text(encoding="utf-8")

        # Verify CUDA fatbin targets have header dependencies
        cuda_fatbin_matches = re.findall(r"build src/([a-zA-Z0-9_]+)\.fatbin:\s+CUSTOM_COMMAND_DEP\s+([^|\n]+)\|\s+([^\n]+)", content)
        self.assertEqual(
            len(cuda_fatbin_matches),
            EXPECTED_CUDA_TARGET_COUNT,
            f"Expected {EXPECTED_CUDA_TARGET_COUNT} CUDA fatbin targets in build.ninja",
        )
        for target_name, inputs, order_deps in cuda_fatbin_matches:
            self.assertIn(
                "integer_adm_cuda.h",
                order_deps,
                f"CUDA target {target_name} missing integer_adm_cuda.h in dependencies: {order_deps}",
            )

        # Verify HIP hsaco targets have header dependencies
        hip_hsaco_matches = re.findall(r"build src/([a-zA-Z0-9_]+)\.hsaco:\s+CUSTOM_COMMAND_DEP\s+([^|\n]+)\|\s+([^\n]+)", content)
        self.assertEqual(
            len(hip_hsaco_matches),
            EXPECTED_HIP_TARGET_COUNT,
            f"Expected {EXPECTED_HIP_TARGET_COUNT} HIP hsaco targets in build.ninja",
        )
        for target_name, inputs, order_deps in hip_hsaco_matches:
            self.assertIn(
                "integer_adm_hip.h",
                order_deps,
                f"HIP target {target_name} missing integer_adm_hip.h in dependencies: {order_deps}",
            )

    def test_live_incremental_rebuild_dry_run_on_header_touch(self) -> None:
        """Verifies Ninja plans an incremental rebuild when a shared header is modified."""
        ninja_exe = shutil.which("ninja")
        build_ninja = BUILD_DIR / "build.ninja"
        if not ninja_exe or not build_ninja.exists():
            self.skipTest("ninja or build/build.ninja not available")

        cuda_header = CORE_SRC / "feature" / "cuda" / "integer_adm_cuda.h"
        hip_header = CORE_SRC / "feature" / "hip" / "integer_adm_hip.h"
        self.assertTrue(cuda_header.exists())
        self.assertTrue(hip_header.exists())

        orig_cuda_stat = cuda_header.stat()
        orig_hip_stat = hip_header.stat()

        try:
            # Advance CUDA header mtime by 10s
            future_mtime = time.time() + 10.0
            os.utime(cuda_header, (future_mtime, future_mtime))

            # Ninja dry-run must show cu_ptx_target_adm_cm needs rebuild
            res = subprocess.run(
                [ninja_exe, "-C", str(BUILD_DIR), "-n", "src/adm_cm.fatbin"],
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertNotIn("ninja: no work to do.", res.stdout)
            self.assertIn("cu_ptx_target_adm_cm", res.stdout)

            # Advance HIP header mtime by 10s
            os.utime(hip_header, (future_mtime, future_mtime))

            # Ninja dry-run must show hip_hsaco_adm_cm needs rebuild
            res = subprocess.run(
                [ninja_exe, "-C", str(BUILD_DIR), "-n", "src/adm_cm.hsaco"],
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertNotIn("ninja: no work to do.", res.stdout)
            self.assertIn("hip_hsaco_adm_cm", res.stdout)
        finally:
            # Restore original timestamps
            os.utime(cuda_header, (orig_cuda_stat.st_atime, orig_cuda_stat.st_mtime))
            os.utime(hip_header, (orig_hip_stat.st_atime, orig_hip_stat.st_mtime))


if __name__ == "__main__":
    unittest.main()
