#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device code compression (ADR-1590): the policy and the build-time check.

Two halves, both device-free:

* core/src/check_device_compression.py fails the build when a CUDA fatbin, a
  HIP code object bundle or a SYCL device image section is stored raw. The
  fixtures are synthetic: a fatbin with a cubin ELF header or PTX text in the
  clear, a plain offload bundle, ELF files whose SYCL sections hold SPIR-V or
  zstd frames (hand-built raw-block frames, so no zstd library is needed).
* core/src/meson.build gives every device compile its backend's compression
  list, defined once between BEGIN/END markers, at the strongest setting; the
  option that turns it off defaults to on.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

CORE = Path(__file__).resolve().parents[1]
MESON_SRC = CORE / "src" / "meson.build"
MESON_TEST = CORE / "test" / "meson.build"
OPTIONS = CORE / "meson_options.txt"


def load(name: str, path: Path) -> ModuleType:
    """Import a script of the tree by path."""
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


checker = load("check_device_compression", CORE / "src" / "check_device_compression.py")
# The synthetic ELF64 writer of the AOT image check's tests (ADR-1360).
elf64 = load("test_sycl_aot_image_check", CORE / "test" / "test_sycl_aot_image_check.py").elf64

FATBIN_HEADER = struct.pack("<IHHQ", 0xBA55ED50, 1, 16, 0)
OPAQUE = bytes(range(1, 200)) * 3
# A one-byte frame content size field holds at most 255.
MAX_FRAME_CONTENT = 255


def elf_header(machine: int) -> bytes:
    """The first 20 bytes of an ELF64 little-endian header for ``machine``."""
    return b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9 + struct.pack("<HH", 2, machine)


def zstd_frame(payload: bytes) -> bytes:
    """A valid zstd frame of one raw block: single segment, 1-byte content size."""
    assert len(payload) <= MAX_FRAME_CONTENT
    block = struct.pack("<I", (len(payload) << 3) | 1)[:3]
    return b"\x28\xb5\x2f\xfd" + bytes([0x20, len(payload)]) + block + payload


def sycl_elf(**sections: bytes) -> bytes:
    """An ELF64 file whose sections are named __CLANG_OFFLOAD_BUNDLE__sycl-<key>."""
    named = {
        f"__CLANG_OFFLOAD_BUNDLE__sycl-{key.replace('_', '-')}": body
        for key, body in sections.items()
    }
    return elf64({".text": b"\0" * 8, **named})


class FatbinCheckTest(unittest.TestCase):
    def test_compressed_fatbin_passes(self) -> None:
        checker.check_fatbin("k.fatbin", FATBIN_HEADER + OPAQUE)

    def test_raw_cubin_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "1 cubin and 0 PTX"):
            checker.check_fatbin("k.fatbin", FATBIN_HEADER + OPAQUE + elf_header(190) + OPAQUE)

    def test_raw_ptx_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "0 cubin and 1 PTX"):
            checker.check_fatbin("k.fatbin", FATBIN_HEADER + b".version 9.0\n.target sm_80\n")

    def test_wrong_magic_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "not a CUDA fatbin"):
            checker.check_fatbin("k.fatbin", OPAQUE)

    def test_boundaries(self) -> None:
        # An ELF magic too close to the end to hold e_machine, and a header for
        # another machine, are not cubins; an empty file is not a fatbin.
        checker.check_fatbin("k.fatbin", FATBIN_HEADER + elf_header(62) + b"\x7fELF\x02\x01")
        with self.assertRaises(checker.CompressionError):
            checker.check_fatbin("k.fatbin", b"")


class HsacoCheckTest(unittest.TestCase):
    def test_compressed_bundle_passes(self) -> None:
        checker.check_hsaco("k.hsaco", b"CCOB" + OPAQUE)

    def test_plain_bundle_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "not a compressed offload bundle"):
            checker.check_hsaco("k.hsaco", b"__CLANG_OFFLOAD_BUNDLE__" + elf_header(224))

    def test_raw_code_object_after_magic_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "1 AMDGPU code objects"):
            checker.check_hsaco("k.hsaco", b"CCOB" + OPAQUE + elf_header(224))


class SyclElfCheckTest(unittest.TestCase):
    def test_zstd_frames_with_padding_pass(self) -> None:
        gen = zstd_frame(b"zebin") + b"\0" * 7 + zstd_frame(b"zebin2")
        checker.check_sycl_elf("libvmaf.so", sycl_elf(spir64_gen=gen, spir64=zstd_frame(b"SPV")))

    def test_raw_spirv_fails(self) -> None:
        raw = b"\x03\x02\x23\x07" + OPAQUE
        with self.assertRaisesRegex(checker.CompressionError, "sycl-spir64: the device image"):
            checker.check_sycl_elf("libvmaf.so", sycl_elf(spir64_gen=zstd_frame(b"z"), spir64=raw))

    def test_raw_bytes_between_frames_fail(self) -> None:
        blob = zstd_frame(b"a") + b"\0\0RAW" + zstd_frame(b"b")
        with self.assertRaisesRegex(checker.CompressionError, "raw bytes at offset"):
            checker.check_sycl_elf("libvmaf.so", sycl_elf(spir64=blob))

    def test_truncated_frame_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "truncated"):
            checker.check_sycl_elf("libvmaf.so", sycl_elf(spir64=zstd_frame(b"abcdef")[:-3]))

    def test_binary_without_sycl_sections_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "no __CLANG_OFFLOAD_BUNDLE__sycl"):
            checker.check_sycl_elf("libvmaf.so", elf64({".text": b"\0" * 8}))


