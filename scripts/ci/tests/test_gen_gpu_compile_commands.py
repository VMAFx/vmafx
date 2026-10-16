# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for scripts/ci/gen-gpu-compile-commands.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import shlex
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from types import ModuleType
from unittest import mock

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "gen-gpu-compile-commands.py"


def _load() -> ModuleType:
    """Import the hyphenated script under test by path."""
    spec = importlib.util.spec_from_file_location("gen_gpu_compile_commands", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot build an import spec for {SCRIPT}")
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
    def _run(
        self, existing: Callable[[Path], list[dict[str, str]]] = lambda _root: []
    ) -> list[dict[str, str]]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            build = root / "build"
            build.mkdir()
            (build / "build.ninja").write_text(NINJA, encoding="utf-8")
            compdb = json.dumps(existing(root))
            (build / "compile_commands.json").write_text(compdb, encoding="utf-8")
            self.assertEqual(gen.main(["gen", str(build)]), 0)
            entries: list[dict[str, str]] = json.loads(
                (build / "compile_commands.json").read_text(encoding="utf-8")
            )
            for entry in entries:
                entry["file"] = Path(entry["file"]).name
            return entries

    def test_cuda_and_hip_rules_become_clang_entries(self) -> None:
        entries = {e["file"]: shlex.split(e["command"]) for e in self._run()}
        self.assertEqual(
            sorted(entries), ["adm_cm.cu", "adm_cm.hip", "hip_dispatch_drop_probe.hip"]
        )  # the icpx rule is not ours
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
        probe = entries["hip_dispatch_drop_probe.hip"]
        self.assertEqual(probe[0], "clang++")
        self.assertIn("-I/opt/rocm/include", probe)
        self.assertFalse(any(arg.startswith("--cuda-path") for arg in probe))

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


# The layout a configured tree has: every kernel target lists the headers it
# includes in `depend_files`, so meson puts them between the `|` and the
# compiler. The first parser matched `<kernel> | <compiler>` only and found no
# rule at all in such a tree: the cuda and hip lanes measured no kernel file,
# and both committed baselines held none.
NINJA_WITH_HEADER_DEPS = """\
build src/adm_dwt2.fatbin: CUSTOM_COMMAND_DEP ../core/src/feature/cuda/adm_dwt2.cu | \
../core/include/libvmaf/picture.h ../core/src/cuda/cuda_helper.cuh /usr/local/cuda/bin/nvcc
 DEPFILE = src/adm_dwt2.fatbin.d
 DEPFILE_UNQUOTED = src/adm_dwt2.fatbin.d
 COMMAND = /usr/local/cuda/bin/nvcc --fatbin ../core/src/feature/cuda/adm_dwt2.cu \
-o src/adm_dwt2.fatbin -I /r/core/src -DDEVICE_CODE --std c++20
 description = Generating$ adm_dwt2.fatbin

build src/psnr_score.hsaco: CUSTOM_COMMAND ../core/src/feature/hip/psnr_score.hip | \
../core/src/hip/kernel_template.h /opt/rocm/bin/hipcc || src/order_only.stamp
 COMMAND = /opt/rocm/bin/hipcc --genco -I /opt/rocm/include -I /r/core/src \
../core/src/feature/hip/psnr_score.hip -o src/psnr_score.hsaco

build src/psnr_score_hsaco.c: CUSTOM_COMMAND src/psnr_score.hsaco | /usr/bin/xxd
 COMMAND = /usr/bin/xxd -i src/psnr_score.hsaco src/psnr_score_hsaco.c
"""


def _generate(ninja: str) -> tuple[int, list[dict[str, str]], str]:
    """Run the generator over *ninja*; return its code, database and stderr."""
    with tempfile.TemporaryDirectory() as tmp:
        build = Path(tmp).resolve() / "build"
        build.mkdir()
        (build / "build.ninja").write_text(ninja, encoding="utf-8")
        compdb = build / "compile_commands.json"
        compdb.write_text("[]", encoding="utf-8")
        printed = io.StringIO()
        with contextlib.redirect_stderr(printed):
            code = gen.main(["gen", str(build)])
        entries: list[dict[str, str]] = json.loads(compdb.read_text(encoding="utf-8"))
        return code, entries, printed.getvalue()


class RuleLayouts(unittest.TestCase):
    def test_header_dependencies_do_not_hide_the_kernel(self) -> None:
        code, entries, printed = _generate(NINJA_WITH_HEADER_DEPS)
        self.assertEqual(code, 0)
        commands = {Path(e["file"]).name: shlex.split(e["command"]) for e in entries}
        self.assertEqual(
            sorted(commands), ["adm_dwt2.cu", "hip_dispatch_drop_probe.hip", "psnr_score.hip"]
        )
        self.assertIn("3 CUDA/HIP kernel entries", printed)
        # The compiler is read from the command, not from the dependency list,
        # where a header now comes first.
        self.assertIn("--cuda-path=/usr/local/cuda", commands["adm_dwt2.cu"])
        self.assertIn("-std=c++20", commands["adm_dwt2.cu"])
        self.assertIn("-I/opt/rocm/include", commands["psnr_score.hip"])
        self.assertIn("-I/opt/rocm/include", commands["hip_dispatch_drop_probe.hip"])

    def test_missing_probe_source_fails_hip_lane(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp).resolve() / "build"
            build.mkdir()
            (build / "build.ninja").write_text(NINJA, encoding="utf-8")
            (build / "compile_commands.json").write_text("[]", encoding="utf-8")
            printed = io.StringIO()
            with (
                mock.patch.object(gen, "REPO_ROOT", Path(tmp).resolve()),
                contextlib.redirect_stderr(printed),
            ):
                code = gen.main(["gen", str(build)])
            self.assertEqual(code, 1)
            self.assertIn("missing HIP dispatch drop probe source", printed.getvalue())

    def test_a_build_without_kernels_adds_nothing(self) -> None:
        ninja = "build src/a.o: c_COMPILER ../core/src/a.c\n ARGS = -I.\n"
        code, entries, printed = _generate(ninja)
        self.assertEqual(code, 0)
        self.assertEqual(entries, [])
        self.assertIn("0 CUDA/HIP kernel entries", printed)

    def test_a_kernel_rule_without_a_command_fails(self) -> None:
        ninja = "build src/k.fatbin: CUSTOM_COMMAND ../core/src/k.cu | /opt/cuda/bin/nvcc\n"
        code, entries, printed = _generate(ninja)
        self.assertEqual(code, 1)
        self.assertEqual(entries, [])  # the database is left as it was
        self.assertIn("k.cu: build statement has no COMMAND", printed)

    def test_a_renamed_rule_fails_instead_of_dropping_the_kernels(self) -> None:
        ninja = NINJA_WITH_HEADER_DEPS.replace("CUSTOM_COMMAND_DEP", "NVCC_COMPILER")
        code, entries, printed = _generate(ninja)
        self.assertEqual(code, 1)
        self.assertEqual(entries, [])
        self.assertIn("2 build statements compile a .cu / .hip file, 1 were parsed", printed)


if __name__ == "__main__":
    unittest.main()
