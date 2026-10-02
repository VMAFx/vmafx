/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Whole-path dispatch invariance for psnr_hvs: the same pictures must give
 *  the same score bits whichever calc_psnrhvs() the extractor binds.
 *
 *  psnr_hvs dispatches one function per plane: the scalar calc_psnrhvs()
 *  (third_party/xiph/psnr_hvs.c), calc_psnrhvs_avx2() on x86 and
 *  calc_psnrhvs_neon() on aarch64. test_psnr_hvs_neon and test_psnr_hvs_avx2
 *  compare the integer DCT alone, and test_psnr_hvs_simd compares the AVX2
 *  function with a copy of the scalar one kept inside the test. None of them
 *  reaches the shipped scalar function, and none ran the NEON function past
 *  its DCT. That is how calc_psnrhvs_neon() kept a float product in the
 *  masking threshold where the scalar reference has a double product
 *  (T-PSNR-HVS-NEON-NOT-SCALAR-BITS-2026-10-02): the DCT was exact, the mask
 *  was one float ulp off on about one block in twenty of real content.
 *
 *  This test drives the extractor through the public API, as the CLI's
 *  --cpumask does, once with the host's instruction set and once with every
 *  flag masked, and requires psnr_hvs_y, psnr_hvs_cb, psnr_hvs_cr and psnr_hvs
 *  to be the same bit patterns. It covers:
 *
 *    - blocks recorded from the Netflix pair src01_hrc00 / src01_hrc01
 *      (576x324, frames 18, 23, 33 and 0), one per plane, each of which the
 *      float product scored differently. One 8x8 picture holds one block per
 *      plane, so no later block can absorb the difference;
 *    - generated texture with an error of a few codes at 8, 10 and 12 bits,
 *      which reaches the same rounding cases without recorded data;
 *    - uniformly random samples at 8, 10 and 12 bits;
 *    - the fixtures of the CPU-vs-GPU comparison (psnr_hvs_twin_parity.h) in
 *      every chroma layout and at every depth from 8 to 12 bits;
 *    - edge inputs: identical pictures (an infinite score), flat planes, the
 *      largest error a depth allows, a one-sample difference, and sizes from
 *      one block up, including sizes whose last block does not reach the edge.
 *
 *  Bit-identical is the assertion, not a tolerance: the SIMD functions keep
 *  the scalar's order of every float operation (ADR-0159, ADR-0160, ADR-1207).
 *  On a host without AVX2 or NEON both runs take the scalar function and the
 *  test passes without asserting anything.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* HvsFixture, its patterns, hvs_fill_pic(), hvs_collect() and
 * hvs_score_bits(): shared with the twin comparisons, so a SIMD function and a
 * GPU twin are held to the scalar reference on the same pictures. */
#include "psnr_hvs_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Every instruction-set flag masked: the scalar calc_psnrhvs(). */
#define CPUMASK_SCALAR UINT64_MAX

/* Ref and dist of one 8x8 block per plane, as 8-bit samples in raster order.
 * The values in the comments are calc_psnrhvs() on that block alone: the
 * scalar reference, and what the float product returned. */
