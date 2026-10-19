/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP compute kernels for the float_adm feature extractor (T7-10b batch-2 /
 *  ADR-0468; AIM per ADR-0574; the CPU's arithmetic since ADR-1458). They are
 *  the kernels of the CUDA twin (cuda/float_adm/float_adm_score.cu, ADR-1420)
 *  for a HIP device.
 *
 *  Stages, per scale:
 *    0 — DWT vertical pass (ref+dis fused)
 *    1 — DWT horizontal pass (ref+dis fused) → 4 sub-bands
 *    2 — decouple, then the CSF of decouple_a and of decouple_r
 *    3 — per-sample terms of the three reductions over the reduced region
 *    4 — one fp32 sum per row and slot
 *
 *  The twin returns the CPU extractor's values bit for bit. The arithmetic of
 *  stages 2 to 4 lives in feature/float_adm_gpu_common.h, which the CUDA twin
 *  and the device-free host test compile as well; the kernels below only move
 *  samples between the buffers and those functions. A sum over a tile of
 *  threads cannot be the reference's, whose accumulators are sequential fp32,
 *  so stage 3 stores the terms and stage 4 adds each row left to right; the
 *  host adds the rows top to bottom.
 *
 *  The shared header's rounding operations are plain C operators here. That
 *  is the reference's arithmetic because this file is built with the strict
 *  FP list of core/src/meson.build (ADR-1407): no contraction, and an fp32
 *  `/` that is the correctly rounded quotient, which is what the reference's
 *  DIVS() is since ADR-1442. The fp64 product and sum of the gain limit and
 *  the two CSF constants are IEEE operations on the device. The DWT taps of
 *  stages 0 and 1 are plain fp32 products and sums in the reference's order.
 *  adm_p_norm other than 1 or 3 raises each term with the device's powf(),
 *  which is not the C library's: that option is close to the CPU, not equal.
 */

#include <hip/hip_runtime.h>
#include <cstddef>
#include <cstdint>

/* feature/float_adm_gpu_common.h in the spelling of a HIP device. */
#include "feature/hip/float_adm/float_adm_hip_math.h"

#define FADM_LO0 (0.482962913144690f)
#define FADM_LO1 (0.836516303737469f)
#define FADM_LO2 (0.224143868041857f)
#define FADM_LO3 (-0.129409522550921f)
#define FADM_HI0 (-0.129409522550921f)
#define FADM_HI1 (-0.224143868041857f)
#define FADM_HI2 (0.836516303737469f)
#define FADM_HI3 (-0.482962913144690f)

namespace
{

/* A device address the host passes in a kernel-argument struct as uint64_t
 * (the struct is shared with the C host, float_adm_gpu_common.h). */
template <typename T> __device__ __forceinline__ T *fadm_device_ptr(uint64_t address)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr): kernel-argument device address (ADR-1458)
    return reinterpret_cast<T *>(address);
}

__device__ __forceinline__ int fadm_mirror(int idx, int sup)
{
    /* Both axes use `2*sup - idx - 1` — matches CPU
     * dwt2_src_indices_filt_s in adm_tools.c (the only mirror form
     * the float ADM CPU pipeline uses). */
    if (idx < 0) {
        idx = -idx;
    } else if (idx >= sup) {
        idx = 2 * sup - idx - 1;
    }
    /* A one-sample input has nothing to mirror to: the form above yields 1
     * and -1 there, which the reference reads outside its buffer. Stay
     * inside; every longer input is unaffected. */
    if (idx >= sup)
        idx = sup - 1;
    return (idx < 0) ? 0 : idx;
}

__device__ __forceinline__ float fadm_read_src_pixel(const uint8_t *plane, ptrdiff_t stride_bytes,
                                                     int y, int x, int w, int h, unsigned bpc,
                                                     float scaler, float pixel_offset)
{
    y = fadm_mirror(y, h);
    if (x < 0)
        x = 0;
    if (x >= w)
        x = w - 1;
    if (bpc <= 8u) {
        return (float)plane[y * stride_bytes + x] + pixel_offset;
    }
    const uint16_t v = reinterpret_cast<const uint16_t *>(plane + y * stride_bytes)[x];
    return (float)v / scaler + pixel_offset;
}

