/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Host input in every layout of the format table scores bit for bit as the
 * planar frame (RC4 WP13, ADR-2145): the Netflix and sparks fixtures with
 * their chroma repeated into 4:2:2 and 4:4:4 (8, 10, 12 and 16 bits), held in
 * host memory as planar, semi-planar NVxx / Pxxx, packed (YUYV422, UYVY422,
 * AYUV, V210, Y210, Y212, Y410, XV36, VUYX), MSB-aligned planar words, or as
 * RGB / RGBA / BGRA with a stated matrix and ranges, imported by the CPU
 * device and scored through the cells of vmafx_device_cells.h. The same
 * runner (vmafx_format_cells.h) runs the CUDA, HIP and SYCL lanes; here it is
 * the CPU's, so the host input is held to the planar frame and the device
 * lanes to the host.
 *
 * Needs the fixtures (77 without them).
 * VMAFX_TEST_FORMAT_GROUP=<8|10|12|16> and VMAFX_TEST_FORMATS=<names> select
 * formats.
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
#include "vmafx_format_cells.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

/* The producer's frame in host memory: its bytes (own, or the planar clip's). */
typedef struct Held {
    uint8_t *staged;
    const uint8_t *bytes;
} Held;

static unsigned long imports;
static VmafxDevice *cpu_device;

static void *hold(void *self, const VmafxFrameDesc *d, const uint8_t *planar, uint32_t pix_fmt,
                  unsigned shift)
{
    (void)self;
    Held *const h = calloc(1, sizeof(*h));
    if (h) {
        h->bytes = vt_producer_bytes(d, planar, pix_fmt, shift, &h->staged);
    }
    if (h && !h->bytes) {
        free(h);
        return NULL;
    }
    return h;
}

static VmafxFrameImport describe(const VmafxFrameDesc *d, uint32_t pix_fmt, const uint8_t *data)
{
    VmafxFrameImport imp;
    if (vt_is_packed(pix_fmt)) {
        imp = vt_import_packed(d, pix_fmt, data);
    } else if (vt_is_msb(pix_fmt)) {
        imp = vt_import_planar_words(d, pix_fmt, data);
    } else if (vt_is_semi(pix_fmt)) {
        imp = vt_import_semiplanar(d, pix_fmt, d->bpc, data);
    } else {
        VmafxFrameDesc planar = *d;
        planar.pix_fmt = pix_fmt;
        imp = vt_import_planar(&planar, data);
    }
    vt_apply_rgb_statement(&imp);
    return imp;
}

static VmafxFrame *import_frame(void *self, VmafxContext *context, const VmafxFrameDesc *d,
                                uint32_t pix_fmt, void *held)
{
    (void)self;
    (void)context;
    const VmafxFrameImport imp = describe(d, pix_fmt, ((const Held *)held)->bytes);
    VmafxFrame *frame = NULL;
    if (vmafx_frame_import(cpu_device, &imp, &frame, NULL) != VMAFX_OK) {
        return NULL;
    }
    imports++;
    return frame;
}

static void release(void *self, void *held)
{
    (void)self;
    free(((Held *)held)->staged);
    free(held);
}

static char *test_formats(void)
{
    const VfOps ops = {
        .device = cpu_device, .hold = hold, .import = import_frame, .release = release};
    VfResult r;
    memset(&r, 0, sizeof(r));
    unsigned formats = 0;
    for (size_t i = 0; i < VF_N_FORMATS; i++) {
        if (!vf_selected(&vf_formats[i])) {
            continue;
        }
        VtClip probe;
        if (!vf_clip(&vf_formats[i], &probe)) {
            vt_clip_close(&probe);
            mu_skipped = 1; /* the fixtures are not here */
            return NULL;
        }
        vt_clip_close(&probe);
        char *const msg = vf_run_format(&ops, &vf_formats[i], &r);
        mu_assert_msg(msg);
        formats++;
    }
    (void)fprintf(stderr,
                  "[%u formats, %lu cells, %lu values, %lu cells differing in every attempt, %lu "
                  "attempts rerun, %lu cells refused for both, %lu imports] ",
                  formats, r.cells, r.values, r.differing, r.reruns, r.refused, imports);
    mu_assert("formats ran", formats > 0u && r.cells > 0u && r.values > 0u);
    mu_assert("every cell bit-identical", r.differing == 0u);
    return NULL;
}

char *run_tests(void)
{
    const VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    if (vmafx_device_create(&desc, &cpu_device, NULL) != VMAFX_OK) {
        return "the CPU device";
    }
    static const MuTest tests[] = {MU_TEST(test_formats)};
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    vmafx_device_unref(cpu_device);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