static const uint8_t FRAME_18[3][2][64] = {
    /* plane 0, x=280 y=0: 0x1.b6b87ep-12, with the float product 0x1.b6b87ap-12 */
    {{142, 118, 152, 176, 130, 139, 175, 175, 210, 160, 146, 144, 133, 131, 190, 221,
      186, 218, 199, 154, 161, 197, 220, 220, 211, 230, 206, 177, 163, 223, 208, 223,
      194, 159, 134, 165, 202, 191, 171, 210, 172, 89,  116, 196, 205, 157, 158, 203,
      97,  54,  73,  139, 197, 176, 143, 134, 59,  84,  72,  97,  78,  71,  65,  60},
     {133, 117, 167, 164, 132, 148, 180, 174, 189, 167, 148, 152, 132, 146, 190, 204,
      190, 221, 195, 152, 168, 199, 215, 218, 210, 227, 213, 181, 183, 208, 204, 211,
      203, 178, 151, 160, 190, 178, 172, 195, 170, 96,  124, 189, 204, 165, 175, 201,
      95,  63,  80,  133, 180, 176, 142, 136, 62,  73,  66,  75,  72,  80,  66,  66}},
    /* plane 1, x=35 y=0: 0x1.fe13a8p-16, with the float product 0x1.fe13a2p-16 */
    {{107, 94,  91,  92,  97,  99,  100, 98,  103, 97,  103, 105, 104, 102, 112, 112,
      102, 95,  97,  108, 108, 112, 109, 104, 99,  96,  97,  106, 110, 107, 108, 106,
      95,  101, 98,  98,  103, 111, 114, 107, 106, 108, 108, 102, 96,  96,  107, 108,
      108, 112, 114, 110, 97,  105, 112, 105, 114, 112, 113, 107, 103, 107, 106, 113},
     {106, 97,  96,  100, 105, 100, 101, 103, 102, 100, 101, 102, 101, 105, 109, 108,
      103, 98,  97,  102, 105, 107, 109, 107, 100, 97,  98,  104, 108, 109, 109, 107,
      100, 102, 102, 101, 103, 107, 110, 110, 104, 105, 108, 107, 98,  99,  107, 109,
      109, 109, 110, 110, 104, 105, 110, 107, 113, 113, 113, 111, 108, 110, 110, 113}},
    /* plane 2, x=238 y=0: 0x1.bdb8ap-14, with the float product 0x1.bdb89ep-14 */
    {{123, 125, 119, 116, 122, 123, 120, 119, 127, 128, 120, 118, 124, 121, 118, 120,
      120, 121, 116, 117, 122, 120, 120, 121, 116, 113, 115, 116, 118, 119, 121, 117,
      113, 112, 112, 114, 115, 120, 123, 122, 121, 123, 124, 118, 115, 118, 124, 119,
      126, 123, 127, 120, 119, 121, 118, 117, 129, 123, 123, 117, 119, 124, 122, 119},
     {125, 121, 117, 116, 118, 119, 118, 119, 127, 126, 122, 121, 120, 120, 120, 120,
      120, 120, 121, 123, 121, 120, 120, 120, 114, 114, 117, 120, 120, 119, 119, 119,
      114, 114, 116, 119, 119, 119, 119, 119, 119, 120, 123, 123, 121, 120, 118, 118,
      124, 125, 125, 124, 121, 120, 120, 119, 125, 125, 125, 123, 123, 122, 122, 121}},
};

static const uint8_t FRAME_23[3][2][64] = {
    /* plane 0, x=182 y=0: 0x1.f51ab6p-13, with the float product 0x1.f51abep-13 */
    {{80, 48, 65, 98, 81, 41, 35, 35, 58, 51, 62, 79, 73, 53, 56, 34,  46, 59, 53, 49, 59, 53,
      46, 43, 47, 49, 48, 40, 69, 82, 79, 61, 43, 47, 47, 44, 85, 105, 84, 61, 46, 43, 49, 48,
      59, 67, 50, 53, 44, 41, 44, 55, 47, 51, 40, 39, 42, 42, 61, 56,  45, 57, 45, 49},
     {70, 52, 66, 87, 81, 50, 41, 37, 50, 52, 62, 75, 71, 58, 50, 42, 49, 50, 52, 54, 60, 64,
      52, 52, 48, 48, 50, 50, 75, 85, 74, 65, 48, 47, 48, 48, 81, 92, 67, 59, 50, 47, 47, 48,
      60, 66, 51, 54, 42, 45, 47, 47, 48, 47, 42, 47, 41, 45, 47, 47, 47, 45, 41, 43}},
    /* plane 1, x=210 y=0: 0x1.22ba44p-15, with the float product 0x1.22ba42p-15 */
    {{105, 75,  85,  108, 111, 91,  65,  88,  119, 98,  102, 114, 114, 104, 99,  104,
      121, 107, 108, 117, 119, 116, 112, 114, 117, 109, 119, 123, 121, 116, 98,  111,
      111, 125, 126, 114, 118, 109, 97,  114, 135, 136, 125, 115, 119, 117, 112, 116,
      134, 136, 124, 104, 115, 111, 112, 115, 124, 121, 120, 114, 118, 108, 107, 120},
     {101, 72,  81,  104, 109, 91,  72,  91,  118, 96,  108, 114, 113, 106, 100, 113,
      124, 110, 112, 117, 118, 117, 114, 118, 121, 112, 115, 120, 122, 115, 105, 115,
      119, 124, 118, 114, 120, 109, 100, 113, 128, 131, 120, 109, 117, 115, 111, 114,
      132, 137, 126, 106, 112, 110, 113, 116, 125, 128, 124, 116, 118, 110, 110, 115}},
    /* plane 2, x=112 y=0: 0x1.9f4b6p-15, with the float product 0x1.9f4b5ep-15 */
    {{120, 120, 121, 120, 120, 121, 121, 121, 120, 122, 120, 121, 122, 124, 121, 119,
      118, 124, 121, 121, 121, 126, 121, 120, 122, 122, 121, 120, 121, 123, 123, 122,
      121, 121, 122, 121, 122, 124, 124, 123, 122, 123, 122, 121, 121, 122, 122, 123,
      121, 122, 123, 121, 123, 122, 121, 122, 118, 120, 123, 125, 124, 125, 122, 122},
     {120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 121, 121,
      121, 120, 120, 121, 121, 121, 121, 121, 121, 120, 120, 121, 121, 121, 122, 122,
      120, 120, 121, 121, 121, 121, 122, 122, 120, 120, 121, 121, 122, 122, 122, 122,
      120, 120, 120, 121, 123, 122, 122, 122, 120, 121, 121, 123, 123, 122, 122, 122}},
};

