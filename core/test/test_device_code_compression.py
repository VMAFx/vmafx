#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device code compression (ADR-1590): the policy and the build-time check.

Two halves, both device-free:

* core/src/check_device_compression.py fails the build when a CUDA fatbin, a
  HIP code object bundle or a SYCL device image section is stored raw. The
  fixtures are synthetic: fatbins with raw entries or valid zstd streams
  containing cubin ELF headers or PTX markers, a plain offload bundle, and ELF
  files whose SYCL sections hold SPIR-V or zstd frames. Hand-built raw-block
  frames need no extra library; Python 3.14's standard decoder verifies the
  CUDA fixtures independently of the build check's flag/size contract.
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
from unittest.mock import patch

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

OPAQUE = bytes(range(1, 200)) * 3
# A one-byte frame content size field holds at most 255.
MAX_FRAME_CONTENT = 255


def elf_header(machine: int) -> bytes:
    """The first 20 bytes of an ELF64 little-endian header for ``machine``."""
    return b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9 + struct.pack("<HH", 2, machine)


def fatbin(
    *entries: tuple[int, int, int, bytes],
    compressed_size: int | None = None,
    decompressed_size: int | None = None,
) -> bytes:
    """Build a container with padded entry payloads."""
    body = bytearray()
    for kind, sm, flags, payload in entries:
        header = bytearray(64)
        padded = payload + b"\0" * (-len(payload) % 8)
        struct.pack_into("<H", header, 0, kind)
        struct.pack_into("<I", header, 4, len(header))
        struct.pack_into("<Q", header, 8, len(padded))
        default_size = len(payload) if flags & 0x8000 else 0
        struct.pack_into(
            "<I", header, 16, default_size if compressed_size is None else compressed_size
        )
        struct.pack_into("<I", header, 28, sm)
        struct.pack_into("<Q", header, 40, flags)
        struct.pack_into(
            "<Q", header, 56, default_size if decompressed_size is None else decompressed_size
        )
        body.extend(header + padded)
    return struct.pack("<IHHQ", 0xBA55ED50, 1, 16, len(body)) + body


def zstd_frame(payload: bytes) -> bytes:
    """A valid zstd frame of one raw block: single segment, 1-byte content size."""
    assert len(payload) <= MAX_FRAME_CONTENT
    block = struct.pack("<I", (len(payload) << 3) | 1)[:3]
    return b"\x28\xb5\x2f\xfd" + bytes([0x20, len(payload)]) + block + payload


def compressed_fatbin(payload: bytes, *, kind: int = 2, sm: int = 90) -> bytes:
    """A size-mode CUDA entry with a valid zstd stream and exact decoded size.

    A raw block deliberately retains markers inside the stream. nvcc's size
    mode uses zstd with flag 0x8000; the checker does not decode the stream.
    """
    return fatbin((kind, sm, 0x8011, zstd_frame(payload)), decompressed_size=len(payload))


class FatbinFixtureTest(unittest.TestCase):
    def setUp(self) -> None:
        try:
            from compression import zstd  # noqa: PLC0415 - Python >= 3.14 only
        except ImportError:
            self.skipTest("fixture decoding needs Python >= 3.14's compression.zstd")
        self.zstd = zstd

    def test_marker_streams_decode_to_original(self) -> None:
        for kind, sm, payload in ((1, 80, b".target sm_80"), (2, 90, elf_header(190))):
            with self.subTest(kind=kind):
                data = compressed_fatbin(payload, kind=kind, sm=sm)
                [entry] = checker.fatbin_entries("fixture.fatbin", data)
                _index, _kind, _sm, _flags, start, end, compressed, decompressed = entry
                stream = data[start : start + compressed]
                self.assertEqual(compressed, len(zstd_frame(payload)))
                self.assertEqual(decompressed, len(payload))
                self.assertEqual(data[start + compressed : end], b"\0" * (-compressed % 8))
                self.assertIn(payload, stream)
                self.assertEqual(self.zstd.decompress(stream), payload)

    def test_plaintext_with_consistent_metadata_is_not_a_valid_fixture(self) -> None:
        for kind, sm, payload in ((1, 80, b".target sm_80"), (2, 90, elf_header(190))):
            with self.subTest(kind=kind):
                data = fatbin((kind, sm, 0x8011, payload))
                [entry] = checker.fatbin_entries("fixture.fatbin", data)
                self.assertEqual(entry[-2:], (len(payload), len(payload)))
                # Fixture trust check only: the production checker does not decode.
                with self.assertRaises(self.zstd.ZstdError):
                    self.zstd.decompress(data[entry[4] : entry[4] + entry[-2]])

    def test_invalid_stream_with_consistent_metadata_is_not_a_valid_fixture(self) -> None:
        payload = b".target sm_80"
        frame = zstd_frame(payload)
        reserved_block = frame[:6] + bytes([frame[6] | 6]) + frame[7:]
        for stream in (frame[:-1], reserved_block):
            with self.subTest(stream=stream):
                data = fatbin((1, 80, 0x8011, stream), decompressed_size=len(payload))
                [entry] = checker.fatbin_entries("fixture.fatbin", data)
                self.assertEqual(entry[-2:], (len(stream), len(payload)))
                with self.assertRaises(self.zstd.ZstdError):
                    self.zstd.decompress(data[entry[4] : entry[4] + entry[-2]])


