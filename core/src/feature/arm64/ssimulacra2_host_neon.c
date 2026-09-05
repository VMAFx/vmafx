/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * aarch64 NEON host-kernel variants for the ssimulacra2 Vulkan extractor
 * (ADR-0242). 4-wide float lanes; structurally mirrors the AVX2 sibling
 * (ssimulacra2_host_avx2.c) and the standalone NEON kernels in
 * ssimulacra2_neon.c.
 *
 * The only difference from `ssimulacra2_linear_rgb_to_xyb_neon` is the
 * `plane_stride` parameter: channel pointers are `base + p * plane_stride`
 * instead of `base + p * w*h`.
 *
 * Bit-exact contract: ADR-0161 / ADR-0242 — per-lane scalar cbrtf,
 * `#pragma STDC FP_CONTRACT OFF` (gated by a -Wunknown-pragmas push
 * so older GCC keeps quiet), compiled with `-ffp-contract=off`.
 */

#include <arm_neon.h>
#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "feature/ssimulacra2_math.h"
#include "ssimulacra2_host_neon.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#pragma STDC FP_CONTRACT OFF
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static const float kM00 = 0.30f;
static const float kM02 = 0.078f;
static const float kM10 = 0.23f;
static const float kM12 = 0.078f;
static const float kM20 = 0.24342268924547819f;
static const float kM21 = 0.20476744424496821f;
static const float kOpsinBias = 0.0037930732552754493f;

static inline float32x4_t cbrtf_lane4(float32x4_t v)
{
    alignas(16) float tmp[4];
    vst1q_f32(tmp, v);
    for (int k = 0; k < 4; k++) {
        tmp[k] = vmaf_ss2_cbrtf(tmp[k]);
    }
    return vld1q_f32(tmp);
}

/* ADR-0252 carve-out: matmul + per-lane cbrtf + rescale kept together for
 * line-for-line diff against the host scalar reference. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — ADR-0141 §2 /
// ADR-0252 / ADR-0161 / ADR-0278: host XYB kernel kept whole for the bit-exact audit.
void ssimulacra2_host_linear_rgb_to_xyb_neon(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    assert(plane_stride >= (size_t)w * (size_t)h);

    const float *rp = lin;
    const float *gp = lin + plane_stride;
    const float *bp = lin + 2u * plane_stride;
    float *xp = xyb;
    float *yp = xyb + plane_stride;
    float *bxp = xyb + 2u * plane_stride;

    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);

    const float32x4_t vm00 = vdupq_n_f32(kM00);
    const float32x4_t vm01 = vdupq_n_f32(m01);
    const float32x4_t vm02 = vdupq_n_f32(kM02);
    const float32x4_t vm10 = vdupq_n_f32(kM10);
    const float32x4_t vm11 = vdupq_n_f32(m11);
    const float32x4_t vm12 = vdupq_n_f32(kM12);
    const float32x4_t vm20 = vdupq_n_f32(kM20);
    const float32x4_t vm21 = vdupq_n_f32(kM21);
    const float32x4_t vm22 = vdupq_n_f32(m22);
    const float32x4_t vbias = vdupq_n_f32(kOpsinBias);
    const float32x4_t vzero = vdupq_n_f32(0.0f);
    const float32x4_t vcbrt_bias = vdupq_n_f32(cbrt_bias);
    const float32x4_t vhalf = vdupq_n_f32(0.5f);
    const float32x4_t v14 = vdupq_n_f32(14.0f);
    const float32x4_t v42 = vdupq_n_f32(0.42f);
    const float32x4_t v55 = vdupq_n_f32(0.55f);
    const float32x4_t v01 = vdupq_n_f32(0.01f);

    const size_t scale_pixels = (size_t)w * (size_t)h;
    size_t i = 0;

    for (; i + 4 <= scale_pixels; i += 4) {
        const float32x4_t r = vld1q_f32(rp + i);
        const float32x4_t g = vld1q_f32(gp + i);
        const float32x4_t b = vld1q_f32(bp + i);
        /* LMS mixing — left-to-right addition order matches scalar reference. */
        float32x4_t l = vaddq_f32(vmulq_f32(vm00, r), vmulq_f32(vm01, g));
        l = vaddq_f32(l, vmulq_f32(vm02, b));
        l = vaddq_f32(l, vbias);
        float32x4_t m = vaddq_f32(vmulq_f32(vm10, r), vmulq_f32(vm11, g));
        m = vaddq_f32(m, vmulq_f32(vm12, b));
        m = vaddq_f32(m, vbias);
        float32x4_t sv = vaddq_f32(vmulq_f32(vm20, r), vmulq_f32(vm21, g));
        sv = vaddq_f32(sv, vmulq_f32(vm22, b));
        sv = vaddq_f32(sv, vbias);
        l = vmaxq_f32(l, vzero);
        m = vmaxq_f32(m, vzero);
        sv = vmaxq_f32(sv, vzero);

        const float32x4_t L = vsubq_f32(cbrtf_lane4(l), vcbrt_bias);
        const float32x4_t M = vsubq_f32(cbrtf_lane4(m), vcbrt_bias);
        const float32x4_t S = vsubq_f32(cbrtf_lane4(sv), vcbrt_bias);

        /* X = 0.5*(L-M); Y = 0.5*(L+M); B = (S-Y)+0.55; X=14X+0.42; Y+=0.01 */
        const float32x4_t X = vmulq_f32(vhalf, vsubq_f32(L, M));
        const float32x4_t Y = vmulq_f32(vhalf, vaddq_f32(L, M));
        const float32x4_t Bfinal = vaddq_f32(vsubq_f32(S, Y), v55);
        const float32x4_t Xfinal = vaddq_f32(vmulq_f32(X, v14), v42);
        const float32x4_t Yfinal = vaddq_f32(Y, v01);

        vst1q_f32(xp + i, Xfinal);
        vst1q_f32(yp + i, Yfinal);
        vst1q_f32(bxp + i, Bfinal);
    }

    /* Scalar tail — bit-identical to ss2v_host_linear_rgb_to_xyb body. */
    for (; i < scale_pixels; i++) {
        float r = rp[i];
        float g = gp[i];
        float bb = bp[i];
        float lv = kM00 * r + m01 * g + kM02 * bb + kOpsinBias;
        float mv = kM10 * r + m11 * g + kM12 * bb + kOpsinBias;
        float sv = kM20 * r + kM21 * g + m22 * bb + kOpsinBias;
        if (lv < 0.0f)
            lv = 0.0f;
        if (mv < 0.0f)
            mv = 0.0f;
        if (sv < 0.0f)
            sv = 0.0f;
        float L = vmaf_ss2_cbrtf(lv) - cbrt_bias;
        float M = vmaf_ss2_cbrtf(mv) - cbrt_bias;
        float S = vmaf_ss2_cbrtf(sv) - cbrt_bias;
        float X = 0.5f * (L - M);
        float Y = 0.5f * (L + M);
        float B = S;
        B = (B - Y) + 0.55f;
        X = X * 14.0f + 0.42f;
        Y = Y + 0.01f;
        xp[i] = X;
        yp[i] = Y;
        bxp[i] = B;
    }
}

