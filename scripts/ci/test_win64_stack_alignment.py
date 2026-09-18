#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ``check-win64-stack-alignment.py`` (ADR-1254).

The fixtures are real disassembly, trimmed: the ``BROKEN`` sample is the
prologue and spill sequence gcc 14.2.1 emitted for ``ssim_accumulate_avx512``
on ``x86_64-w64-mingw32`` before the fix, and ``REALIGNED`` is what the same
source produced for SysV, where gcc masks ``%rsp`` and the aligned spills are
therefore legitimate.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_win64_stack_alignment",
    Path(__file__).with_name("check-win64-stack-alignment.py"),
)
assert _SPEC and _SPEC.loader
gate = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(gate)


# The BROKEN fixture carries four unsafe accesses: three stores and one reload.
EXPECTED_BROKEN_FINDINGS = 4

BROKEN = """
0000000000000520 <ssim_accumulate_avx512>:
 520:\tpush   %r15
 52c:\tsub    $0x2a8,%rsp
 533:\tvmovaps %xmm6,0x200(%rsp)
 721:\tvmovaps %zmm28,0x1a0(%rsp)
 72c:\tvmovaps %zmm29,0x160(%rsp)
 742:\tvmovapd %zmm31,0xe0(%rsp)
 7a0:\tvmovaps 0x1a0(%rsp),%zmm28
 7b0:\tret
"""

REALIGNED = """
0000000000004414 <ssim_accumulate_avx512>:
    4414:\tlea    0x8(%rsp),%r10
    4419:\tand    $0xffffffffffffffc0,%rsp
    441d:\tpush   -0x8(%r10)
    4500:\tvmovaps %zmm28,0x1a0(%rsp)
    4510:\tret
"""

POINTER_ROUNDED = """
0000000000000000 <a_aligned>:
   0:\tsub    $0x78,%rsp
   a:\tlea    0x3f(%rsp),%rax
   f:\tand    $0xffffffffffffffc0,%rax
  13:\tvmovapd %zmm0,(%rax)
  19:\tret
"""

FIXED = """
0000000000000520 <ssim_accumulate_avx512>:
 520:\tpush   %r15
 52c:\tsub    $0x2a8,%rsp
 533:\tvmovaps %xmm6,0x200(%rsp)
 721:\tvbroadcastss %xmm0,%zmm28
 72c:\tvmulps %zmm1,%zmm2,%zmm3
 7b0:\tret
"""

XMM_ONLY = """
0000000000000100 <saves_xmm_only>:
 100:\tsub    $0x40,%rsp
 104:\tvmovaps %xmm6,0x20(%rsp)
 10a:\tvmovaps %xmm7,0x30(%rsp)
 110:\tret
"""


def test_flags_unrealigned_zmm_spill():
    findings = gate.scan_disassembly(BROKEN)
    assert len(findings) == EXPECTED_BROKEN_FINDINGS, findings
    assert all(fn == "ssim_accumulate_avx512" for fn, _ in findings)
    # Both directions of the move are reported, store and reload.
    assert any("%zmm28,0x1a0(%rsp)" in insn for _, insn in findings)
    assert any("0x1a0(%rsp),%zmm28" in insn for _, insn in findings)


def test_accepts_frame_realignment():
    assert gate.scan_disassembly(REALIGNED) == []


def test_accepts_rounded_scratch_pointer():
    """gcc's other correct idiom: round a pointer inside an oversized frame."""
    assert gate.scan_disassembly(POINTER_ROUNDED) == []


def test_accepts_fixed_function():
    assert gate.scan_disassembly(FIXED) == []


def test_ignores_128_bit_saves():
    """16-byte alignment is exactly what the MS x64 ABI guarantees."""
    assert gate.scan_disassembly(XMM_ONLY) == []


def test_realignment_does_not_leak_into_the_next_function():
    """A realigned frame must not excuse the function that follows it."""
    findings = gate.scan_disassembly(REALIGNED + BROKEN)
    assert len(findings) == EXPECTED_BROKEN_FINDINGS, findings
