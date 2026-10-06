/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * 4:2:2 and 4:4:4 imports on a HIP device (RC4 WP3, ADR-2133, ADR-2092): the
 * Netflix and sparks fixtures with their chroma repeated into 4:2:2 and 4:4:4
 * (8, 10, 12 and 16 bits), held by the producer as planar device pointers (odd
 * offsets and pitches), semi-planar NV16 / NV24 / P210 / P410 / P216 / P416,
 * packed YUYV422 / VUYX / Y210 / Y410 / Y212 / XV36 and MSB-aligned planar
 * 4:4:4 words, imported, and scored through every twin declared
 * exact (vmafx_device_cells.h) bit for bit as the same frames uploaded from
 * the host; the host-copy counter stays 0 and every semi-planar and packed
 * import is converted on the device.
 *
 * Needs a HIP device (77 without one) and the fixtures.
 * VMAFX_TEST_FORMAT_GROUP=<8|10|12|16> and VMAFX_TEST_FORMATS=<names> select
 * formats (vmafx_format_cells.h).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_hip_test_util.h"
#include "vmafx_format_cells.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Padding after every producer row (odd: odd pitches) and the bytes the planar
 * planes start past an aligned address. */
#define ROW_PAD 37u
#define SKEW 3u

static VhGpu gpu;
static bool have_gpu;
static unsigned long imports;

static void *hold(void *self, const VmafxFrameDesc *d, const uint8_t *planar, uint32_t pix_fmt,
                  unsigned shift)
{
    (void)self;
    VhPlanes *const p = calloc(1, sizeof(*p));
    const bool planar_layout = !vt_is_semi(pix_fmt) && !vt_is_packed(pix_fmt);
    if (p &&
        vh_upload_skewed(&gpu, d, planar, pix_fmt, shift, ROW_PAD, planar_layout ? SKEW : 0u, p)) {
        return p;
    }
    free(p);
    return NULL;
}

static VmafxFrame *import_frame(void *self, VmafxContext *context, const VmafxFrameDesc *d,
                                uint32_t pix_fmt, void *held)
{
    (void)self;
    (void)context;
    const VmafxFrameImport imp = vh_import_desc(d, pix_fmt, d->bpc, held);
    VmafxFrame *frame = NULL;
    if (vmafx_frame_import(gpu.device, &imp, &frame, NULL) != VMAFX_OK) {
        return NULL;
    }
    imports++;
    return frame;
}

static void release(void *self, void *held)
{
    (void)self;
    vh_free(&gpu, held);
    free(held);
}

static char *test_formats(void)
{
    if (!have_gpu) {
        mu_skipped = 1;
        return NULL;
    }
    const VfOps ops = {
        .device = gpu.device, .hold = hold, .import = import_frame, .release = release};
    VfResult r;
    memset(&r, 0, sizeof(r));
    unsigned formats = 0;
    for (size_t i = 0; i < VF_N_FORMATS; i++) {
        if (!vf_selected(&vf_formats[i])) {
            continue;
        }
        char *const msg = vf_run_format(&ops, &vf_formats[i], &r);
        mu_assert_msg(msg);
        formats++;
    }
    (void)fprintf(
        stderr,
        "[%u formats, %lu cells, %lu values, %lu cells differing in every attempt, %lu "
        "attempts rerun, %lu cells refused for both, %lu imports, %llu conversions, %llu host copies] ",
        formats, r.cells, r.values, r.differing, r.reruns, r.refused, imports,
        (unsigned long long)vmafx_test_conversions(), (unsigned long long)vmafx_test_host_copies());
    mu_assert("formats ran", formats > 0u && r.cells > 0u && r.values > 0u);
    mu_assert("every cell bit-identical", r.differing == 0u);
    mu_assert("no host copy of an imported frame", vmafx_test_host_copies() == 0u);
    return NULL;
}

/* A packed layout in a HIP array is refused naming the memory kind, not
 * read as samples. */
static char *test_packed_arrays_refused(void)
{
    if (!have_gpu) {
        mu_skipped = 1;
        return NULL;
    }
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_DEVICE_ARRAY;
    imp.pix_fmt = VMAFX_PIXEL_FORMAT_Y410;
    imp.bpc = 10u;
    imp.w = 64u;
    imp.h = 64u;
    imp.n_planes = 1u;
    imp.plane[0].handle = 1u;
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, &error);
    mu_assert("refused",
              status == VMAFX_E_NOTSUP && frame == NULL &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.memory", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vh_open(&gpu);
    static const MuTest tests[] = {
        MU_TEST(test_packed_arrays_refused),
        MU_TEST(test_formats),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    vh_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
