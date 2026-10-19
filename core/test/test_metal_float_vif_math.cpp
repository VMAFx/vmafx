/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1498: the arithmetic float_vif_metal's kernels run
 * (core/src/feature/metal/metal_float_vif_math.h, the Metal spelling of
 * core/src/feature/sycl/sycl_float_vif_math.h, ADR-1422), compiled on the
 * host and held against the CPU extractor's own routines, value by value.
 *
 * The header holds vif_pixel_statistic_s() and log2f_approx() for a device
 * without an fp64 type (Metal Shading Language Specification 4.1, section
 * 2.1). The reference evaluates two expressions in fp64, because
 * `vif_sigma_nsq` is a double:
 *
 *     1.0f + (g * g * sigma1_sq) / (sv_sq + vif_sigma_nsq)
 *     1.0f + (sigma1_sq) / (vif_sigma_nsq)
 *
 * The header evaluates them as exact fp32 pairs and, where a pair lies next
 * to an fp32 rounding boundary, replays the reference's fp64 operations in
 * 64-bit integers (metal_soft_double.h). This test checks:
 *
 *   - the nine polynomial coefficients against the C initialiser of
 *     vif_tools.c's `float log2_poly_s[9]`, and log2's special values;
 *   - the raw sample conversion against picture_copy() on 8-, 10-, 12- and
 *     16-bit pictures, every sample value;
 *   - the argument block the host builds for the kernels against
 *     `vif_sigma_nsq` (the fp64 value is rebuilt exactly from it);
 *   - the pixel statistic against vif_statistic_s() on a 1x1 plane, which
 *     returns exactly one pixel's numerator and denominator term, over
 *     inputs that reach every branch and several values of vif_sigma_nsq
 *     (zero included) and vif_enhn_gain_limit;
 *   - the two fp64 expressions against the compiler's own fp64, three ways:
 *     as the kernels select, by the integer replay alone on every sample, and
 *     by the pair alone; samples next to a rounding boundary, built so that
 *     the replay is taken; and twelve operands on which the pair alone rounds
 *     to the wrong value;
 *   - the row-ordered sums against vif_statistic_s() on planes whose sides
 *     are no multiple of anything;
 *   - the whole per-frame pipeline, composed in the kernels' order (decimate,
 *     vertical pass, horizontal pass with the statistic, row sums) from the
 *     header's helpers and vif_get_filter()'s taps, against compute_vif(), at
 *     8 to 16 bits, default and non-default options and every kernelscale of
 *     a range that includes the widest filter (69 taps).
 *
 * A fixed table of taps, a device log2, an fp32 vif_sigma_nsq or a per-block
 * reduction each fail at least one of these. A host replay cannot see the
 * device's scheduling, its compiler or its denormal handling, and does not
 * compile the .metal source: core/test/test_metal_float_vif_exact_contract.py
 * pins the kernels to the constructs replayed here, and
 * core/test/test_metal_float_vif_parity.c measures the device.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "test.h"
#include "float_bits.h"

#include "feature/metal/metal_float_vif_math.h"

extern "C" {
#include "feature/vif.h"
#include "feature/vif_tools.h"
#include "libvmaf/picture.h"
#include "mem.h"
#include "picture.h"
#include "feature/picture_copy.h"
}

