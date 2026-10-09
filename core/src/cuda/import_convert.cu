/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Planarisation of imported semi-planar frames on a CUDA device (RC4 WP3,
 * ADR-1929 item 6, ADR-2023). The kernels are shared with the HIP lane
 * (ADR-2092) and live in core/src/vmafx/import_convert_kernels.h.
 */

#include "vmafx/import_convert_kernels.h"

/* The entry points the module lookup finds (import_device.c). */
extern "C" {

__global__ void vmafx_import_deint_8(const uint8_t *src, size_t src_pitch, uint8_t *cb, uint8_t *cr,
                                     size_t dst_pitch, unsigned w, unsigned h)
{
    vmafx_import_deint_8_body(src, src_pitch, cb, cr, dst_pitch, w, h);
}

__global__ void vmafx_import_deint_16(const uint8_t *src, size_t src_pitch, uint8_t *cb,
                                      uint8_t *cr, size_t dst_pitch, unsigned w, unsigned h,
                                      unsigned shift)
{
    vmafx_import_deint_16_body(src, src_pitch, cb, cr, dst_pitch, w, h, shift);
}

__global__ void vmafx_import_shift_16(const uint8_t *src, size_t src_pitch, uint8_t *dst,
                                      size_t dst_pitch, unsigned w, unsigned h, unsigned shift)
{
    vmafx_import_shift_16_body(src, src_pitch, dst, dst_pitch, w, h, shift);
}

} /* extern "C" */