__device__ __forceinline__ float fadm_read_band_a(const float *band_buf, int buf_stride, int half_h,
                                                  int parent_w, int parent_h, int y, int x)
{
    /* Parent LL band read: parent dims = cur_w/cur_h here. The buffer
     * is 4 sub-bands packed contiguous. Band 0 = LL. */
    y = fadm_mirror(y, parent_h);
    if (x < 0)
        x = 0;
    if (x >= parent_w)
        x = parent_w - 1;
    /* Note: integer cast on buf_stride/half_h to silence -Wconversion. */
    (void)half_h;
    return band_buf[y * buf_stride + x];
}

__device__ __forceinline__ float fadm_read_dwt_tmp(const float *dwt_tmp, int gy, int x_sub,
                                                   int cur_w, int half_offset)
{
    x_sub = fadm_mirror(x_sub, cur_w);
    const int stride = cur_w * 2;
    return dwt_tmp[gy * stride + half_offset + x_sub];
}

} /* namespace */

extern "C" {

/* ------------------------------------------------------------------
 * Stage 0 — DWT vertical pass.
 *
 * Output row n consumes input rows (2n - 1 .. 2n + 2). Layout of
 * dwt_tmp:  [gy * (cur_w * 2) + gx]              = lo
 *           [gy * (cur_w * 2) + cur_w + gx]      = hi
 * Z dimension fuses ref+dis (z=0 → ref plane, z=1 → dis plane).
 *
 * For SCALE > 0, the input is the parent LL band (band 0 of the
 * previous-scale ref_band/dis_band buffer). The scale-0 path reads
 * from the raw u8/u16 source.
 * ------------------------------------------------------------------ */
__global__ void float_adm_dwt_vert(int scale, const uint8_t *ref_raw, const uint8_t *dis_raw,
                                   ptrdiff_t raw_stride, const float *parent_ref_band,
                                   const float *parent_dis_band, int parent_buf_stride,
                                   int parent_half_h, int parent_w, int parent_h,
                                   float *dwt_tmp_ref, float *dwt_tmp_dis, int cur_w, int cur_h,
                                   int half_h, unsigned bpc, float scaler, float pixel_offset)
{
    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    const int plane_is_dis = (int)blockIdx.z;
    if (gx >= cur_w || gy >= half_h)
        return;
    (void)half_h;

    const int row_start = 2 * gy - 1;
    float s[4];
#pragma unroll
    for (int k = 0; k < 4; k++) {
        if (scale == 0) {
            const uint8_t *plane = (plane_is_dis == 0) ? ref_raw : dis_raw;
            s[k] = fadm_read_src_pixel(plane, raw_stride, row_start + k, gx, cur_w, cur_h, bpc,
                                       scaler, pixel_offset);
        } else {
            const float *band = (plane_is_dis == 0) ? parent_ref_band : parent_dis_band;
            s[k] = fadm_read_band_a(band, parent_buf_stride, parent_half_h, parent_w, parent_h,
                                    row_start + k, gx);
        }
    }

    const float lo = FADM_LO0 * s[0] + FADM_LO1 * s[1] + FADM_LO2 * s[2] + FADM_LO3 * s[3];
    const float hi = FADM_HI0 * s[0] + FADM_HI1 * s[1] + FADM_HI2 * s[2] + FADM_HI3 * s[3];

    const int out_stride = cur_w * 2;
    float *dst = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    dst[gy * out_stride + gx] = lo;
    dst[gy * out_stride + cur_w + gx] = hi;
}

/* ------------------------------------------------------------------
 * Stage 1 — DWT horizontal pass.
 *
 * Reads lo / hi sub-rows from stage 0; emits 4 bands (a/h/v/d) with
 *   band_a = lo · lo (LL); band_h = hi · lo (HL high-V);
 *   band_v = lo · hi (LH high-H); band_d = hi · hi (HH).
 * Same a/h/v/d order as integer ADM convention.
 * ------------------------------------------------------------------ */
__global__ void float_adm_dwt_hori(int scale, const float *dwt_tmp_ref, const float *dwt_tmp_dis,
                                   float *ref_band, float *dis_band, int cur_w, int half_w,
                                   int half_h, int buf_stride)
{
    (void)scale;
    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    const int plane_is_dis = (int)blockIdx.z;
    if (gx >= half_w || gy >= half_h)
        return;

    const float *src = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    float *dst = (plane_is_dis == 0) ? ref_band : dis_band;

    const int base_x = 2 * gx;
    /* lo sub-row taps. */
    const float l0 = fadm_read_dwt_tmp(src, gy, base_x - 1, cur_w, 0);
    const float l1 = fadm_read_dwt_tmp(src, gy, base_x + 0, cur_w, 0);
    const float l2 = fadm_read_dwt_tmp(src, gy, base_x + 1, cur_w, 0);
    const float l3 = fadm_read_dwt_tmp(src, gy, base_x + 2, cur_w, 0);
    const float a_val = FADM_LO0 * l0 + FADM_LO1 * l1 + FADM_LO2 * l2 + FADM_LO3 * l3;
    const float v_val = FADM_HI0 * l0 + FADM_HI1 * l1 + FADM_HI2 * l2 + FADM_HI3 * l3;

    /* hi sub-row taps. */
    const float h0 = fadm_read_dwt_tmp(src, gy, base_x - 1, cur_w, cur_w);
    const float h1 = fadm_read_dwt_tmp(src, gy, base_x + 0, cur_w, cur_w);
    const float h2 = fadm_read_dwt_tmp(src, gy, base_x + 1, cur_w, cur_w);
    const float h3 = fadm_read_dwt_tmp(src, gy, base_x + 2, cur_w, cur_w);
    const float h_val = FADM_LO0 * h0 + FADM_LO1 * h1 + FADM_LO2 * h2 + FADM_LO3 * h3;
    const float d_val = FADM_HI0 * h0 + FADM_HI1 * h1 + FADM_HI2 * h2 + FADM_HI3 * h3;

    const int slice = buf_stride * half_h;
    dst[0 * slice + gy * buf_stride + gx] = a_val;
    dst[1 * slice + gy * buf_stride + gx] = h_val;
    dst[2 * slice + gy * buf_stride + gx] = v_val;
    dst[3 * slice + gy * buf_stride + gx] = d_val;
}

/* ------------------------------------------------------------------
 * Stage 2 — decouple + CSF.
 *
 * adm_decouple_s() followed by both adm_csf_s() calls of compute_adm():
 * decouple_a filtered for the adm2 numerator's masking threshold and the AIM
 * numerator's signal, decouple_r filtered for the other two. Runs over the
 * whole band; the reference stops a tap outside the reduced region, and
 * every sample is a function of its own position only.
 * ------------------------------------------------------------------ */
__global__ void float_adm_decouple_csf(const FloatAdmGpuDecoupleArgs a)
{
    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    const FloatAdmGpuBands *bd = &a.bands;
    if (gx >= bd->half_w || gy >= bd->half_h)
        return;

    const float *ref_band = fadm_device_ptr<const float>(bd->ref_band);
    const float *dis_band = fadm_device_ptr<const float>(bd->dis_band);
    float *csf_a = fadm_device_ptr<float>(bd->csf_a);
    float *csf_fa = fadm_device_ptr<float>(bd->csf_fa);
    float *csf_r = fadm_device_ptr<float>(bd->csf_r);
    float *csf_fr = fadm_device_ptr<float>(bd->csf_fr);

    float o[FADM_BANDS];
    float t[FADM_BANDS];
#pragma unroll
    for (int b = 0; b < FADM_BANDS; b++) {
        const size_t idx = fadm_band_index(b + 1, gy, gx, bd->buf_stride, bd->half_h);
        o[b] = ref_band[idx];
        t[b] = dis_band[idx];
    }
    const int angle_flag = fadm_angle_flag(o[0], o[1], t[0], t[1], a.cos_1deg_sq);

#pragma unroll
    for (int b = 0; b < FADM_BANDS; b++) {
        const FloatAdmCsfSample c = fadm_decouple_csf(&a, b, o[b], t[b], angle_flag);
        const size_t idx = fadm_band_index(b, gy, gx, bd->buf_stride, bd->half_h);
        csf_a[idx] = c.csf_a;
        csf_fa[idx] = c.csf_fa;
        csf_r[idx] = c.csf_r;
        csf_fr[idx] = c.csf_fr;
    }
}

/* ------------------------------------------------------------------
 * Stage 3 — the terms adm_csf_den_scale_s() and the two adm_cm_s() calls of
 * compute_adm() accumulate, for one sample of the reduced region:
 *   DEN  |rfactor * ref|^p
 *   CM   decouple_r masked by the threshold of decouple_a (adm2 numerator)
 *   AIM  decouple_a masked by the threshold of decouple_r (AIM numerator)
 * ------------------------------------------------------------------ */
__global__ void float_adm_terms(const FloatAdmGpuTermArgs a)
{
    const uint32_t rx = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t ry = blockIdx.y * blockDim.y + threadIdx.y;
    if (rx >= a.region_w || ry >= a.region_h)
        return;
    const FloatAdmGpuBands *bd = &a.bands;
    const int x = a.left + (int)rx;
    const int y = a.top + (int)ry;

    const float *ref_band = fadm_device_ptr<const float>(bd->ref_band);
    const float *csf_a = fadm_device_ptr<const float>(bd->csf_a);
    const float *csf_fa = fadm_device_ptr<const float>(bd->csf_fa);
    const float *csf_r = fadm_device_ptr<const float>(bd->csf_r);
    const float *csf_fr = fadm_device_ptr<const float>(bd->csf_fr);
    float *terms = fadm_device_ptr<float>(a.terms);

    float thr_a = 0.0f;
    float thr_r = 0.0f;
    if (a.bypass_cm == 0u) {
        thr_a = fadm_threshold(csf_a, csf_fa, bd, y, x);
        thr_r = fadm_threshold(csf_r, csf_fr, bd, y, x);
    }

#pragma unroll
    for (uint32_t b = 0u; b < FADM_BANDS; b++) {
        const float src = ref_band[fadm_band_index((int)b + 1, y, x, bd->buf_stride, bd->half_h)];
        const size_t idx = fadm_band_index((int)b, y, x, bd->buf_stride, bd->half_h);
        terms[fadm_term_index(FADM_SLOT_DEN + b, rx, ry, a.region_w, a.region_h)] =
            fadm_den_term(bd->rfactor[b], src, a.is_cube, a.p_norm);
        terms[fadm_term_index(FADM_SLOT_CM + b, rx, ry, a.region_w, a.region_h)] =
            fadm_cm_term(csf_r[idx], thr_a, a.is_cube, a.p_norm);
        terms[fadm_term_index(FADM_SLOT_AIM + b, rx, ry, a.region_w, a.region_h)] =
            fadm_cm_term(csf_a[idx], thr_r, a.is_cube, a.p_norm);
    }
}

/* ------------------------------------------------------------------
 * Stage 4 — one thread per (slot, row) adds that row's terms left to right
 * in fp32, the reference's per-row accumulator.
 * ------------------------------------------------------------------ */
__global__ void float_adm_row_sums(const FloatAdmGpuRowArgs a)
{
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= FADM_TERM_SLOTS * a.region_h)
        return;
    const uint32_t slot = id / a.region_h;
    const uint32_t y = id - slot * a.region_h;

    const float *terms = fadm_device_ptr<const float>(a.terms);
    float *rows = fadm_device_ptr<float>(a.rows);
    rows[fadm_row_index(slot, y, a.region_h)] = fadm_row_sum(
        terms + fadm_term_index(slot, 0u, y, a.region_w, a.region_h), a.region_h, a.region_w);
}

} /* extern "C" */