namespace
{

constexpr unsigned kStatSamples = 200000u;
constexpr unsigned kRatioSamples = 2000000u;
constexpr unsigned kBoundarySamples = 400000u;

/* Deterministic generator: the same inputs on every host. */
uint32_t g_rng = 0x2545f491u;

uint32_t rng_next()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

/* Uniform in [0, 1). */
float rng_unit()
{
    return static_cast<float>(rng_next() >> 8) * (1.0f / 16777216.0f);
}

uint32_t bits_of(float value)
{
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool same_bits(float a, float b)
{
    return (a != a && b != b) || bits_of(a) == bits_of(b);
}

const double kSigmaNsq[] = {2.0, 1.5, 0.3, 5.0, 0.0};
const double kGainLimit[] = {100.0, 1.0, 1.7};

/* ------------------------------------------------------------------ */
/* log2f_approx()                                                      */
/* ------------------------------------------------------------------ */

const char *test_log2_coefficients_are_the_references()
{
    /* vif_tools.c::log2_poly_s: each decimal literal rounded to fp32 by the
     * initialiser of a `float` array. */
    const float reference[9] = {
        -0.012671635276421f, 0.064841182402670f, -0.157048836463065f, 0.257167726303123f,
        -0.353800560300520f, 0.480131410397451f, -0.721314327952201f,
        // NOLINTNEXTLINE(modernize-use-std-numbers): the reference's polynomial coefficient, not log2(e), vif_tools.c log2_poly_s, ADR-1498
        1.442694803896991f, 0.0f};
    const uint32_t pattern[9] = {
        VMAF_MTL_FVIF_LOG2_C0, VMAF_MTL_FVIF_LOG2_C1, VMAF_MTL_FVIF_LOG2_C2,
        VMAF_MTL_FVIF_LOG2_C3, VMAF_MTL_FVIF_LOG2_C4, VMAF_MTL_FVIF_LOG2_C5,
        VMAF_MTL_FVIF_LOG2_C6, VMAF_MTL_FVIF_LOG2_C7, VMAF_MTL_FVIF_LOG2_C8};
    for (unsigned i = 0u; i < 9u; i++) {
        if (pattern[i] != bits_of(reference[i])) {
            (void)fprintf(stderr, "\ncoefficient %u: %08x vs %08x\n", i, pattern[i],
                          bits_of(reference[i]));
        }
        mu_assert("a log2 coefficient pattern is not the reference's fp32 literal",
                  pattern[i] == bits_of(reference[i]));
    }
    return nullptr;
}

/* The values log2f_approx() special-cases, and the arguments the statistic
 * never reaches: call the header directly for them. */
const char *test_log2_special_values()
{
    mu_assert("log2(1) must be exactly 0", vmaf_mtl_fvif_log2(1.0f) == 0.0f);
    mu_assert("log2(2) must be exactly 1", vmaf_mtl_fvif_log2(2.0f) == 1.0f);
    mu_assert("log2(0) must be -inf", vmaf_test_identical_f32(vmaf_mtl_fvif_log2(0.0f), -INFINITY));
    const float negative = vmaf_mtl_fvif_log2(-1.0f);
    mu_assert("log2(negative) must be NaN", negative != negative);
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* Raw samples                                                         */
/* ------------------------------------------------------------------ */

/* A 400P picture of `bpc` bits, 256 samples wide, whose samples are 0, 1, 2,
 * ... in raster order (the pictures the kernels read are the same planes): the
 * float planes picture_copy() makes of it against the header's conversion. */
const char *check_raw_samples(unsigned bpc, unsigned count)
{
    const unsigned w = 256u;
    const unsigned h = count / w;
    VmafPicture pic;
    std::memset(&pic, 0, sizeof(pic));
    mu_assert("vmaf_picture_alloc failed",
              vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV400P, bpc, w, h) == 0);
    for (unsigned y = 0u; y < h; y++) {
        for (unsigned x = 0u; x < w; x++) {
            uint8_t *row = static_cast<uint8_t *>(pic.data[0]) + y * pic.stride[0];
            if (bpc <= 8u) {
                row[x] = static_cast<uint8_t>(y * w + x);
            } else {
                reinterpret_cast<uint16_t *>(row)[x] = static_cast<uint16_t>(y * w + x);
            }
        }
    }
    std::vector<float> plane(count);
    picture_copy(plane.data(), static_cast<ptrdiff_t>(w * sizeof(float)), &pic, -128, bpc, 0);
    unsigned wrong = 0u;
    for (unsigned i = 0u; i < count; i++) {
        wrong += same_bits(plane[i], vmaf_mtl_fvif_raw_to_float(i, bpc)) ? 0u : 1u;
    }
    (void)vmaf_picture_unref(&pic);
    mu_assert("a raw sample is not what picture_copy() reads", wrong == 0u);
    return nullptr;
}

const char *test_raw_samples_are_picture_copy()
{
    mu_assert_msg(check_raw_samples(8u, 256u));
    mu_assert_msg(check_raw_samples(10u, 1024u));
    mu_assert_msg(check_raw_samples(12u, 4096u));
    mu_assert_msg(check_raw_samples(16u, 65536u));
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* The argument block                                                  */
/* ------------------------------------------------------------------ */

const char *test_statistic_arguments_rebuild_the_double()
{
    for (const double nsq : kSigmaNsq) {
        const VmafMtlFvifStatisticArgs a = vmaf_mtl_fvif_statistic_args(nsq, 100.0);
        const VmafMtlFvifStatParams p = vmaf_mtl_fvif_stat_params(a);
        const uint64_t mant = (static_cast<uint64_t>(a.noise_mant_hi) << 32) | a.noise_mant_lo;
        const double rebuilt = std::ldexp(static_cast<double>(mant), a.noise_exp);
        mu_assert("the significand and exponent do not rebuild vif_sigma_nsq",
                  vmaf_test_identical_f64(rebuilt, nsq));
        mu_assert("the pair does not hold vif_sigma_nsq to 2^-46",
                  std::fabs(static_cast<double>(p.noise.hi) + static_cast<double>(p.noise.lo) -
                            nsq) <= nsq * 0x1p-46);
        mu_assert("`above` is below vif_sigma_nsq", static_cast<double>(p.noise.above) >= nsq);
        mu_assert("`above` is not the smallest such fp32 value",
                  nsq == 0.0 || static_cast<double>(std::nextafter(p.noise.above, 0.0f)) < nsq);
        mu_assert(
            "sigma_max_inv is not vif_statistic_s()'s",
            same_bits(a.sigma_max_inv, static_cast<float>(std::pow(static_cast<float>(nsq), 2.0f) /
                                                          (255.0 * 255.0))));
    }
    const VmafMtlFvifStatisticArgs a = vmaf_mtl_fvif_statistic_args(2.0, 1.7);
    mu_assert("the gain limit is not (float)vif_enhn_gain_limit",
              same_bits(a.gain_limit, static_cast<float>(1.7)));
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* The pixel statistic                                                 */
/* ------------------------------------------------------------------ */

struct Moments {
    float mu1, mu2, xx, yy, xy;
};

/* One pixel's five moments. `kind` selects the variance regime so that every
 * branch of vif_pixel_statistic_s() is reached: flat reference, flat
 * distortion, variances below and above sigma_nsq, negative covariance and
 * moments that make a variance negative before the clamp. */
Moments random_moments(unsigned kind)
{
    static const float scale[] = {0.0f, 1.0e-11f, 1.0e-3f, 1.5f, 40.0f, 3000.0f, 16000.0f};
    const unsigned levels = static_cast<unsigned>(sizeof(scale) / sizeof(scale[0]));
    const float s1 = scale[kind % levels] * rng_unit();
    const float s2 = scale[(kind / levels) % levels] * rng_unit();
    const float rho = 2.0f * rng_unit() - 1.0f;
    Moments m;
    m.mu1 = 255.0f * rng_unit() - 128.0f;
    m.mu2 = 255.0f * rng_unit() - 128.0f;
    m.xx = s1 + m.mu1 * m.mu1;
    m.yy = s2 + m.mu2 * m.mu2;
    m.xy = rho * std::sqrt(s1 * s2) + m.mu1 * m.mu2;
    if (kind % 11u == 0u) {
        m.xx = m.mu1 * m.mu1 - 1.0e-3f * rng_unit();
    }
    return m;
}

/* The CPU's terms for one pixel: vif_statistic_s() on a 1x1 plane. */
void cpu_terms(const Moments &m, double nsq, double egl, float *num, float *den)
{
    vif_statistic_s(&m.mu1, &m.mu2, &m.xx, &m.yy, &m.xy, num, den, 1, 1,
                    static_cast<int>(sizeof(float)), static_cast<int>(sizeof(float)),
                    static_cast<int>(sizeof(float)), static_cast<int>(sizeof(float)),
                    static_cast<int>(sizeof(float)), egl, nsq);
}

/* The samples of one (sigma_nsq, gain limit) pair that differ from the CPU's. */
unsigned statistic_mismatches(double nsq, double egl, unsigned already_reported)
{
    const VmafMtlFvifStatParams p =
        vmaf_mtl_fvif_stat_params(vmaf_mtl_fvif_statistic_args(nsq, egl));
    unsigned differing = 0u;
    for (unsigned i = 0u; i < kStatSamples / 5u; i++) {
        const Moments m = random_moments(i);
        float cpu_num = 0.0f;
        float cpu_den = 0.0f;
        cpu_terms(m, nsq, egl, &cpu_num, &cpu_den);
        const VmafMtlFvifTerm t = vmaf_mtl_fvif_pixel_term(m.mu1, m.mu2, m.xx, m.yy, m.xy, p);
        if (same_bits(t.num, cpu_num) && same_bits(t.den, cpu_den)) {
            continue;
        }
        if (already_reported + differing++ < 5u) {
            (void)fprintf(stderr,
                          "\nsample %u (nsq=%g egl=%g): num %.9g vs cpu %.9g, den %.9g vs "
                          "cpu %.9g",
                          i, nsq, egl, static_cast<double>(t.num), static_cast<double>(cpu_num),
                          static_cast<double>(t.den), static_cast<double>(cpu_den));
        }
    }
    return differing;
}

const char *test_statistic_is_vif_pixel_statistic_s()
{
    g_rng = 0x2545f491u;
    unsigned differing = 0u;
    for (const double nsq : kSigmaNsq) {
        for (const double egl : kGainLimit) {
            differing += statistic_mismatches(nsq, egl, differing);
        }
    }
    if (differing != 0u) {
        (void)fprintf(stderr, "\n%u samples differ\n", differing);
    }
    mu_assert("metal_float_vif_math.h differs from vif_pixel_statistic_s()", differing == 0u);
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* 1.0f + numerator / denominator                                      */
/* ------------------------------------------------------------------ */

/* What the reference computes: the quotient and the sum in fp64, one rounding
 * to fp32. */
float reference_ratio(float numerator, bool has_addend, float addend, double sigma_nsq)
{
    const double denominator = has_addend ? static_cast<double>(addend) + sigma_nsq : sigma_nsq;
    return static_cast<float>(1.0 + static_cast<double>(numerator) / denominator);
}

struct RatioCounts {
    unsigned selected_wrong;
    unsigned replay_wrong;
    unsigned pair_wrong;
    unsigned replays;
};

void check_ratio(float numerator, bool has_addend, float addend, double sigma_nsq,
                 RatioCounts *counts)
{
    const VmafMtlFvifStatParams p =
        vmaf_mtl_fvif_stat_params(vmaf_mtl_fvif_statistic_args(sigma_nsq, 100.0));
    const VmafMtlFvifDenominator denominator = has_addend ?
                                                   vmaf_mtl_fvif_noise_plus(addend, p.noise) :
                                                   vmaf_mtl_fvif_noise_denominator(p.noise);
    const VmafMtlFvifFf sum = vmaf_mtl_fvif_one_plus_ratio_pair(numerator, denominator.pair);
    const float selected = vmaf_mtl_fvif_one_plus_ratio(numerator, denominator);
    const float replay = vmaf_mtl_fvif_one_plus_ratio_replayed(numerator, denominator.exact);
    counts->replays += vmaf_mtl_fvif_needs_replay(numerator, sum, denominator) ? 1u : 0u;
    const float reference = reference_ratio(numerator, has_addend, addend, sigma_nsq);
    if ((!same_bits(selected, reference) || !same_bits(replay, reference)) &&
        counts->selected_wrong + counts->replay_wrong < 5u) {
        (void)fprintf(stderr,
                      "\nnumerator=%a addend=%a (%d) nsq=%.17g: fp64 %a, selected %a, replay %a",
                      static_cast<double>(numerator), static_cast<double>(addend),
                      has_addend ? 1 : 0, sigma_nsq, static_cast<double>(reference),
                      static_cast<double>(selected), static_cast<double>(replay));
    }
    counts->selected_wrong += same_bits(selected, reference) ? 0u : 1u;
    counts->replay_wrong += same_bits(replay, reference) ? 0u : 1u;
    counts->pair_wrong += same_bits(sum.hi, reference) ? 0u : 1u;
}

/* Random operands over the ranges the statistic produces: products of a gain
 * below 100 and a variance below 2^14, residual variances from 1e-10 up. */
const char *test_ratio_is_the_fp64_expression()
{
    static const float scale[] = {1.0e-9f, 1.0e-3f, 1.5f, 40.0f, 3000.0f, 16000.0f};
    RatioCounts counts = {
        .selected_wrong = 0u, .replay_wrong = 0u, .pair_wrong = 0u, .replays = 0u};
    g_rng = 0x9e3779b9u;
    for (unsigned i = 0u; i < kRatioSamples; i++) {
        /* The zero variance has its own test: a zero denominator makes the
         * integer replay's quotient a division by zero. */
        const double sigma_nsq = kSigmaNsq[i % 4u];
        const float variance = scale[(i / 4u) % 6u] * rng_unit() + 1.0e-30f;
        const float residual = scale[(i / 24u) % 6u] * rng_unit() + 1.0e-10f;
        const float gain = (i & 64u) ? 3.0f * rng_unit() : 100.0f * rng_unit();
        const float product = gain * gain * variance + 1.0e-30f;
        check_ratio(product, true, residual, sigma_nsq, &counts);
        check_ratio(variance, false, 0.0f, sigma_nsq, &counts);
    }
    mu_assert("the integer replay is not the reference's fp64 arithmetic",
              counts.replay_wrong == 0u);
    mu_assert("the selected value is not the reference's fp64 arithmetic",
              counts.selected_wrong == 0u);
    return nullptr;
}

/* Operands built to land next to a rounding boundary of the sum, where the
 * kernels take the integer replay: the quotient is (k + 1/2) fp32 steps of a
 * sum in [1, 2), for small k, so that the numerator's own rounding leaves it
 * within a few thousandths of a step of the boundary. The test also requires
 * that most of these samples do take the replay, so it cannot pass by never
 * reaching it. */
const char *test_ratio_next_to_a_rounding_boundary()
{
    RatioCounts counts = {
        .selected_wrong = 0u, .replay_wrong = 0u, .pair_wrong = 0u, .replays = 0u};
    g_rng = 0x1b873593u;
    for (unsigned i = 0u; i < kBoundarySamples; i++) {
        const double sigma_nsq = kSigmaNsq[i % 4u];
        const bool has_addend = ((i / 4u) % 2u) != 0u;
        const float addend = 40.0f * rng_unit() + 1.0e-10f;
        const double denominator = has_addend ? static_cast<double>(addend) + sigma_nsq : sigma_nsq;
        const double steps = static_cast<double>(rng_next() % 1024u);
        const float numerator = static_cast<float>(denominator * ((steps + 0.5) * 0x1p-23));
        check_ratio(numerator, has_addend, addend, sigma_nsq, &counts);
    }
    mu_assert("next to a rounding boundary the integer replay is not the reference's fp64",
              counts.replay_wrong == 0u);
    mu_assert("next to a rounding boundary the selected value is not the reference's fp64",
              counts.selected_wrong == 0u);
    if (counts.replays < kBoundarySamples / 2u) {
        (void)fprintf(stderr, "\nonly %u of %u boundary samples take the replay\n", counts.replays,
                      kBoundarySamples);
    }
    mu_assert("the boundary samples do not reach the integer replay",
              counts.replays >= kBoundarySamples / 2u);
    (void)fprintf(stderr, "[replay taken on %u of %u, pair alone wrong on %u] ", counts.replays,
                  kBoundarySamples, counts.pair_wrong);
    return nullptr;
}

/* Operands on which the pair alone rounds to the wrong fp32 value: found by a
 * search over 8.4e9 random quotients (85 such operands, about one in 1e8;
 * none where the selected value was wrong). Most are exact ties, where the
 * reference's two roundings and the pair's one disagree. The kernels must
 * take the replay on each, and the test fails if the selection is removed. */
struct RatioWitness {
    float numerator;
    bool has_addend;
    float addend;
    double sigma_nsq;
};

const char *test_pair_alone_is_not_the_fp64_expression()
{
    static const RatioWitness witness[] = {
        {.numerator = 0x1.2b38p-11f,
         .has_addend = true,
         .addend = 0x1.543d0cp-33f,
         .sigma_nsq = 5.0},
        {.numerator = 0x1.800aa2p-22f,
         .has_addend = true,
         .addend = 0x1.c5b018p-13f,
         .sigma_nsq = 2.0},
        {.numerator = 0x1.4f1662p-10f,
         .has_addend = true,
         .addend = 0x1.234676p-13f,
         .sigma_nsq = 5.0},
        {.numerator = 0x1.18p-18f, .has_addend = true, .addend = 0x1.febc18p-31f, .sigma_nsq = 2.0},
        {.numerator = 0x1.416ce8p+20f,
         .has_addend = true,
         .addend = 0x1.11354ap+3f,
         .sigma_nsq = 0.3},
        {.numerator = 0x1.2f3788p+2f,
         .has_addend = true,
         .addend = 0x1.374cfp+0f,
         .sigma_nsq = 4.7},
        {.numerator = 0x1.653388p+5f,
         .has_addend = true,
         .addend = 0x1.3bd38p-11f,
         .sigma_nsq = 4.7},
        {.numerator = 0x1.333334p-26f, .has_addend = false, .addend = 0.0f, .sigma_nsq = 0.3},
        {.numerator = 0x1.241e6ep+1f,
         .has_addend = true,
         .addend = 0x1.68cc86p-12f,
         .sigma_nsq = 1.5},
        {.numerator = 0x1.69570ep+26f,
         .has_addend = true,
         .addend = 0x1.e4383cp+3f,
         .sigma_nsq = 4.7},
        {.numerator = 0x1.3949p+17f,
         .has_addend = true,
         .addend = 0x1.55ae86p+0f,
         .sigma_nsq = 1.5},
        {.numerator = 0x1.f2p-18f, .has_addend = true, .addend = 0x1.069bb8p-31f, .sigma_nsq = 1.5},
    };
    const unsigned count = static_cast<unsigned>(sizeof(witness) / sizeof(witness[0]));
    RatioCounts counts = {
        .selected_wrong = 0u, .replay_wrong = 0u, .pair_wrong = 0u, .replays = 0u};
    for (const RatioWitness &w : witness) {
        check_ratio(w.numerator, w.has_addend, w.addend, w.sigma_nsq, &counts);
    }
    mu_assert("the selected value is not the reference's fp64 on a witness",
              counts.selected_wrong == 0u && counts.replay_wrong == 0u);
    mu_assert("every witness must take the integer replay", counts.replays == count);
    mu_assert("the witnesses no longer show the pair alone rounding differently",
              counts.pair_wrong == count);
    return nullptr;
}

/* A zero `vif_sigma_nsq` makes the denominator's argument 0 / 0 for a flat
 * reference: the reference's NaN, read by log2f_approx() through its
 * significand. The statistic over a flat plane must equal the CPU's. */
const char *test_zero_noise_flat_plane_is_the_references()
{
    const Moments flat = {.mu1 = 10.0f, .mu2 = 12.0f, .xx = 100.0f, .yy = 144.0f, .xy = 120.0f};
    for (const double egl : kGainLimit) {
        float cpu_num = 0.0f;
        float cpu_den = 0.0f;
        cpu_terms(flat, 0.0, egl, &cpu_num, &cpu_den);
        const VmafMtlFvifStatParams p =
            vmaf_mtl_fvif_stat_params(vmaf_mtl_fvif_statistic_args(0.0, egl));
        const VmafMtlFvifTerm t =
            vmaf_mtl_fvif_pixel_term(flat.mu1, flat.mu2, flat.xx, flat.yy, flat.xy, p);
        mu_assert("a flat pixel at vif_sigma_nsq = 0: numerator differs from the CPU's",
                  same_bits(t.num, cpu_num));
        mu_assert("a flat pixel at vif_sigma_nsq = 0: denominator differs from the CPU's",
                  same_bits(t.den, cpu_den));
    }
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* The kernels, on the host                                            */
/* ------------------------------------------------------------------ */

/* A float plane, stride = width, as the kernels read and write it. */
struct Plane {
    int w = 0;
    int h = 0;
    std::vector<float> v;
};

Plane make_plane(int width, int height)
{
    Plane plane;
    plane.w = width;
    plane.h = height;
    plane.v.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0f);
    return plane;
}

float plane_at(const Plane &plane, int x, int y)
{
    return plane.v[static_cast<size_t>(y) * static_cast<size_t>(plane.w) + static_cast<size_t>(x)];
}

void plane_set(Plane &plane, int x, int y, float value)
{
    plane.v[static_cast<size_t>(y) * static_cast<size_t>(plane.w) + static_cast<size_t>(x)] = value;
}

/* float_vif.metal's filter taps: the CPU's own, per scale. */
struct Filters {
    float taps[4][128];
    int count[4];
};

int filters_init(Filters &f, float kernelscale)
{
    std::memset(f.taps, 0, sizeof(f.taps));
    for (int scale = 0; scale < 4; scale++) {
        f.count[scale] = vif_get_filter_size(scale, kernelscale);
        if (f.count[scale] < 1 || f.count[scale] > 128) {
            return -1;
        }
        vif_get_filter(f.taps[scale], scale, kernelscale);
    }
    return 0;
}

/* float_vif_vertical for one pixel: the five moments, taps in order. */
void kernel_vertical(const Filters &f, int scale, const Plane &ref, const Plane &dis, int x, int y,
                     float m[5])
{
    float mu1 = 0.0f;
    float mu2 = 0.0f;
    float xx = 0.0f;
    float yy = 0.0f;
    float xy = 0.0f;
    const int half = f.count[scale] / 2;
    for (int k = 0; k < f.count[scale]; k++) {
        const int row = vmaf_mtl_fvif_mirror(y - half + k, ref.h);
        const float c = f.taps[scale][k];
        const float r = plane_at(ref, x, row);
        const float d = plane_at(dis, x, row);
        const float rr = r * r;
        const float dd = d * d;
        const float rd = r * d;
        mu1 = vmaf_mtl_fvif_tap(mu1, c, r);
        mu2 = vmaf_mtl_fvif_tap(mu2, c, d);
        xx = vmaf_mtl_fvif_tap(xx, c, rr);
        yy = vmaf_mtl_fvif_tap(yy, c, dd);
        xy = vmaf_mtl_fvif_tap(xy, c, rd);
    }
    m[0] = mu1;
    m[1] = mu2;
    m[2] = xx;
    m[3] = yy;
    m[4] = xy;
}

/* float_vif_compute for one pixel: the horizontal pass, then the statistic;
 * the terms go to `terms` at vmaf_mtl_fvif_term_index(). */
void kernel_compute(const Filters &f, int scale, const Plane moments[5],
                    const VmafMtlFvifStatParams &p, int x, int y, std::vector<float> &terms)
{
    float mu1 = 0.0f;
    float mu2 = 0.0f;
    float xx = 0.0f;
    float yy = 0.0f;
    float xy = 0.0f;
    const int w = moments[0].w;
    const int half = f.count[scale] / 2;
    for (int k = 0; k < f.count[scale]; k++) {
        const int col = vmaf_mtl_fvif_mirror(x - half + k, w);
        const float c = f.taps[scale][k];
        mu1 = vmaf_mtl_fvif_tap(mu1, c, plane_at(moments[0], col, y));
        mu2 = vmaf_mtl_fvif_tap(mu2, c, plane_at(moments[1], col, y));
        xx = vmaf_mtl_fvif_tap(xx, c, plane_at(moments[2], col, y));
        yy = vmaf_mtl_fvif_tap(yy, c, plane_at(moments[3], col, y));
        xy = vmaf_mtl_fvif_tap(xy, c, plane_at(moments[4], col, y));
    }
    const VmafMtlFvifTerm t = vmaf_mtl_fvif_pixel_term(mu1, mu2, xx, yy, xy, p);
    const size_t at =
        vmaf_mtl_fvif_term_index(static_cast<vmaf_mtl_u32>(x), static_cast<vmaf_mtl_u32>(y),
                                 static_cast<vmaf_mtl_u32>(moments[0].h));
    terms[at] = t.num;
    terms[at + 1u] = t.den;
}

/* float_vif_row_sums for row y: left to right into one fp32 accumulator. */
void kernel_row_sums(const std::vector<float> &terms, int w, int h, int y, std::vector<float> &rows)
{
    float num = 0.0f;
    float den = 0.0f;
    for (int x = 0; x < w; x++) {
        const size_t at =
            vmaf_mtl_fvif_term_index(static_cast<vmaf_mtl_u32>(x), static_cast<vmaf_mtl_u32>(y),
                                     static_cast<vmaf_mtl_u32>(h));
        num += terms[at];
        den += terms[at + 1u];
    }
    rows[static_cast<size_t>(y)] = num;
    rows[static_cast<size_t>(h) + static_cast<size_t>(y)] = den;
}

/* float_vif_decimate for one output pixel: vertical taps inside. */
void kernel_decimate(const Filters &f, int scale, const Plane &in_ref, const Plane &in_dis, int x,
                     int y, float *out_ref, float *out_dis)
{
    float acc_ref = 0.0f;
    float acc_dis = 0.0f;
    const int half = f.count[scale] / 2;
    for (int kj = 0; kj < f.count[scale]; kj++) {
        const float c_j = f.taps[scale][kj];
        const int px = vmaf_mtl_fvif_mirror(2 * x - half + kj, in_ref.w);
        float v_ref = 0.0f;
        float v_dis = 0.0f;
        for (int ki = 0; ki < f.count[scale]; ki++) {
            const float c_i = f.taps[scale][ki];
            const int py = vmaf_mtl_fvif_mirror(2 * y - half + ki, in_ref.h);
            v_ref = vmaf_mtl_fvif_tap(v_ref, c_i, plane_at(in_ref, px, py));
            v_dis = vmaf_mtl_fvif_tap(v_dis, c_i, plane_at(in_dis, px, py));
        }
        acc_ref = vmaf_mtl_fvif_tap(acc_ref, c_j, v_ref);
        acc_dis = vmaf_mtl_fvif_tap(acc_dis, c_j, v_dis);
    }
    *out_ref = acc_ref;
    *out_dis = acc_dis;
}

/* The host's sum of the rows of one scale: top to bottom into one fp32
 * accumulator, then widened (compute_vif() stores the floats as doubles). */
void host_sum_rows(const std::vector<float> &rows, int h, double *num, double *den)
{
    float n = 0.0f;
    float d = 0.0f;
    for (int y = 0; y < h; y++) {
        n += rows[static_cast<size_t>(y)];
        d += rows[static_cast<size_t>(h) + static_cast<size_t>(y)];
    }
    *num = static_cast<double>(n);
    *den = static_cast<double>(d);
}

/* One scale of the pipeline: the three kernels over (ref, dis). */
void run_scale(const Filters &f, int scale, const Plane &ref, const Plane &dis,
               const VmafMtlFvifStatParams &p, double *num, double *den)
{
    Plane moments[5];
    for (Plane &plane : moments) {
        plane = make_plane(ref.w, ref.h);
    }
    for (int y = 0; y < ref.h; y++) {
        for (int x = 0; x < ref.w; x++) {
            float m[5];
            kernel_vertical(f, scale, ref, dis, x, y, m);
            for (int i = 0; i < 5; i++) {
                plane_set(moments[i], x, y, m[i]);
            }
        }
    }
    std::vector<float> terms(static_cast<size_t>(ref.w) * static_cast<size_t>(ref.h) * 2u);
    for (int y = 0; y < ref.h; y++) {
        for (int x = 0; x < ref.w; x++) {
            kernel_compute(f, scale, moments, p, x, y, terms);
        }
    }
    std::vector<float> rows(static_cast<size_t>(ref.h) * 2u);
    for (int y = 0; y < ref.h; y++) {
        kernel_row_sums(terms, ref.w, ref.h, y, rows);
    }
    host_sum_rows(rows, ref.h, num, den);
}

/* The next scale's planes: `scale`'s filter over the pair, every second
 * sample. */
void next_scale(const Filters &f, int scale, Plane *ref, Plane *dis)
{
    Plane out_ref = make_plane(ref->w / 2, ref->h / 2);
    Plane out_dis = make_plane(ref->w / 2, ref->h / 2);
    for (int y = 0; y < out_ref.h; y++) {
        for (int x = 0; x < out_ref.w; x++) {
            float r = 0.0f;
            float d = 0.0f;
            kernel_decimate(f, scale, *ref, *dis, x, y, &r, &d);
            plane_set(out_ref, x, y, r);
            plane_set(out_dis, x, y, d);
        }
    }
    *ref = out_ref;
    *dis = out_dis;
}

/* A sample of a synthetic frame after picture_copy() with offset -128. */
float frame_sample(int x, int y, unsigned bpc, bool distorted)
{
    const unsigned peak = (1u << bpc) - 1u;
    unsigned v =
        (static_cast<unsigned>(x * 7 + y * 13) ^ static_cast<unsigned>((x * y) >> 2)) & peak;
    if (distorted) {
        v = (v + (static_cast<unsigned>(x * 3 + y * 5) % 23u) * (peak / 255u)) & peak;
    }
    return vmaf_mtl_fvif_raw_to_float(v, bpc);
}

struct Frames {
    Plane ref;
    Plane dis;
    std::vector<float> cpu_ref; /* stride in bytes: aligned */
    std::vector<float> cpu_dis;
    int cpu_stride_bytes = 0;
};

/* The pair, as the float planes both sides read: stride = width for the
 * kernels' planes, an aligned stride for compute_vif(). */
Frames make_frames(int w, int h, unsigned bpc)
{
    Frames f;
    f.ref = make_plane(w, h);
    f.dis = make_plane(w, h);
    f.cpu_stride_bytes = static_cast<int>(ALIGN_CEIL(static_cast<size_t>(w) * sizeof(float)));
    const size_t stride_floats = static_cast<size_t>(f.cpu_stride_bytes) / sizeof(float);
    f.cpu_ref.assign(stride_floats * static_cast<size_t>(h), 0.0f);
    f.cpu_dis.assign(stride_floats * static_cast<size_t>(h), 0.0f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float r = frame_sample(x, y, bpc, false);
            const float d = frame_sample(x, y, bpc, true);
            plane_set(f.ref, x, y, r);
            plane_set(f.dis, x, y, d);
            f.cpu_ref[static_cast<size_t>(y) * stride_floats + static_cast<size_t>(x)] = r;
            f.cpu_dis[static_cast<size_t>(y) * stride_floats + static_cast<size_t>(x)] = d;
        }
    }
    return f;
}

/* compute_vif() as float_vif.c calls it: the four filters precomputed. */
bool cpu_scores(const Frames &fr, const Filters &flt, float kernelscale, double nsq, double egl,
                double cpu[8])
{
    float filters[4][128];
    int widths[4];
    for (int s = 0; s < 4; s++) {
        std::memcpy(filters[s], flt.taps[s], sizeof(filters[s]));
        widths[s] = flt.count[s];
    }
    double score = 0.0;
    double score_num = 0.0;
    double score_den = 0.0;
    return compute_vif(fr.cpu_ref.data(), fr.cpu_dis.data(), fr.ref.w, fr.ref.h,
                       fr.cpu_stride_bytes, fr.cpu_stride_bytes, &score, &score_num, &score_den,
                       cpu, egl, static_cast<double>(kernelscale), 0, nsq,
                       reinterpret_cast<const float (*)[128]>(filters), widths) == 0;
}

const char *check_pipeline(int w, int h, unsigned bpc, float kernelscale, double nsq, double egl)
{
    Filters flt = {};
    mu_assert("a filter is outside the kernels' 128 taps", filters_init(flt, kernelscale) == 0);
    mu_assert("the frame is below the four-scale ladder's floor",
              w >= vif_get_min_dim(kernelscale) && h >= vif_get_min_dim(kernelscale));
    const Frames fr = make_frames(w, h, bpc);
    double cpu[8] = {0};
    mu_assert("compute_vif() failed", cpu_scores(fr, flt, kernelscale, nsq, egl, cpu));
    const VmafMtlFvifStatParams p =
        vmaf_mtl_fvif_stat_params(vmaf_mtl_fvif_statistic_args(nsq, egl));
    Plane ref = fr.ref;
    Plane dis = fr.dis;
    for (int scale = 0; scale < 4; scale++) {
        if (scale > 0) {
            next_scale(flt, scale, &ref, &dis);
        }
        double num = 0.0;
        double den = 0.0;
        run_scale(flt, scale, ref, dis, p, &num, &den);
        const size_t ni = static_cast<size_t>(scale) * 2u;
        if (!vmaf_test_identical_f64(num, cpu[ni]) || !vmaf_test_identical_f64(den, cpu[ni + 1u])) {
            (void)fprintf(stderr,
                          "\n%dx%d bpc=%u ks=%g scale %d: num %.17g vs cpu %.17g, den %.17g vs "
                          "cpu %.17g\n",
                          w, h, bpc, static_cast<double>(kernelscale), scale, num, cpu[ni], den,
                          cpu[ni + 1u]);
        }
        mu_assert("the pipeline's numerator differs from compute_vif()",
                  vmaf_test_identical_f64(num, cpu[ni]));
        mu_assert("the pipeline's denominator differs from compute_vif()",
                  vmaf_test_identical_f64(den, cpu[ni + 1u]));
    }
    return nullptr;
}

const char *test_pipeline_is_compute_vif()
{
    mu_assert_msg(check_pipeline(96, 64, 8u, 1.0f, 2.0, 100.0));
    mu_assert_msg(check_pipeline(96, 64, 10u, 1.0f, 2.0, 100.0));
    mu_assert_msg(check_pipeline(96, 64, 16u, 1.0f, 2.0, 100.0));
    /* Odd sizes: 50x38 halves to 25x19, 12x9 and 6x4, so no scale is a
     * multiple of any tile. */
    mu_assert_msg(check_pipeline(50, 38, 8u, 1.0f, 2.0, 100.0));
    mu_assert_msg(check_pipeline(50, 38, 12u, 1.0f, 1.5, 1.0));
    mu_assert_msg(check_pipeline(67, 41, 8u, 1.0f, 0.3, 1.7));
    /* The smallest frame the extractors accept. */
    mu_assert_msg(check_pipeline(16, 16, 8u, 1.0f, 2.0, 100.0));
    return nullptr;
}

/* Every kernelscale, the widest filters included (69 taps at 4.0, which no
 * tile holds): the taps are kernel arguments and the planes are read through
 * the mirror. */
const char *test_pipeline_runs_every_kernelscale()
{
    static const float kernelscale[] = {0.1f, 0.5f, 1.5f, 2.0f, 3.0f, 4.0f};
    for (const float ks : kernelscale) {
        const int side = vif_get_min_dim(ks);
        mu_assert_msg(check_pipeline(side + 24, side + 8, 8u, ks, 2.0, 100.0));
        mu_assert_msg(check_pipeline(side, side, 10u, ks, 1.5, 1.7));
    }
    return nullptr;
}

/* The row-ordered sum is vif_statistic_s()'s own, on planes no block size
 * divides: pins the order of the two fp32 accumulations. */
const char *check_reduction(int w, int h, double nsq, double egl)
{
    const int stride_bytes = static_cast<int>(ALIGN_CEIL(static_cast<size_t>(w) * sizeof(float)));
    const size_t stride = static_cast<size_t>(stride_bytes) / sizeof(float);
    std::vector<float> m[5];
    for (std::vector<float> &plane : m) {
        plane.assign(stride * static_cast<size_t>(h), 0.0f);
    }
    const VmafMtlFvifStatParams p =
        vmaf_mtl_fvif_stat_params(vmaf_mtl_fvif_statistic_args(nsq, egl));
    std::vector<float> terms(static_cast<size_t>(w) * static_cast<size_t>(h) * 2u);
    g_rng = 0x2545f491u;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const Moments r = random_moments(static_cast<unsigned>(y * w + x));
            const size_t at = static_cast<size_t>(y) * stride + static_cast<size_t>(x);
            m[0][at] = r.mu1;
            m[1][at] = r.mu2;
            m[2][at] = r.xx;
            m[3][at] = r.yy;
            m[4][at] = r.xy;
            const VmafMtlFvifTerm t = vmaf_mtl_fvif_pixel_term(r.mu1, r.mu2, r.xx, r.yy, r.xy, p);
            const size_t out =
                vmaf_mtl_fvif_term_index(static_cast<vmaf_mtl_u32>(x), static_cast<vmaf_mtl_u32>(y),
                                         static_cast<vmaf_mtl_u32>(h));
            terms[out] = t.num;
            terms[out + 1u] = t.den;
        }
    }
    float cpu_num = 0.0f;
    float cpu_den = 0.0f;
    vif_statistic_s(m[0].data(), m[1].data(), m[2].data(), m[3].data(), m[4].data(), &cpu_num,
                    &cpu_den, w, h, stride_bytes, stride_bytes, stride_bytes, stride_bytes,
                    stride_bytes, egl, nsq);
    std::vector<float> rows(static_cast<size_t>(h) * 2u);
    for (int y = 0; y < h; y++) {
        kernel_row_sums(terms, w, h, y, rows);
    }
    double num = 0.0;
    double den = 0.0;
    host_sum_rows(rows, h, &num, &den);
    mu_assert("the row-ordered numerator differs from vif_statistic_s()",
              vmaf_test_identical_f64(num, static_cast<double>(cpu_num)));
    mu_assert("the row-ordered denominator differs from vif_statistic_s()",
              vmaf_test_identical_f64(den, static_cast<double>(cpu_den)));
    return nullptr;
}

const char *test_row_sums_are_vif_statistic_s()
{
    mu_assert_msg(check_reduction(96, 64, 2.0, 100.0));
    mu_assert_msg(check_reduction(67, 41, 2.0, 100.0));
    mu_assert_msg(check_reduction(33, 17, 1.5, 1.0));
    mu_assert_msg(check_reduction(1, 1, 2.0, 100.0));
    return nullptr;
}

/* The layout of the argument blocks: every field is four bytes, so the host
 * struct is the MSL struct (no padding on either side). */
const char *test_argument_blocks_have_no_padding()
{
    mu_assert("VmafMtlFvifFilterArgs is not four u32", sizeof(VmafMtlFvifFilterArgs) == 16u);
    mu_assert("VmafMtlFvifInputArgs is not three u32", sizeof(VmafMtlFvifInputArgs) == 12u);
    mu_assert("VmafMtlFvifDecimateArgs is not five u32", sizeof(VmafMtlFvifDecimateArgs) == 20u);
    mu_assert("VmafMtlFvifRowArgs is not two u32", sizeof(VmafMtlFvifRowArgs) == 8u);
    mu_assert("VmafMtlFvifStatisticArgs is not eight 4-byte fields",
              sizeof(VmafMtlFvifStatisticArgs) == 32u);
    return nullptr;
}

/* The statistic tests. */
mu_message_t run_statistic_tests()
{
    mu_run_test(test_statistic_is_vif_pixel_statistic_s);
    mu_run_test(test_zero_noise_flat_plane_is_the_references);
    return nullptr;
}

/* The ratio, pipeline and reduction tests. */
mu_message_t run_pipeline_tests()
{
    mu_run_test(test_ratio_is_the_fp64_expression);
    mu_run_test(test_ratio_next_to_a_rounding_boundary);
    mu_run_test(test_pair_alone_is_not_the_fp64_expression);
    mu_run_test(test_row_sums_are_vif_statistic_s);
    mu_run_test(test_pipeline_is_compute_vif);
    mu_run_test(test_pipeline_runs_every_kernelscale);
    return nullptr;
}

} // namespace

mu_message_t run_tests(void)
{
    mu_run_test(test_log2_coefficients_are_the_references);
    mu_run_test(test_log2_special_values);
    mu_run_test(test_raw_samples_are_picture_copy);
    mu_run_test(test_statistic_arguments_rebuild_the_double);
    mu_run_test(test_argument_blocks_have_no_padding);
    mu_assert_msg(run_statistic_tests());
    mu_assert_msg(run_pipeline_tests());
    return nullptr;
}