class MainTest(unittest.TestCase):
    def run_main(self, files: dict[str, bytes], flags: list[str]) -> tuple[int, str, Path]:
        """Write ``files``, run main() with ``flags`` naming them; return rc, stderr, stamp."""
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for name, data in files.items():
            (root / name).write_bytes(data)
        argv = ["--stamp", str(root / "stamp")]
        argv += [str(root / word) if word in files else word for word in flags]
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            rc = checker.main(argv)
        return rc, stderr.getvalue(), root / "stamp"

    def test_every_file_compressed_writes_the_stamp(self) -> None:
        files = {"a.fatbin": FATBIN_HEADER + OPAQUE, "b.fatbin": FATBIN_HEADER, "c.hsaco": b"CCOB"}
        rc, _err, stamp = self.run_main(
            files, ["--fatbin", "a.fatbin", "b.fatbin", "--hsaco", "c.hsaco"]
        )
        self.assertEqual(rc, 0)
        self.assertEqual(stamp.read_text(encoding="utf-8"), "3 device code files compressed\n")

    def test_one_raw_file_fails_with_the_remedy(self) -> None:
        files = {"a.fatbin": FATBIN_HEADER, "b.hsaco": b"__CLANG_OFFLOAD_BUNDLE__"}
        rc, err, stamp = self.run_main(files, ["--fatbin", "a.fatbin", "--hsaco", "b.hsaco"])
        self.assertEqual(rc, 1)
        self.assertIn("b.hsaco: not a compressed offload bundle", err)
        self.assertIn("ADR-1590", err)
        self.assertFalse(stamp.exists())

    def test_nothing_to_check_is_an_error(self) -> None:
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            checker.main(["--stamp", "unused"])


def policy_block(source: str, backend: str) -> str:
    """The text between the BEGIN/END markers of one backend's compression policy."""
    pattern = (
        rf"# BEGIN VMAF {backend} device code compression policy\n(.*?)"
        rf"# END VMAF {backend} device code compression policy"
    )
    blocks = re.findall(pattern, source, flags=re.DOTALL)
    assert len(blocks) == 1, f"{backend}: {len(blocks)} policy blocks"
    return blocks[0]


class PolicyContractTest(unittest.TestCase):
    source = MESON_SRC.read_text(encoding="utf-8")

    def test_each_backend_uses_its_strongest_setting(self) -> None:
        cuda = policy_block(self.source, "CUDA")
        self.assertIn(
            "cuda_compress_args = ['-Xfatbin=-compress-all', '--compress-mode=size']", cuda
        )
        self.assertIn("cuda_compress_args = ['--no-compress']", cuda)
        self.assertNotIn("'--concat'", cuda)
        for backend in ("HIP", "SYCL"):
            block = policy_block(self.source, backend)
            name = backend.lower() + "_compress_args"
            self.assertIn(
                f"{name} = ['--offload-compress', '--offload-compression-level=22']", block
            )
            self.assertIn("error(", block, f"{backend}: an unsupported compiler must fail")

    def test_flags_are_spelled_only_inside_the_policy_blocks(self) -> None:
        outside = self.source
        for backend in ("CUDA", "HIP", "SYCL"):
            outside = outside.replace(policy_block(self.source, backend), "")
        code = "\n".join(line.split("#", 1)[0] for line in outside.splitlines())
        for flag in ("--offload-compress", "--compress-mode", "-compress-all", "--no-compress"):
            self.assertNotIn(flag, code)

    def test_every_device_compile_takes_the_list(self) -> None:
        nvcc = re.search(r"custom_target\('cu_ptx_target_'.*?\n        \)", self.source, re.DOTALL)
        assert nvcc is not None
        self.assertIn("cuda_compress_args", nvcc.group(0))
        for path in (MESON_SRC, MESON_TEST):
            text = path.read_text(encoding="utf-8")
            genco = re.findall(r"\[hipcc_exe, '--genco'\].*?'@OUTPUT@'\]", text, re.DOTALL)
            self.assertTrue(genco, path)
            for command in genco:
                self.assertIn("hip_compress_args", command, path)
        aot_uses = re.findall(r"= (.*?)sycl_icpx_aot_base_args \+ \[", self.source)
        self.assertEqual(aot_uses, ["sycl_compress_args + "] * 2)
        self.assertIn(
            "if not sycl_msvc_device_link\n        sycl_link_args += sycl_compress_args",
            self.source,
        )
        self.assertRegex(
            self.source, r"'-fsycl-max-parallel-link-jobs=8'\] \\\n\s+\+ sycl_compress_args"
        )

    def test_the_build_checks_what_it_produced(self) -> None:
        for target, flag in (("cuda", "--fatbin"), ("hip", "--hsaco"), ("sycl", "--sycl-elf")):
            check = re.search(
                rf"custom_target\('{target}_device_compression_check',.*?\)\n",
                self.source,
                re.DOTALL,
            )
            self.assertIsNotNone(check, target)
            self.assertIn(f"'{flag}', '@INPUT@'", check.group(0))

    def test_option_defaults_to_on(self) -> None:
        options = OPTIONS.read_text(encoding="utf-8")
        match = re.search(
            r"option\('compress_device_code',\s*type: 'boolean',\s*value: (\w+)", options
        )
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), "true")


if __name__ == "__main__":
    unittest.main()
