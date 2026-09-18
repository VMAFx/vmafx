# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for scripts/ci/gen-gpu-compile-commands.py."""

from __future__ import annotations

import importlib.util
import json
import shlex
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "gen-gpu-compile-commands.py"


def _load():
    spec = importlib.util.spec_from_file_location("gen_gpu_compile_commands", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


gen = _load()

NINJA = """\
build src/adm_cm.fatbin: CUSTOM_COMMAND ../core/src/feature/cuda/adm_cm.cu | /opt/cuda/bin/nvcc
 COMMAND = /opt/cuda/bin/nvcc --fatbin -gencode=arch=compute_89,code=sm_89 \
../core/src/feature/cuda/adm_cm.cu -o src/adm_cm.fatbin -I /r/core/src -DDEVICE_CODE \
--threads 4 --std c++20
 description = Generating$ adm_cm.fatbin

build src/adm_cm.hsaco: CUSTOM_COMMAND_DEP ../core/src/feature/hip/adm_cm.hip | /opt/rocm/bin/hipcc
 COMMAND = /opt/rocm/bin/hipcc --genco --offload-arch=gfx1036 -I /opt/rocm/include \
-I /r/core/src -Xclang -dependency-file -Xclang src/adm_cm.hsaco.d -Xclang -MT -Xclang \
src/adm_cm.hsaco ../core/src/feature/hip/adm_cm.hip -o src/adm_cm.hsaco
 depfile = src/adm_cm.hsaco.d

build src/picture.o: CUSTOM_COMMAND ../core/src/sycl/picture.cpp | /opt/intel/icpx
 COMMAND = icpx -fsycl -c ../core/src/sycl/picture.cpp -o src/picture.o
"""


class KernelEntries(unittest.TestCase):
    def _run(self, existing=lambda _root: []) -> list[dict[str, str]]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            build = root / "build"
            build.mkdir()
            (build / "build.ninja").write_text(NINJA, encoding="utf-8")
            compdb = json.dumps(existing(root))
            (build / "compile_commands.json").write_text(compdb, encoding="utf-8")
            self.assertEqual(gen.main(["gen", str(build)]), 0)
            entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
            for entry in entries:
                entry["file"] = Path(entry["file"]).name
            return entries

    def test_cuda_and_hip_rules_become_clang_entries(self) -> None:
        entries = {e["file"]: shlex.split(e["command"]) for e in self._run()}
        self.assertEqual(sorted(entries), ["adm_cm.cu", "adm_cm.hip"])  # the icpx rule is not ours
        cuda = entries["adm_cm.cu"]
        self.assertEqual(cuda[0], "clang++")
        self.assertIn("--cuda-path=/opt/cuda", cuda)
        for flag in ("-I/r/core/src", "-DDEVICE_CODE", "-std=c++20"):
            self.assertIn(flag, cuda)
        for dropped in ("--fatbin", "--threads", "-o", "-gencode=arch=compute_89,code=sm_89"):
            self.assertNotIn(dropped, cuda)
        hip = entries["adm_cm.hip"]
        self.assertIn("-I/opt/rocm/include", hip)
        self.assertNotIn("--genco", hip)
        # A depfile target (CUSTOM_COMMAND_DEP) is found, and its dependency
        # flags stay out of the analysis command.
        self.assertFalse(any("dependency-file" in arg or arg == "-MT" for arg in hip))
        self.assertFalse(any(arg.startswith("--cuda-path") for arg in hip))

    def test_existing_entries_are_kept_and_kernels_replaced(self) -> None:
        def existing(root: Path) -> list[dict[str, str]]:
            kernel = str(root / "core/src/feature/cuda/adm_cm.cu")
            return [
                {"directory": "/b", "file": kernel, "command": "stale"},
                {"directory": "/b", "file": "/x/host.c", "command": "cc -c host.c"},
            ]

        entries = self._run(existing)
        self.assertIn("host.c", [e["file"] for e in entries])
        self.assertNotIn("stale", [e["command"] for e in entries])


if __name__ == "__main__":
    unittest.main()