static const uint8_t FRAME_33[3][2][64] = {
    /* plane 0, x=140 y=0: 0x1.b36398p-13, with the float product 0x1.b3639cp-13 */
    {{69, 66, 67, 61, 55, 53, 54, 52, 119, 67, 58, 81, 93, 50, 62, 57, 117, 65, 55, 58, 67, 50,
      57, 75, 74, 61, 58, 64, 62, 50, 48,  49, 71, 64, 65, 58, 47, 49, 47,  46, 60, 53, 52, 55,
      64, 63, 56, 48, 56, 48, 45, 56, 62,  53, 49, 51, 62, 66, 53, 77, 61,  58, 67, 62},
     {61, 71, 66, 60, 65, 57, 57, 54, 115, 83, 66, 86, 87, 56, 57, 56, 113, 78, 64, 62, 60, 56,
      55, 65, 75, 69, 60, 56, 55, 55, 53,  52, 76, 61, 57, 53, 54, 56, 53,  49, 68, 59, 52, 51,
      60, 60, 53, 47, 62, 55, 46, 45, 53,  55, 53, 49, 62, 59, 57, 62, 68,  63, 61, 58}},
    /* plane 1, x=0 y=0: 0x1.f02896p-14, with the float product 0x1.f0289ap-14 */
    {{99,  86,  92,  96,  98,  87,  89, 90,  103, 97,  96,  103, 105, 87,  90,  93,
      112, 109, 109, 93,  102, 104, 95, 91,  108, 101, 108, 97,  101, 97,  92,  94,
      111, 107, 111, 103, 101, 92,  95, 104, 112, 111, 104, 97,  102, 89,  91,  110,
      109, 113, 107, 91,  99,  104, 97, 101, 112, 112, 107, 96,  108, 114, 111, 99},
     {101, 93,  92,  97,  96,  89, 90,  95,  100, 98,  98,  101, 101, 89,  93,  93,
      99,  106, 105, 98,  100, 98, 97,  95,  103, 107, 107, 101, 97,  98,  98,  96,
      105, 108, 108, 107, 101, 93, 101, 103, 105, 106, 105, 104, 101, 89,  96,  107,
      106, 107, 107, 100, 100, 98, 98,  104, 111, 112, 113, 103, 104, 107, 107, 106}},
    /* plane 2, x=42 y=0: 0x1.79e7aap-15, with the float product 0x1.79e7aep-15 */
    {{119, 114, 120, 119, 116, 118, 118, 120, 119, 117, 121, 122, 118, 119, 118, 118,
      119, 118, 118, 121, 118, 118, 118, 117, 119, 119, 120, 121, 120, 117, 118, 118,
      121, 120, 119, 121, 120, 118, 118, 118, 122, 117, 120, 123, 119, 118, 116, 117,
      120, 120, 118, 118, 118, 116, 117, 119, 121, 119, 120, 120, 121, 120, 120, 119},
     {119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119,
      119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119,
      119, 119, 119, 119, 119, 119, 119, 119, 120, 120, 120, 120, 120, 120, 120, 120,
      120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120}},
};

