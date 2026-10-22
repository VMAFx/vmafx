/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ciede2000 CPU vs. HIP parity (first added as a places=4 test, ADR-0883; the
 * CPU's arithmetic since ADR-1448).
 *
 * The CIEDE2000 colour difference is ciede.c on the CPU and ciede_hip.c on
 * HIP. Since ADR-1448 the kernel evaluates the reference's statements with
 * every fp64 value as an fp32 pair (feature/ciede_ff_math.h, the arithmetic
 * of the SYCL twin) and the host adds the per-pixel values in the
 * reference's raster order. What still differs: a pixel in a few hundred
 * thousand rounds to the neighbouring float because glibc's powf() is not
 * correctly rounded, and a few in a hundred million because a pair does not
 * decide a rounding the way fp64 does. So this test asserts a tolerance of
 * 1e-8 where it asserted 1e-4.
 *
 * Before ADR-1448 the twin computed in fp32 with another form of the formula
 * and added per wave and per 16x16 block; it was 2e-7 to 1e-5 from the CPU,
 * so the cases below fail on it.
 *
 * The fixtures, the comparison and the cases are ciede_twin_parity.h's. They
 * include what the build used to compile this file twice for: the odd
 * 577x325 4:2:0 frame (ceil chroma width 289, ADR-1213).
 *
 * Skip behaviour: exits 77 when there is no HIP device, and when the kernels
 * are not built (enable_hipcc=false: the extractor returns -ENOSYS).
 */

#include <errno.h>

#include "libvmaf/libvmaf_hip.h"

#include "ciede_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

/* One small frame through the twin. Returns the first error of the run;
 * -ENOSYS is the build without device kernels. */
static int probe_run(VmafHipState *hip_state)
{
    static const CiedeTwinCase probe = {"probe", 64u, 64u, 8u, VMAF_PIX_FMT_YUV420P};
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = vmaf_hip_import_state(vmaf, hip_state);
    if (!err)
        err = vmaf_use_feature(vmaf, "ciede_hip", NULL);
    if (!err)
        err = ciede_twin_fill_picture(&ref, &probe, false);
    if (!err) {
        err = ciede_twin_fill_picture(&dist, &probe, true);
        if (err)
            (void)vmaf_picture_unref(&ref);
    }
    if (!err)
        err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (!err)
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0u);
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* A device state, or non-zero when the cases cannot run: no device, or a
 * build whose extractor has no kernels. Any other failure of the probe is
 * left for the case itself to report. */
static int twin_open(void **state)
{
    VmafHipState *hip_state = NULL;
    const VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err == 0 && hip_state != NULL && probe_run(hip_state) == -ENOSYS) {
        vmaf_hip_state_free(&hip_state);
        err = -ENOSYS;
    }
    *state = hip_state;
    return err;
}

static int twin_import(VmafContext *vmaf, void *state)
{
    return vmaf_hip_import_state(vmaf, (VmafHipState *)state);
}

static int twin_close(void *state)
{
    VmafHipState *hip_state = (VmafHipState *)state;
    vmaf_hip_state_free(&hip_state);
    return 0;
}

static const CiedeTwin twin = {
    .extractor = "ciede_hip",
    .backend = "HIP",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_ciede_hip_registered(void)
{
    return ciede_twin_registered(&twin);
}

static char *test_ciede_8bit(void)
{
    return ciede_twin_bit_depth(&twin, 8u);
}

static char *test_ciede_10bit(void)
{
    return ciede_twin_bit_depth(&twin, 10u);
}

static char *test_ciede_12bit(void)
{
    return ciede_twin_bit_depth(&twin, 12u);
}

static char *test_ciede_16bit(void)
{
    return ciede_twin_bit_depth(&twin, 16u);
}

static char *test_ciede_odd_frame(void)
{
    return ciede_twin_odd_frame(&twin);
}

static char *test_ciede_odd_ceil_chroma(void)
{
    return ciede_twin_odd_ceil_chroma(&twin);
}

static char *test_ciede_422(void)
{
    return ciede_twin_422(&twin);
}

static char *test_ciede_422_10bit_odd(void)
{
    return ciede_twin_422_10bit_odd(&twin);
}

static char *test_ciede_444(void)
{
    return ciede_twin_444(&twin);
}

static char *test_ciede_1080p(void)
{
    return ciede_twin_1080p(&twin);
}

static char *test_ciede_odd_depths(void)
{
    return ciede_twin_odd_depths(&twin);
}

static char *run_bit_depth_cases(void)
{
    mu_run_test(test_ciede_8bit);
    mu_run_test(test_ciede_10bit);
    mu_run_test(test_ciede_12bit);
    mu_run_test(test_ciede_16bit);
    mu_run_test(test_ciede_odd_depths);
    return NULL;
}

static char *run_layout_cases(void)
{
    mu_run_test(test_ciede_odd_frame);
    mu_run_test(test_ciede_odd_ceil_chroma);
    mu_run_test(test_ciede_422);
    mu_run_test(test_ciede_422_10bit_odd);
    return NULL;
}

static char *run_wide_cases(void)
{
    mu_run_test(test_ciede_444);
    mu_run_test(test_ciede_1080p);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ciede_hip_registered);
    mu_assert_msg(run_bit_depth_cases());
    mu_assert_msg(run_layout_cases());
    mu_assert_msg(run_wide_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