def sycl_elf(**sections: bytes) -> bytes:
    """An ELF64 file whose sections are named __CLANG_OFFLOAD_BUNDLE__sycl-<key>."""
    named = {
        f"__CLANG_OFFLOAD_BUNDLE__sycl-{key.replace('_', '-')}": body
        for key, body in sections.items()
    }
    return elf64({".text": b"\0" * 8, **named})


class FatbinCheckTest(unittest.TestCase):
    def test_compressed_fatbin_passes(self) -> None:
        checker.check_fatbin("k.fatbin", compressed_fatbin(b"compressed cubin"))

    def test_compressed_payload_with_ptx_marker_passes(self) -> None:
        checker.check_fatbin("k.fatbin", compressed_fatbin(b".target sm_80", kind=1, sm=80))

    def test_compressed_payload_with_cubin_header_passes(self) -> None:
        checker.check_fatbin("k.fatbin", compressed_fatbin(elf_header(190)))

    def test_flagged_zero_compressed_size_fails(self) -> None:
        with self.assertRaisesRegex(
            checker.CompressionError,
            r"entry 0 cubin sm_90 flags 0x8011.*compressed size 0.*decompressed size 20",
        ):
            checker.check_fatbin(
                "k.fatbin", fatbin((2, 90, 0x8011, elf_header(190)), compressed_size=0)
            )

    def test_flagged_oversized_compressed_size_fails(self) -> None:
        with self.assertRaisesRegex(
            checker.CompressionError,
            r"entry 0 PTX sm_80 flags 0x8011.*compressed size 9.*decompressed size 8",
        ):
            checker.check_fatbin(
                "k.fatbin", fatbin((1, 80, 0x8011, b"12345678"), compressed_size=9)
            )

    def test_flagged_zero_decompressed_size_fails(self) -> None:
        with self.assertRaisesRegex(
            checker.CompressionError,
            r"entry 0 PTX sm_80 flags 0x8011.*compressed size 8.*decompressed size 0",
        ):
            checker.check_fatbin(
                "k.fatbin", fatbin((1, 80, 0x8011, b"12345678"), decompressed_size=0)
            )

    def test_ptx_marker_in_compressed_padding_fails(self) -> None:
        payload = b"ptx"
        stream = zstd_frame(payload)
        with self.assertRaisesRegex(checker.CompressionError, "0 cubin and 1 PTX"):
            checker.check_fatbin(
                "k.fatbin",
                fatbin(
                    (1, 80, 0x8011, stream + b".target sm_"),
                    compressed_size=len(stream),
                    decompressed_size=len(payload),
                ),
            )

    def test_compressed_size_equals_payload_size_passes(self) -> None:
        data = compressed_fatbin(b"1234567", kind=1, sm=80)
        [entry] = checker.fatbin_entries("k.fatbin", data)
        self.assertEqual(entry[-2], entry[5] - entry[4])
        checker.check_fatbin("k.fatbin", data)

    def test_compressed_size_one_below_payload_size_passes(self) -> None:
        data = compressed_fatbin(b"123456", kind=1, sm=80)
        [entry] = checker.fatbin_entries("k.fatbin", data)
        self.assertEqual(entry[-2] + 1, entry[5] - entry[4])
        checker.check_fatbin("k.fatbin", data)

    def test_raw_ptx_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "entry 0 PTX sm_80 flags 0x11"):
            checker.check_fatbin(
                "k.fatbin", fatbin((1, 80, 0x11, b".version 9.0\n.target sm_80\n"))
            )

    def test_raw_cubin_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "entry 0 cubin sm_90 flags 0x11"):
            checker.check_fatbin("k.fatbin", fatbin((2, 90, 0x11, elf_header(190))))

    def test_raw_without_marker_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "entry 0 PTX sm_80 flags 0x11"):
            checker.check_fatbin("k.fatbin", fatbin((1, 80, 0x11, b"opaque")))

    def test_trailing_ptx_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "offset 16"):
            checker.check_fatbin("k.fatbin", fatbin() + b".target sm_80")

    def test_cubin_in_entry_header_fails(self) -> None:
        data = bytearray(compressed_fatbin(b"payload"))
        data[16 + 64 : 16 + 64] = elf_header(190) + b"\0" * 4
        struct.pack_into("<I", data, 16 + 4, 88)
        struct.pack_into("<Q", data, 8, len(data) - 16)
        with self.assertRaisesRegex(checker.CompressionError, "1 cubin and 0 PTX"):
            checker.check_fatbin("k.fatbin", bytes(data))

    def test_zero_entries_passes(self) -> None:
        checker.check_fatbin("k.fatbin", fatbin())

    def test_payload_past_body_fails(self) -> None:
        data = bytearray(fatbin((2, 90, 0x8011, b"x")))
        struct.pack_into("<Q", data, 16 + 8, 16)
        with self.assertRaisesRegex(checker.CompressionError, "offset 16"):
            checker.check_fatbin("k.fatbin", bytes(data))

    def test_short_entry_header_fails(self) -> None:
        data = bytearray(fatbin((2, 90, 0x8011, b"x")))
        struct.pack_into("<I", data, 16 + 4, 63)
        with self.assertRaisesRegex(checker.CompressionError, "offset 16"):
            checker.check_fatbin("k.fatbin", bytes(data))

    def test_two_containers_pass(self) -> None:
        container = compressed_fatbin(b"payload")
        checker.check_fatbin("k.fatbin", container + container)

    def test_entry_limit_fails(self) -> None:
        entry = (2, 90, 0x8011, b"")
        with patch.object(checker, "MAX_FATBIN_ENTRIES", 2):
            with self.assertRaisesRegex(checker.CompressionError, "entry limit.*offset"):
                checker.check_fatbin("k.fatbin", fatbin(entry, entry, entry))

    def test_wrong_magic_fails(self) -> None:
        with self.assertRaisesRegex(checker.CompressionError, "not a CUDA fatbin"):
            checker.check_fatbin("k.fatbin", OPAQUE)

    def test_boundaries(self) -> None:
        data = bytearray(compressed_fatbin(b"payload"))
        data[80:80] = elf_header(62) + b"\x7fELF\x02\x01" + b"\0" * 6
        struct.pack_into("<I", data, 20, 96)
        struct.pack_into("<Q", data, 8, len(data) - 16)
        checker.check_fatbin("k.fatbin", bytes(data))
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
        files = {
            "a.fatbin": compressed_fatbin(b"compressed cubin"),
            "b.fatbin": fatbin(),
            "c.hsaco": b"CCOB",
        }
        rc, _err, stamp = self.run_main(
            files, ["--fatbin", "a.fatbin", "b.fatbin", "--hsaco", "c.hsaco"]
        )
        self.assertEqual(rc, 0)
        self.assertEqual(stamp.read_text(encoding="utf-8"), "3 device code files compressed\n")

    def test_one_raw_file_fails_with_the_remedy(self) -> None:
        files = {"a.fatbin": fatbin(), "b.hsaco": b"__CLANG_OFFLOAD_BUNDLE__"}
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