static const uint8_t FRAME_0[3][2][64] = {
    /* plane 0, x=49 y=0: 0x1.f63cap-12, with the float product 0x1.f63c9cp-12 */
    {{148, 107, 71,  68,  67, 107, 83,  51, 168, 92, 76, 85, 77, 102, 72, 64,
      100, 100, 160, 153, 97, 103, 101, 70, 59,  66, 84, 83, 95, 124, 97, 62,
      57,  53,  54,  59,  61, 68,  54,  65, 62,  62, 90, 92, 60, 73,  56, 58,
      99,  61,  83,  72,  47, 59,  90,  90, 129, 79, 66, 74, 49, 59,  62, 70},
     {154, 119, 91,  67,  74,  105, 89,  61, 175, 98, 81, 86, 85, 110, 85, 68,
      94,  103, 158, 153, 109, 104, 107, 70, 60,  74, 85, 87, 97, 122, 94, 61,
      61,  58,  59,  57,  61,  67,  60,  62, 62,  67, 92, 89, 59, 65,  62, 64,
      97,  72,  83,  72,  54,  59,  80,  90, 125, 79, 69, 73, 58, 64,  63, 73}},
    /* plane 1, x=7 y=7: 0x1.1b3354p-13, with the float product 0x1.1b3352p-13 */
    {{110, 103, 104, 106, 112, 114, 113, 115, 108, 100, 107, 107, 106, 108, 107, 111,
      103, 103, 104, 98,  91,  95,  106, 109, 95,  101, 99,  109, 100, 106, 111, 106,
      92,  93,  97,  104, 106, 107, 111, 104, 104, 98,  96,  106, 101, 111, 106, 101,
      95,  92,  99,  111, 109, 115, 98,  108, 100, 90,  94,  101, 114, 113, 109, 107},
     {110, 105, 105, 107, 114, 115, 114, 116, 107, 104, 106, 106, 109, 108, 108, 115,
      104, 107, 104, 101, 97,  95,  106, 111, 99,  104, 103, 105, 102, 105, 109, 109,
      95,  98,  99,  103, 107, 111, 114, 111, 106, 102, 98,  103, 106, 112, 108, 106,
      101, 94,  99,  110, 109, 111, 101, 106, 100, 93,  97,  104, 110, 112, 110, 110}},
    /* plane 2, x=28 y=0: 0x1.590d76p-14, with the float product 0x1.590d72p-14 */
    {{119, 119, 115, 117, 119, 127, 119, 116, 116, 117, 122, 118, 118, 125, 121, 116,
      120, 118, 117, 116, 116, 117, 119, 122, 125, 122, 119, 120, 122, 124, 121, 123,
      132, 130, 127, 126, 119, 120, 120, 120, 135, 135, 124, 122, 117, 119, 119, 119,
      126, 121, 116, 119, 116, 119, 119, 121, 120, 116, 117, 118, 118, 120, 119, 120},
     {119, 119, 119, 119, 119, 118, 118, 119, 119, 119, 119, 119, 119, 121, 122, 119,
      119, 119, 119, 119, 119, 119, 119, 119, 123, 123, 122, 121, 121, 120, 120, 120,
      130, 130, 128, 126, 123, 122, 122, 121, 133, 135, 131, 128, 124, 121, 121, 121,
      128, 123, 120, 118, 119, 119, 119, 120, 122, 115, 115, 115, 117, 118, 119, 120}},
};

static const uint8_t (*const FRAME_BLOCKS[])[2][64] = {FRAME_18, FRAME_23, FRAME_33, FRAME_0};

