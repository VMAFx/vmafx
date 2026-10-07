/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 *
 * test_rust_cambi_kernels.c — the init-time stages of the Rust `cambi` twin
 * (core/src/rust/feature/cambi) against the C functions of cambi.c, value by
 * value: the reciprocal table, the TVI / visibility tables for both EOTFs,
 * the adjusted window, the spatial-mask index and the resize walk. The
 * per-frame stages are covered by the differential harness
 * (scripts/ci/rust_twin_diff.py); these stages decide integers the fixtures
 * reach only for a few option values. ADR-1713.
 */

#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "feature/cambi_internal.h"
#include "log.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Test hooks exported by the vmafx-fex-cambi crate (src/testhook.rs). */
float vmafx_rs_testhook_cambi_reciprocal(uint32_t i);
uint32_t vmafx_rs_testhook_cambi_reciprocal_size(void);
int32_t vmafx_rs_testhook_cambi_tables(uint32_t max_log_contrast, double tvi_threshold,
                                       double vis_lum_threshold, uint32_t pq, uint32_t slot);
uint32_t vmafx_rs_testhook_cambi_window(uint32_t window, uint32_t w, uint32_t h, uint32_t speedup);
uint32_t vmafx_rs_testhook_cambi_mask_index(uint32_t w, uint32_t h);
uint32_t vmafx_rs_testhook_cambi_resize_index(uint32_t in_len, uint32_t out_len, uint32_t i);

#define VLT_SLOT 32U
#define BASE_SLOT 33U
#define SIZE_SLOT 34U

static const unsigned kDims[] = {1U,   63U,   64U,   127U,  180U,  216U,  270U,  324U,  480U, 576U,
                                 720U, 1080U, 1440U, 1920U, 2160U, 2560U, 3840U, 4320U, 7680U};
#define N_DIMS (sizeof(kDims) / sizeof(kDims[0]))

static mu_message_t test_reciprocal_table(void)
{
    unsigned size = 0;
    const float *lut = vmaf_cambi_reciprocal_lut(&size);
    mu_assert("reciprocal table size differs", vmafx_rs_testhook_cambi_reciprocal_size() == size);
    for (unsigned i = 0; i < size; i++) {
        const float rust = vmafx_rs_testhook_cambi_reciprocal(i);
        mu_assert("reciprocal table entry differs", memcmp(&rust, &lut[i], sizeof(float)) == 0);
    }
    return NULL;
}

/* One configuration of vmaf_cambi_init_tvi_and_vlt() against the Rust tables. */
static mu_message_t check_tables(unsigned mlc, double tvi, double vlt, unsigned pq)
{
    const int num_diffs = 1 << mlc;
    uint16_t diffs[32];
    uint16_t tvi_for_diff[32] = {0};
    uint16_t vlt_luma = 0;
    uint16_t base = 0;
    uint16_t size = 0;
    for (int d = 0; d < num_diffs; d++)
        diffs[d] = (uint16_t)(d + 1);
    const int err = vmaf_cambi_init_tvi_and_vlt(num_diffs, diffs, tvi, vlt, pq ? "pq" : "bt1886",
                                                "bt1886", tvi_for_diff, &vlt_luma, &base, &size);
    if (err) {
        mu_assert("Rust accepts tables the C refuses",
                  vmafx_rs_testhook_cambi_tables(mlc, tvi, vlt, pq, VLT_SLOT) == -1);
        return NULL;
    }
    for (int d = 0; d < num_diffs; d++) {
        mu_assert("tvi_for_diff differs",
                  vmafx_rs_testhook_cambi_tables(mlc, tvi, vlt, pq, (uint32_t)d) ==
                      (int32_t)tvi_for_diff[d]);
    }
    mu_assert("vlt_luma differs",
              vmafx_rs_testhook_cambi_tables(mlc, tvi, vlt, pq, VLT_SLOT) == (int32_t)vlt_luma);
    mu_assert("v_band_base differs",
              vmafx_rs_testhook_cambi_tables(mlc, tvi, vlt, pq, BASE_SLOT) == (int32_t)base);
    mu_assert("v_band_size differs",
              vmafx_rs_testhook_cambi_tables(mlc, tvi, vlt, pq, SIZE_SLOT) == (int32_t)size);
    return NULL;
}

static mu_message_t test_contrast_tables(void)
{
    static const double tvis[] = {0.0001, 0.005, 0.019, 0.05, 0.1, 0.4, 1.0};
    static const double vlts[] = {0.0, 0.06, 1.0, 20.0, 300.0};
    /* The C logs every configuration it refuses (an empty value band); the
     * test expects those refusals, so it silences the logger here. */
    vmaf_set_log_level(VMAF_LOG_LEVEL_NONE);
    for (unsigned pq = 0; pq < 2; pq++) {
        for (unsigned mlc = 0; mlc <= 5; mlc++) {
            for (size_t t = 0; t < sizeof(tvis) / sizeof(tvis[0]); t++) {
                for (size_t v = 0; v < sizeof(vlts) / sizeof(vlts[0]); v++)
                    mu_assert_msg(check_tables(mlc, tvis[t], vlts[v], pq));
            }
        }
    }
    return NULL;
}

static mu_message_t test_window_and_mask_index(void)
{
    static const int windows[] = {15, 16, 33, 64, 65, 100, 127};
    for (size_t a = 0; a < N_DIMS; a++) {
        for (size_t b = 0; b < N_DIMS; b++) {
            const unsigned w = kDims[a];
            const unsigned h = kDims[b];
            mu_assert("mask index differs",
                      vmafx_rs_testhook_cambi_mask_index(w, h) == vmaf_cambi_mask_index(w, h));
            for (size_t k = 0; k < sizeof(windows) / sizeof(windows[0]); k++) {
                for (unsigned speedup = 0; speedup < 2; speedup++) {
                    const uint16_t c = vmaf_cambi_adjust_window(windows[k], w, h, speedup != 0);
                    mu_assert("adjusted window differs",
                              vmafx_rs_testhook_cambi_window((uint32_t)windows[k], w, h, speedup) ==
                                  c);
                }
            }
        }
    }
    return NULL;
}

static mu_message_t test_resize_walk(void)
{
    static const unsigned pairs[][2] = {
        {576U, 320U}, {324U, 180U}, {1920U, 960U},  {1080U, 540U}, {3840U, 1920U}, {2160U, 1080U},
        {480U, 960U}, {270U, 540U}, {1920U, 1280U}, {1080U, 720U}, {576U, 7680U},  {540U, 216U}};
    static uint32_t indices[7680];
    for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
        const unsigned in_len = pairs[p][0];
        const unsigned out_len = pairs[p][1];
        vmaf_cambi_resize_source_indices(in_len, out_len, indices);
        for (unsigned i = 0; i < out_len; i++) {
            mu_assert("resize source index differs",
                      vmafx_rs_testhook_cambi_resize_index(in_len, out_len, i) == indices[i]);
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_reciprocal_table);
    mu_run_test(test_contrast_tables);
    mu_run_test(test_window_and_mask_index);
    mu_run_test(test_resize_walk);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