void ssimulacra2_host_downsample_2x2_neon(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride)
{
    assert(in != NULL);
    assert(out != NULL);
    assert(iw > 0 && ih > 0);
    assert(plane_stride >= (size_t)iw * (size_t)ih);

    const float32x4_t vquarter = vdupq_n_f32(0.25f);

    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * plane_stride;
        float *op = out + (size_t)c * plane_stride;
        for (unsigned oy = 0; oy < oh; oy++) {
            const unsigned iy0 = oy * 2;
            const unsigned iy1 = (iy0 + 1 < ih) ? iy0 + 1 : ih - 1;
            const float *row0 = ip + (size_t)iy0 * iw;
            const float *row1 = ip + (size_t)iy1 * iw;
            float *orow = op + (size_t)oy * ow;
            unsigned ox = 0;
            /* SIMD interior: 4 output lanes at a time. `vuzp1q_f32` extracts
             * even positions, `vuzp2q_f32` extracts odd — equivalent to the
             * AVX2 shuffle+permute. Sequential adds preserve summation order. */
            const unsigned interior_end = (ow > 0u && iw >= 2u) ? (((ow - 1u) / 4u) * 4u) : 0u;
            for (; ox < interior_end; ox += 4) {
                const size_t base = (size_t)ox * 2u;
                const float32x4_t r0a = vld1q_f32(row0 + base);
                const float32x4_t r0b = vld1q_f32(row0 + base + 4);
                const float32x4_t r1a = vld1q_f32(row1 + base);
                const float32x4_t r1b = vld1q_f32(row1 + base + 4);
                /* Deinterleave even / odd sample pairs across r0a:r0b. */
                const float32x4_t r0e = vuzp1q_f32(r0a, r0b);
                const float32x4_t r0o = vuzp2q_f32(r0a, r0b);
                const float32x4_t r1e = vuzp1q_f32(r1a, r1b);
                const float32x4_t r1o = vuzp2q_f32(r1a, r1b);
                /* (r0e + r0o) + r1e + r1o — scalar summation order. */
                float32x4_t acc = vaddq_f32(r0e, r0o);
                acc = vaddq_f32(acc, r1e);
                acc = vaddq_f32(acc, r1o);
                vst1q_f32(orow + ox, vmulq_f32(acc, vquarter));
            }
            /* Scalar tail. */
            for (; ox < ow; ox++) {
                unsigned ix0 = ox * 2;
                unsigned ix1 = (ix0 + 1 < iw) ? ix0 + 1 : iw - 1;
                float sum = row0[ix0] + row0[ix1] + row1[ix0] + row1[ix1];
                orow[ox] = sum * 0.25f;
            }
        }
    }
}
