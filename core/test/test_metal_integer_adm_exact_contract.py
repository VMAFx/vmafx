#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin integer_adm_metal to the CPU's integer arithmetic and layout (ADR-1498, ADR-1806).

The decouple (ADR-1498, ADR-1413): the CPU's ``adm_decouple_band()`` /
``adm_decouple_band_s123()`` (``integer_adm_kernels.h``) take the reciprocal
2^30 / o from ``div_lookup`` and store the double product of the restored
sample and ``adm_enhn_gain_limit`` truncated toward zero; the kernel calls
``vmaf_mtl_iadm_decouple_s0()`` / ``_s123()`` of ``metal_integer_adm_math.h``
(T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01).

The six defects of T-METAL-INTEGER-ADM-TWIN-DEFECTS-2026-10-05, each a class
this test refuses:

1. the reduction slots: the kernels write and the host reads them through
   ``vmaf_mtl_iadm_accum_word()`` of ``metal_integer_adm_uniforms.h`` only
   (the kernels wrote them twice as far apart as the host read them);
2. the scale-1 parent: scale 1 runs ``integer_adm_dwt_vert_s1``, which reads
   the int16 band of scale 0 (the CPU's ``i16_to_i32()``), never the int32
   kernel;
3. the scales-1-3 masking terms take the CPU's rounding term INT32_MIN
   (``i4_adm_round_terms()``, Netflix#955) from ``I4AdmCmCtx``, never +2^31;
4. the scales-1-3 denominator rounds its squares with ``I4AdmDenCtx``'s
   2^shift_sq, never 2^(shift_sq - 1);
5. under ``adm_skip_scale0`` scale 0 has no numerator, no AIM and only the DWT;
6. every score is concluded by the CPU's ``adm_cm_result()`` /
   ``adm_csf_den_result()`` with the double noise weight, never a float copy.

Device-free: reads the sources only. ``test_metal_integer_adm_host_replay``
runs the kernels on the host, ``test_metal_integer_adm_math`` the header, and
``test_metal_integer_adm_parity`` compares the scores on an Apple device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

MATH = "metal/metal_integer_adm_math.h"
UNIFORMS = "metal/metal_integer_adm_uniforms.h"
KERNEL = "metal/integer_adm.metal"
HOST = "metal/integer_adm_metal_host.c"
HOST_H = "metal/integer_adm_metal_host.h"
MM = "metal/integer_adm_metal.mm"
SHARED = "adm_gain_limit.h"
REFERENCE = "integer_adm_kernels.h"
TABLE = "integer_adm.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

MATH_PIECES = {
    "vmaf_mtl_i32 vmaf_mtl_iadm_recip(vmaf_mtl_i32 o)": ("return 1073741824 / o;",),
    "vmaf_mtl_i32 vmaf_mtl_iadm_gain_limit(": (
        "const vmaf_mtl_i64 gained = adm_gain_limit_product(rst, g);",
        "return (vmaf_mtl_i32)((gained < (vmaf_mtl_i64)t) ? gained : (vmaf_mtl_i64)t);",
        "return (vmaf_mtl_i32)((gained > (vmaf_mtl_i64)t) ? gained : (vmaf_mtl_i64)t);",
    ),
    "vmaf_mtl_i32 vmaf_mtl_iadm_decouple_s0(": (
        "const vmaf_mtl_i32 rst = ((k * o) + 16384) >> 15;",
    ),
    "vmaf_mtl_i32 vmaf_mtl_iadm_decouple_s123(": (
        "const vmaf_mtl_i32 rst = (vmaf_mtl_i32)(((k * o) + 16384) >> 15);",
    ),
    "vmaf_mtl_i32 vmaf_mtl_iadm_i4_csf(": (
        "return (vmaf_mtl_i32)((((vmaf_mtl_i64)rfactor * v) + add) >> shift);",
    ),
    "vmaf_mtl_i32 vmaf_mtl_iadm_i4_masking_term(": (
        "return (vmaf_mtl_i32)(((coeff * magnitude) + add) >> shift);",
    ),
}
KERNEL_CALLS = {
    "vmaf_mtl_iadm_decouple_s0(o_val, t_val, af, iadm_gain(c))": 3,
    "vmaf_mtl_iadm_decouple_s123(o_val, t_val, af, iadm_gain(c))": 1,
    "vmaf_mtl_iadm_i4_csf(irf, src, c.i4_add_shift_dst, c.i4_shift_dst)": 1,
    "c.i4_add_shift_flt, c.i4_shift_flt)": 3,
    "accum_out[vmaf_mtl_iadm_accum_word(wg, slot, 0u)]": 1,
    "accum_out[vmaf_mtl_iadm_accum_word(wg, slot, 1u)]": 1,
}
S1_KERNEL = (
    "kernel void integer_adm_dwt_vert_s1(const device short *parent_ref_band [[buffer(6)]], "
    "const device short *parent_dis_band [[buffer(7)]],"
)
HOST_PIECES = {
    "gain": (
        '#include "../adm_gain_limit.h"',
        "const struct AdmGainLimit gain = adm_gain_limit_split(o->adm_enhn_gain_limit);",
        "c->gain_m_hi = gain.m_hi;",
        "c->gain_m_lo = gain.m_lo;",
        "c->gain_frac_bits = gain.frac_bits;",
    ),
    "slots": (
        "return (size_t)vmaf_mtl_iadm_accum_word(g->wg_count[scale], 0u, 0u) * sizeof(uint32_t);",
        "return ((uint64_t)accum[vmaf_mtl_iadm_accum_word(wg, slot, 1u)] << 32) | "
        "(uint64_t)accum[vmaf_mtl_iadm_accum_word(wg, slot, 0u)];",
    ),
    "parent": (
        "return scale == 1 ? IADM_METAL_DWT_VERT_S1 : IADM_METAL_DWT_VERT_S123;",
        "(scale == 0 ? sizeof(int16_t) : sizeof(int32_t));",
        '"integer_adm_dwt_vert_s1"',
    ),
    "rounding": (
        "i4_adm_cm_ctx_init(&cm, &no_planes, w, h, 0, 0, scale,",
        "c->i4_add_shift_flt = cm.add_bef_shift_flt;",
        "c->i4_shift_flt = cm.shift_flt;",
        "c->i4_add_shift_dst = cm.add_bef_shift_dst;",
    ),
    "denominator": (
        "i4_adm_csf_den_ctx_init(&den, scale, w, h,",
        "c->den_add_shift_sq = den.add_shift_sq;",
        "c->den_shift_sq = den.shift_sq;",
    ),
    "conclusion": (
        "return adm_cm_result(&c, &bd, accum, noise_weight, o->adm_p_norm);",
        "return i4_adm_cm_result(&c, &bd, sums, noise_weight, o->adm_p_norm);",
        "return adm_csf_den_result(&c, accum, o->adm_noise_weight);",
        "return i4_adm_csf_den_result(&c, accum, o->adm_noise_weight);",
        "out[0] = iadm_cm_result(o, scale, w, h, t.cm, o->adm_noise_weight);",
    ),
}
REFERENCE_LINES = (
    "const int32_t tmp_k = (o == 0) ? 32768 : (((int64_t)lut[o + 32768] * t) + 16384) >> 15;",
    "rst = (int16_t)(ADM_KERNEL_MIN((rst * gain), t));",
    "rst = (int16_t)(ADM_KERNEL_MAX((rst * gain), t));",
    "rst = (int32_t)(ADM_KERNEL_MIN((rst * gain), t));",
    "rst = (int32_t)(ADM_KERNEL_MAX((rst * gain), t));",
    "add_bef_shift_flt[idx] = (int32_t)(1u << (i4_shift_flt[idx] - 1));",
    "const uint32_t add_shift_sq[3] = {1u << shift_sq[0], 1u << shift_sq[1], 1u << shift_sq[2]};",
)
TABLE_LINE = "const int32_t recip = (int32_t)(div_Q_factor / i);"


GUARD_COUNT = 2  # #if !defined(__METAL_VERSION__) guards of the shared header


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    names = (MATH, UNIFORMS, KERNEL, HOST, HOST_H, MM, SHARED, REFERENCE, TABLE)
    return {name: (FEATURE / name).read_text(encoding="utf-8") for name in names}


def _function_body(code: str, signature: str) -> str:
    """Flattened text of the definition that starts with `signature` (brace-matched), or empty."""
    start = code.find(signature)
    if start < 0:
        return ""
    depth = 0
    for index in range(code.index("{", start), len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[start : index + 1]
    return ""


def _math_failures(math: str) -> list[str]:
    code = _flat(math)
    failures: list[str] = []
    for signature, pieces in MATH_PIECES.items():
        body = _function_body(code, signature)
        if not body or any(piece not in body for piece in pieces):
            failures.append(f"{MATH}: `{signature}` is not the CPU's integer arithmetic")
    recip = _function_body(code, "vmaf_mtl_i32 vmaf_mtl_iadm_recip(vmaf_mtl_i32 o)")
    if "float" in recip or "(float)rst" in code:
        failures.append(f"{MATH}: the reciprocal or the gain product is formed in fp32")
    if '#include "../adm_gain_limit.h"' not in code:
        failures.append(f"{MATH}: the decouple does not use the shared adm_gain_limit.h")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    failures: list[str] = []
    for header in ("metal_integer_adm_math.h", "metal_integer_adm_uniforms.h"):
        if f'#include "{header}"' not in code:
            failures.append(f"{KERNEL}: the kernel does not include {header}")
    if any(code.count(call) != count for call, count in KERNEL_CALLS.items()):
        failures.append(f"{KERNEL}: a decouple, CSF, masking or slot site does not call the header")
    if re.search(r"IADM_DIV_Q_FACTOR|\begl\b|float gain_limit|iadm_decouple_r_s", code):
        failures.append(f"{KERNEL}: the fp32 reciprocal or the binary32 gain limit is back")
    if re.search(r"accum_out\[(?!vmaf_mtl_iadm_accum_word\()", code) or "IADM_ACCUM_SLOTS" in code:
        failures.append(
            f"{KERNEL}: a reduction slot is addressed outside vmaf_mtl_iadm_accum_word()"
        )
    if S1_KERNEL not in code:
        failures.append(f"{KERNEL}: scale 1 has no kernel that reads the int16 band of scale 0")
    if re.search(r"add_bef_shift_flt|shift_flt - 1u|1u << 31|2147483648", code):
        failures.append(f"{KERNEL}: a scales-1-3 rounding term is the kernel's own, not INT32_MIN")
    if re.search(r"struct IadmCsf \{|struct IadmDims \{", code):
        failures.append(f"{KERNEL}: the kernel defines its own uniforms")
    return failures


def _host_failures(host: str, header: str) -> list[str]:
    code = _flat(host)
    failures = [
        f"{HOST}: the {group} terms are not the shared layout or the CPU's"
        for group, pieces in HOST_PIECES.items()
        if any(piece not in code for piece in pieces)
    ]
    if re.search(r"<<\s*\(\s*(?:den\.)?(?:den_)?shift_sq\s*-\s*1", code):
        failures.append(f"{HOST}: the denominator square is rounded with 2^(shift - 1)")
    if re.search(r"\(\s*float\s*\)\s*(?:o->|s->)?adm_noise_weight|float noise_weight", code):
        failures.append(f"{HOST}: the noise weight is narrowed to float")
    if '#include "metal_integer_adm_uniforms.h"' not in _flat(header):
        failures.append(f"{HOST_H}: the host does not take the uniforms of the kernels")
    failures += _skip_scale0_failures(code)
    return failures


def _skip_scale0_failures(code: str) -> list[str]:
    failures: list[str] = []
    scores = _function_body(code, "static void iadm_scale_scores(")
    skip = scores.find("if (scale == 0 && o->adm_skip_scale0) {")
    aim = scores.find("t.aim")
    if skip < 0 or aim < 0 or "return;" not in scores[skip:aim]:
        failures.append(
            f"{HOST}: adm_skip_scale0 does not leave scale 0 without a numerator or AIM"
        )
    stages = _function_body(code, "unsigned iadm_metal_stages(")
    skip = stages.find("if (s0 && o->adm_skip_scale0) {")
    decouple = stages.find("IADM_METAL_DECOUPLE_CSF_S0")
    if skip < 0 or decouple < 0 or "return n;" not in stages[skip:decouple]:
        failures.append(f"{HOST}: adm_skip_scale0 runs more than the DWT at scale 0")
    views = _function_body(code, "unsigned iadm_metal_view_stages(")
    if "iadm_metal_stages(o, g, scale, all)" not in views or (
        "view == 0u || !iadm_is_dwt(all[i].entry)" not in views
    ):
        failures.append(f"{HOST}: a later viewing distance does not run the stages after the DWT")
    return failures


def _mm_failures(mm: str) -> list[str]:
    code = _flat(mm)
    failures: list[str] = []
    if '#include "integer_adm_metal_host.h"' not in code:
        failures.append(f"{MM}: the dispatch does not use integer_adm_metal_host.c")
    if re.search(r"IadmCsfHost|IadmDimsHost|IADM_ACCUM_SLOTS|\bpowf\(|\bconclude_adm_", code):
        failures.append(f"{MM}: the dispatch keeps its own uniforms, slots or score formulas")
    for piece in (
        "iadm_metal_uniforms(&o, &s->geom, scale, &d, &c);",
        # Every viewing distance's stages, the DWT only for the first (ADR-2795).
        "iadm_metal_view_stages(&o, &s->geom, scale, v, stages);",
        "iadm_metal_scores(&o, &s->geom, accum, index, &r);",
        "bind_buffer(enc, s->ref_band[scale - 1], 6);",
    ):
        if piece not in code:
            failures.append(f"{MM}: `{piece}` is missing")
    return failures


def _shared_failures(shared: str) -> list[str]:
    code = COMMENT.sub(" ", shared)
    guards = [m.start() for m in re.finditer(r"#if !defined\(__METAL_VERSION__\)", code)]
    product = code.find("adm_gain_limit_product(int32_t rst")
    split = code.find("adm_gain_limit_split(double gain)")
    if len(guards) != GUARD_COUNT or not guards[0] < guards[1] < split:
        return [f"{SHARED}: the includes and the split are not kept out of Metal"]
    if product < 0 or code.rfind("#endif", 0, product) < split:
        return [f"{SHARED}: adm_gain_limit_product() is not visible to Metal"]
    return []


def _reference_failures(reference: str, table: str) -> list[str]:
    code = _flat(reference)
    failures = [
        f"{REFERENCE} no longer holds `{line}`; the twin mirrors it"
        for line in REFERENCE_LINES
        if line not in code
    ]
    if TABLE_LINE not in _flat(table):
        failures.append(f"{TABLE} no longer holds `{TABLE_LINE}`; the twin mirrors it")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _math_failures(sources[MATH])
        + _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST], sources[HOST_H])
        + _mm_failures(sources[MM])
        + _shared_failures(sources[SHARED])
        + _reference_failures(sources[REFERENCE], sources[TABLE])
    )


class IntegerAdmMetalExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_fp32_reciprocal_is_detected(self) -> None:
        # The pre-port scale-0 reciprocal.
        failures = self._edited(
            MATH,
            "    return 1073741824 / o;",
            "    return (vmaf_mtl_i32)(1073741824.0f / (float)o);",
        )
        self._assert_detected(failures, "formed in fp32")

    def test_binary32_gain_product_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const vmaf_mtl_i64 gained = adm_gain_limit_product(rst, g);",
            "    const vmaf_mtl_i64 gained = (vmaf_mtl_i64)((float)rst * egl);",
        )
        self._assert_detected(failures, "is not the CPU's integer arithmetic")
        self._assert_detected(failures, "formed in fp32")

    def test_float_restored_sample_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const vmaf_mtl_i32 rst = (vmaf_mtl_i32)(((k * o) + 16384) >> 15);",
            "    const vmaf_mtl_i32 rst = (vmaf_mtl_i32)(float)(((k * o) + 16384) >> 15);",
        )
        self._assert_detected(failures, "vmaf_mtl_iadm_decouple_s123(")

    def test_later_view_rerunning_the_dwt_is_detected(self) -> None:
        # ADR-2795: the second viewing distance reads the first one's bands.
        failures = self._edited(
            HOST, "view == 0u || !iadm_is_dwt(all[i].entry)", "view == 0u || view != 0u"
        )
        self._assert_detected(failures, "stages after the DWT")

    def test_dispatch_without_the_view_stages_is_detected(self) -> None:
        failures = self._edited(
            MM,
            "iadm_metal_view_stages(&o, &s->geom, scale, v, stages);",
            "iadm_metal_stages(&o, &s->geom, scale, stages);",
        )
        self._assert_detected(failures, "iadm_metal_view_stages")

    def test_kernel_side_decouple_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "vmaf_mtl_iadm_decouple_s123(o_val, t_val, af, iadm_gain(c))",
            "iadm_decouple_r_s123(o_val, t_val, af, c.gain_limit)",
        )
        self._assert_detected(failures, "does not call the header")
        self._assert_detected(failures, "binary32 gain limit is back")

    def test_float_limit_on_the_host_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "const struct AdmGainLimit gain = adm_gain_limit_split(o->adm_enhn_gain_limit);",
            "const float gain_limit = (float)o->adm_enhn_gain_limit;",
        )
        self._assert_detected(failures, "the gain terms")

    def test_doubled_slot_stride_is_detected(self) -> None:
        # Defect 1: the kernels' former slot addressing.
        failures = self._edited(
            KERNEL,
            "accum_out[vmaf_mtl_iadm_accum_word(wg, slot, 0u)]",
            "accum_out[(wg * 18u + slot) * 2u]",
        )
        self._assert_detected(failures, "outside vmaf_mtl_iadm_accum_word()")

    def test_host_slot_table_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "(uint64_t)accum[vmaf_mtl_iadm_accum_word(wg, slot, 0u)];",
            "(uint64_t)accum[(wg * 9u + slot) * 2u];",
        )
        self._assert_detected(failures, "the slots terms")

    def test_int32_view_of_scale0_band_is_detected(self) -> None:
        # Defect 2: scale 1 through the int32 kernel.
        failures = self._edited(
            HOST,
            "return scale == 1 ? IADM_METAL_DWT_VERT_S1 : IADM_METAL_DWT_VERT_S123;",
            "return IADM_METAL_DWT_VERT_S123;",
        )
        self._assert_detected(failures, "the parent terms")

    def test_int32_scale1_kernel_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "kernel void integer_adm_dwt_vert_s1(const device short *parent_ref_band",
            "kernel void integer_adm_dwt_vert_s1(const device int *parent_ref_band",
        )
        self._assert_detected(failures, "reads the int16 band of scale 0")

    def test_positive_masking_rounding_is_detected(self) -> None:
        # Defect 3: the kernels' former +2^31.
        failures = self._edited(
            KERNEL,
            "        int flt = vmaf_mtl_iadm_i4_masking_term(IADM_I4_FIX_ONE_BY_30, csf, "
            "c.i4_add_shift_flt,\n",
            "        const long add_bef_shift_flt = (long)(1u << 31);\n"
            "        int flt = vmaf_mtl_iadm_i4_masking_term(IADM_I4_FIX_ONE_BY_30, csf, "
            "add_bef_shift_flt,\n",
        )
        self._assert_detected(failures, "not INT32_MIN")
        self._assert_detected(failures, "does not call the header")

    def test_host_rounding_not_from_cpu_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "c->i4_add_shift_flt = cm.add_bef_shift_flt;",
            "c->i4_add_shift_flt = INT32_MAX;",
        )
        self._assert_detected(failures, "the rounding terms")

    def test_half_denominator_rounding_is_detected(self) -> None:
        # Defect 4.
        failures = self._edited(
            HOST,
            "c->den_add_shift_sq = den.add_shift_sq;",
            "c->den_add_shift_sq = 1u << (den.shift_sq - 1u);",
        )
        self._assert_detected(failures, "the denominator terms")
        self._assert_detected(failures, "2^(shift - 1)")

    def test_scale0_aim_under_skip_is_detected(self) -> None:
        # Defect 5.
        failures = self._edited(
            HOST,
            "        out[1] = 1e-10f; /* the CPU's den = 1e-10, no numerator, no AIM */\n"
            "        return;\n",
            "        out[1] = 1e-10f; /* the CPU's den = 1e-10, no numerator, no AIM */\n",
        )
        self._assert_detected(failures, "without a numerator or AIM")

    def test_float_noise_weight_is_detected(self) -> None:
        # Defect 6.
        failures = self._edited(
            HOST,
            "out[0] = iadm_cm_result(o, scale, w, h, t.cm, o->adm_noise_weight);",
            "out[0] = iadm_cm_result(o, scale, w, h, t.cm, (float)o->adm_noise_weight);",
        )
        self._assert_detected(failures, "narrowed to float")

    def test_score_formula_copy_in_dispatch_is_detected(self) -> None:
        failures = self._edited(
            MM,
            "static int emit_scores(",
            "static float conclude_adm_cm(void) { return powf(2.0f, 3.0f); }\n"
            "static int emit_scores(",
        )
        self._assert_detected(failures, "own uniforms, slots or score formulas")

    def test_unguarded_split_is_detected(self) -> None:
        failures = self._edited(
            SHARED,
            "#if !defined(__METAL_VERSION__) /* Metal has no double: the host splits. */\n",
            "\n",
        )
        self._assert_detected(failures, "kept out of Metal")

    def test_changed_reference_gain_limit_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "        rst = (int32_t)(ADM_KERNEL_MIN((rst * gain), t));",
            "        rst = (int32_t)(ADM_KERNEL_MIN((int32_t)(rst * (float)gain), t));",
        )
        self._assert_detected(failures, "the twin mirrors it")

    def test_changed_reference_rounding_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "        add_bef_shift_flt[idx] = (int32_t)(1u << (i4_shift_flt[idx] - 1));",
            "        add_bef_shift_flt[idx] = (int32_t)(1u << (i4_shift_flt[idx] - 2));",
        )
        self._assert_detected(failures, "the twin mirrors it")

    def test_changed_reciprocal_table_is_detected(self) -> None:
        failures = self._edited(
            TABLE,
            "        const int32_t recip = (int32_t)(div_Q_factor / i);",
            "        const int32_t recip = (int32_t)((float)div_Q_factor / (float)i);",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