enum Content {
    CONTENT_PATTERN,     /* fx.pattern, as psnr_hvs_twin_parity.h fills it */
    CONTENT_FRAME_BLOCK, /* FRAME_BLOCKS[arg]; an 8x8 4:4:4 picture */
    CONTENT_TEXTURE,     /* detail on a gradient; dist adds an error of a few codes */
    CONTENT_RANDOM,      /* independent uniform samples over the whole range */
    CONTENT_FLAT,        /* constant planes: ref `arg`, dist `arg2`, in 1/1000 of the maximum */
    CONTENT_CHECKER,     /* one-sample checkerboard between 0 and the maximum, dist inverted */
    CONTENT_IMPULSE,     /* texture on both sides; dist differs in one sample per plane */
};

typedef struct {
    const char *name;
    HvsFixture fx; /* format, depth and size; the pattern for CONTENT_PATTERN */
    enum Content content;
    unsigned seeds; /* pictures per case: seed 0 .. seeds - 1 */
    unsigned arg;
    unsigned arg2;
} DispatchCase;

static const DispatchCase CASES[] = {
    /* clang-format off */
    /* One case per line: an expanded table is a brace block longer than the
     * function budget (HISS-04). Designated initializers: an omitted field is
     * zero. */
    {.name = "Netflix pair frame 18", .fx = {VMAF_PIX_FMT_YUV444P, 8u, 8u, 8u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FRAME_BLOCK, .seeds = 1u, .arg = 0u},
    {.name = "Netflix pair frame 23", .fx = {VMAF_PIX_FMT_YUV444P, 8u, 8u, 8u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FRAME_BLOCK, .seeds = 1u, .arg = 1u},
    {.name = "Netflix pair frame 33", .fx = {VMAF_PIX_FMT_YUV444P, 8u, 8u, 8u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FRAME_BLOCK, .seeds = 1u, .arg = 2u},
    {.name = "Netflix pair frame 0", .fx = {VMAF_PIX_FMT_YUV444P, 8u, 8u, 8u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FRAME_BLOCK, .seeds = 1u, .arg = 3u},
    {.name = "8-bit texture, one block per plane", .fx = {VMAF_PIX_FMT_YUV444P, 8u, 8u, 8u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_TEXTURE, .seeds = 48u},
    {.name = "8-bit texture 4:2:0", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 64u, 48u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_TEXTURE, .seeds = 8u},
    {.name = "10-bit texture 4:2:2", .fx = {VMAF_PIX_FMT_YUV422P, 10u, 30u, 22u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_TEXTURE, .seeds = 8u},
    {.name = "12-bit texture 4:4:4", .fx = {VMAF_PIX_FMT_YUV444P, 12u, 15u, 15u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_TEXTURE, .seeds = 8u},
    {.name = "8-bit random, one block per plane", .fx = {VMAF_PIX_FMT_YUV444P, 8u, 8u, 8u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_RANDOM, .seeds = 48u},
    {.name = "8-bit random", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 64u, 48u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_RANDOM, .seeds = 4u},
    {.name = "10-bit random", .fx = {VMAF_PIX_FMT_YUV420P, 10u, 37u, 23u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_RANDOM, .seeds = 4u},
    {.name = "12-bit random", .fx = {VMAF_PIX_FMT_YUV444P, 12u, 16u, 16u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_RANDOM, .seeds = 4u},
    {.name = "8-bit ramp 4:2:0", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 256u, 144u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_PATTERN, .seeds = 1u},
    {.name = "8-bit noise 4:2:0", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 256u, 144u, HVS_PATTERN_NOISE, 0}, .content = CONTENT_PATTERN, .seeds = 1u},
    {.name = "9-bit mixed 4:2:0", .fx = {VMAF_PIX_FMT_YUV420P, 9u, 64u, 48u, HVS_PATTERN_MIXED, 0}, .content = CONTENT_PATTERN, .seeds = 1u},
    {.name = "10-bit noise 4:2:2", .fx = {VMAF_PIX_FMT_YUV422P, 10u, 64u, 48u, HVS_PATTERN_NOISE, 0}, .content = CONTENT_PATTERN, .seeds = 1u},
    {.name = "11-bit mixed 4:4:4", .fx = {VMAF_PIX_FMT_YUV444P, 11u, 64u, 48u, HVS_PATTERN_MIXED, 0}, .content = CONTENT_PATTERN, .seeds = 1u},
    {.name = "12-bit noise 4:0:0", .fx = {VMAF_PIX_FMT_YUV400P, 12u, 64u, 48u, HVS_PATTERN_NOISE, 0}, .content = CONTENT_PATTERN, .seeds = 1u},
    {.name = "identical pictures", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 32u, 24u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FLAT, .seeds = 1u, .arg = 500u, .arg2 = 500u},
    {.name = "all zero", .fx = {VMAF_PIX_FMT_YUV420P, 10u, 32u, 24u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FLAT, .seeds = 1u},
    {.name = "flat, one code apart", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 32u, 24u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FLAT, .seeds = 1u, .arg = 500u, .arg2 = 504u},
    {.name = "8-bit zero against maximum", .fx = {VMAF_PIX_FMT_YUV444P, 8u, 16u, 16u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FLAT, .seeds = 1u, .arg = 0u, .arg2 = 1000u},
    {.name = "12-bit zero against maximum", .fx = {VMAF_PIX_FMT_YUV444P, 12u, 16u, 16u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_FLAT, .seeds = 1u, .arg = 0u, .arg2 = 1000u},
    {.name = "8-bit inverted checkerboard", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 32u, 32u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_CHECKER, .seeds = 1u},
    {.name = "12-bit inverted checkerboard", .fx = {VMAF_PIX_FMT_YUV420P, 12u, 32u, 32u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_CHECKER, .seeds = 1u},
    {.name = "one sample differs", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 32u, 32u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_IMPULSE, .seeds = 4u},
    {.name = "12-bit one sample differs", .fx = {VMAF_PIX_FMT_YUV444P, 12u, 8u, 8u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_IMPULSE, .seeds = 4u},
    {.name = "last block short of the edge", .fx = {VMAF_PIX_FMT_YUV444P, 8u, 21u, 13u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_TEXTURE, .seeds = 4u},
    {.name = "odd size 4:2:0", .fx = {VMAF_PIX_FMT_YUV420P, 8u, 33u, 19u, HVS_PATTERN_RAMP, 0}, .content = CONTENT_TEXTURE, .seeds = 4u},
    /* clang-format on */
};
#define NUM_CASES (sizeof(CASES) / sizeof(CASES[0]))

/* Detail on a slow gradient; the distorted side adds an error of up to +-3
 * codes (scaled to the depth), as a lossy encode does. The detail gives the
 * block a masking threshold and the error sits near it, which is where the
 * rounding of the threshold decides a coefficient. */
static unsigned texture_sample(const DispatchCase *c, unsigned plane, unsigned col, unsigned row,
                               unsigned seed, int distorted)
{
    const unsigned scale = 1u << (c->fx.bpc - 8u);
    const unsigned base = 64u + (col * 3u + row * 2u + seed * 5u) % 96u;
    const unsigned key = plane + 3u * seed;
    const unsigned v = (base + hvs_hash(col, row, key) % 48u) * scale;
    if (!distorted)
        return v;
    return v + hvs_hash(col, row, key + 0x10000u) % (6u * scale + 1u) - 3u * scale;
}

static unsigned sample(const DispatchCase *c, unsigned plane, unsigned col, unsigned row,
                       unsigned seed, int distorted)
{
    const unsigned max = (1u << c->fx.bpc) - 1u;
    switch (c->content) {
    case CONTENT_FRAME_BLOCK:
        return FRAME_BLOCKS[c->arg][plane][distorted][row * 8u + col];
    case CONTENT_RANDOM:
        return hvs_hash(col, row, plane + 3u * seed + (distorted ? 0x20000u : 0u)) & max;
    case CONTENT_FLAT:
        return (distorted ? c->arg2 : c->arg) * max / 1000u;
    case CONTENT_CHECKER:
        return (((col ^ row) & 1u) != 0u) == (distorted != 0) ? max : 0u;
    case CONTENT_IMPULSE: {
        const unsigned v = texture_sample(c, plane, col, row, seed, 0);
        const int hit = distorted && col == 3u + seed && row == 2u + seed;
        return hit ? v ^ (max / 2u + 1u) : v;
    }
    case CONTENT_TEXTURE:
    case CONTENT_PATTERN:
    default:
        return texture_sample(c, plane, col, row, seed, distorted);
    }
}

static void fill_plane(VmafPicture *pic, const DispatchCase *c, unsigned plane, unsigned seed,
                       int distorted)
{
    for (unsigned row = 0; row < pic->h[plane]; row++) {
        uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
        for (unsigned col = 0; col < pic->w[plane]; col++) {
            const unsigned v = sample(c, plane, col, row, seed, distorted);
            if (c->fx.bpc > 8u) {
                ((uint16_t *)line)[col] = (uint16_t)v;
            } else {
                line[col] = (uint8_t)v;
            }
        }
    }
}

static int fill_pic(VmafPicture *pic, const DispatchCase *c, unsigned seed, int distorted)
{
    if (c->content == CONTENT_PATTERN)
        return hvs_fill_pic(pic, &c->fx, distorted ? 1u : 0u);
    const int err = vmaf_picture_alloc(pic, c->fx.fmt, c->fx.bpc, c->fx.w, c->fx.h);
    if (err)
        return err;
    const unsigned planes = (c->fx.fmt == VMAF_PIX_FMT_YUV400P) ? 1u : 3u;
    for (unsigned p = 0; p < planes; p++)
        fill_plane(pic, c, p, seed, distorted);
    return 0;
}

static mu_message_t feed(VmafContext *vmaf, const DispatchCase *c, unsigned seed)
{
    mu_assert("vmaf_use_feature failed", !vmaf_use_feature(vmaf, "psnr_hvs", NULL));
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("ref alloc", !fill_pic(&ref, c, seed, 0));
    if (fill_pic(&dist, c, seed, 1) != 0) {
        (void)vmaf_picture_unref(&ref);
        return "dist alloc";
    }
    /* vmaf_read_pictures() takes both pictures over, on failure too. */
    mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
    return NULL;
}

/* The four scores of one picture pair under `cpumask`. */
static mu_message_t score(const DispatchCase *c, unsigned seed, uint64_t cpumask,
                          double scores[HVS_FEATURES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = cpumask};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    mu_message_t msg = feed(vmaf, c, seed);
    if (!msg)
        msg = hvs_collect(vmaf, &c->fx, scores);
    if (vmaf_close(vmaf) != 0 && !msg)
        msg = "vmaf_close failed";
    return msg;
}

/* Scores one picture pair at both dispatch levels; adds the scores that differ
 * to `*differing` and reports each one. */
static mu_message_t compare(const DispatchCase *c, unsigned seed, unsigned *differing)
{
    double scalar[HVS_FEATURES] = {0.0, 0.0, 0.0, 0.0};
    double host[HVS_FEATURES] = {NAN, NAN, NAN, NAN};
    mu_assert_msg(score(c, seed, CPUMASK_SCALAR, scalar));
    mu_assert_msg(score(c, seed, 0u, host));
    for (unsigned i = 0; i < HVS_FEATURES; i++) {
        if (hvs_score_bits(scalar[i]) == hvs_score_bits(host[i]))
            continue;
        (*differing)++;
        (void)fprintf(stderr, "\n%s, seed %u, %s: scalar=%.17g host=%.17g delta=%.3e", c->name,
                      seed, hvs_features[i], scalar[i], host[i], fabs(scalar[i] - host[i]));
    }
    return NULL;
}

static mu_message_t test_dispatch_invariance(void)
{
    unsigned differing = 0u;
    unsigned pictures = 0u;
    for (unsigned i = 0; i < NUM_CASES; i++) {
        for (unsigned seed = 0; seed < CASES[i].seeds; seed++) {
            mu_assert_msg(compare(&CASES[i], seed, &differing));
            pictures++;
        }
    }
    (void)fprintf(stderr, "%s[%u picture pairs, %u scores differ] ", differing ? "\n" : "",
                  pictures, differing);
    mu_assert("psnr_hvs must return the scalar reference's score bits at every dispatch level",
              differing == 0u);
    return NULL;
}

mu_message_t run_tests(void)
{
    mu_run_test(test_dispatch_invariance);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
