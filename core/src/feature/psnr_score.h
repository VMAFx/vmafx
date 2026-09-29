/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Option semantics of the integer `psnr` extractor, shared by the CPU
 *  reference (integer_psnr.c) and its GPU twins so that every backend turns
 *  the same per-plane SSE into the same score:
 *
 *    - `reduced_hbd_peak` -> vmaf_psnr_peak()
 *    - `min_sse`          -> vmaf_psnr_max() (the per-plane psnr_max ceiling)
 *    - `uncapped`         -> vmaf_psnr_from_mse() (ADR-1193)
 *    - `enable_apsnr`     -> vmaf_psnr_aggregate() (clip-aggregate APSNR)
 *
 *  The expressions are the ones integer_psnr.c shipped, moved here verbatim,
 *  so the CPU scores stay bit-identical. Host-side double arithmetic only:
 *  a GPU twin reduces the SSE on the device and calls these from its
 *  collect/flush callbacks, never from a kernel (SYCL: ADR-0220).
 */

#ifndef VMAF_FEATURE_PSNR_SCORE_H
#define VMAF_FEATURE_PSNR_SCORE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Peak sample value. `reduced_hbd_peak` scales the 8-bit peak (255) to the
 * source depth instead of using the full (1 << bpc) - 1 range, for HBD
 * content that was upconverted from 8-bit. Both forms give 255 at 8 bpc. */
static inline uint32_t vmaf_psnr_peak(unsigned bpc, bool reduced_hbd_peak)
{
    return reduced_hbd_peak ? 255u << (bpc - 8u) : (1u << bpc) - 1u;
}

/* Per-plane psnr_max: the finite stand-in for an all-zero SSE and, unless
 * `uncapped`, the ceiling of every computed value. A non-zero `min_sse`
 * derives it from the plane's sample count; otherwise it is 6 * bpc + 12. */
static inline double vmaf_psnr_max(unsigned bpc, uint32_t peak, double min_sse, unsigned width,
                                   unsigned height)
{
    if (min_sse != 0.0) {
        const double mse = min_sse / ((double)width * height);
        return ceil(10. * log10((double)peak * (double)peak / mse));
    }
    return (double)((6u * bpc) + 12u);
}

/*
 * Convert a per-plane MSE into a PSNR, keeping the two roles `psnr_max`
 * used to conflate strictly separate (ADR-1193, T-UPSTREAM-1109,
 * Netflix/vmaf#1109):
 *
 *   (a) infinity sentinel — `mse == 0` means the planes are byte-identical
 *       and the true PSNR is +inf, so a finite stand-in has to be reported.
 *       This role is unconditional and is what the golden 60 / 84 / 108 dB
 *       assertions pin.
 *   (b) hard truncation — every genuinely computed value above `psnr_max`
 *       was silently replaced by it, so an 8-bit pair differing by one
 *       luma step over 576x324 reported 60.000000 dB instead of its true
 *       100.840479 dB. `uncapped` drops role (b) only.
 *
 * The `uncapped == false` arm is the upstream expression
 * `MIN(10. * log10(peak_sq / MAX(mse, 1e-16)), psnr_max)` with the two
 * macros expanded in place, not a re-derivation of it, so the default is
 * bit-identical by construction. That matters in one corner: with a
 * `min_sse` below ~1.9e-11 the ceiling rises past the ~208 dB that a zero
 * MSE floored to 1e-16 produces, and a re-derived `mse == 0 -> psnr_max`
 * arm would report the ceiling where the shipped code reports 208 dB. The
 * 1e-16 floor is kept in the `uncapped` arm too, so a denormal MSE cannot
 * divide to infinity (unreachable below ~1e16 pixels anyway, since sse is
 * a positive integer).
 */
static inline double vmaf_psnr_from_mse(double mse, double peak_sq, double psnr_max, bool uncapped)
{
    const double floored = mse > 1e-16 ? mse : 1e-16;
    if (!uncapped) {
        const double psnr = 10. * log10(peak_sq / floored);
        return psnr < psnr_max ? psnr : psnr_max;
    }
    if (mse <= 0.)
        return psnr_max;
    return 10. * log10(peak_sq / floored);
}

/* Clip-aggregate APSNR over the SSE and sample count summed across every
 * frame of one plane. An all-zero SSE reports that plane's psnr_max; any
 * other value is capped at 10 * log10(peak^2 * n_pixels), rounded up. */
static inline double vmaf_psnr_aggregate(uint32_t peak, uint64_t sse, uint64_t n_pixels,
                                         double psnr_max)
{
    if (sse == 0u)
        return psnr_max;
    const double apsnr =
        10 * (log10((double)peak * (double)peak) + log10((double)n_pixels) - log10((double)sse));
    const double max_apsnr = ceil(10 * log10((double)peak * (double)peak * (double)n_pixels));
    return apsnr < max_apsnr ? apsnr : max_apsnr;
}

#endif /* VMAF_FEATURE_PSNR_SCORE_H */
