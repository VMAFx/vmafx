/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
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
 * C++23 implementation of the picture-copy pixel-normalisation helpers.
 *
 * Migration note (ADR-0729 / Wave 3):
 *   - `git mv picture_copy.c picture_copy.cpp` preserves blame.
 *   - The public C API in picture_copy.h is unchanged; both functions retain
 *     their original C signatures and are exposed to C callers via the
 *     `extern "C"` guard added to picture_copy.h.
 *   - `std::span` replaces raw pointer arithmetic on src->data[channel],
 *     making per-row bounds explicit and enabling bounds-checked iteration.
 *   - C-style casts replaced with `static_cast<>` throughout.
 *   - No behaviour change vs the original C implementation.
 */

#include <cstddef>
#include <cstdint>
#include <span>

#include <libvmaf/picture.h>

#include "picture_copy.h"

/* Normalise a high-bit-depth (16-bit storage) plane into float.
 *
 * dst        — destination float buffer
 * dst_stride — row stride of dst in bytes
 * src        — source VmafPicture
 * offset     — integer offset added to each sample after division
 * scaler     — divisor applied to each 16-bit sample before offset
 * channel    — 0=Y, 1=Cb, 2=Cr */
static void picture_copy_hbd(float *dst, std::ptrdiff_t dst_stride, VmafPicture *src, int offset,
                             float scaler, int channel)
{
    const unsigned h = src->h[channel];
    const unsigned w = src->w[channel];
    const auto *src_row = static_cast<const uint16_t *>(src->data[channel]);
    const std::ptrdiff_t src_stride_samples = static_cast<std::ptrdiff_t>(src->stride[channel]) / 2;

    for (unsigned i = 0; i < h; i++) {
        const std::span<const uint16_t> row_in(src_row, w);
        for (unsigned j = 0; j < w; j++) {
            dst[j] = static_cast<float>(row_in[j]) / scaler + static_cast<float>(offset);
        }
        /* dst_stride is signed ptrdiff_t; negative values advance dst backwards
         * (bottom-up image layout).  This is an intentional, supported use case
         * preserved from the original C code.  Arithmetic on signed ptrdiff_t is
         * well-defined; negative strides are valid (CERT INT31-C note;
         * adversarial review 2026-05-28 finding #14). */
        dst += dst_stride / static_cast<std::ptrdiff_t>(sizeof(float));
        src_row += src_stride_samples;
    }
}

void picture_copy(float *dst, ptrdiff_t dst_stride, VmafPicture *src, int offset, unsigned bpc,
                  int channel)
{
    if (bpc == 10U) {
        picture_copy_hbd(dst, dst_stride, src, offset, 4.0f, channel);
        return;
    }
    if (bpc == 12U) {
        picture_copy_hbd(dst, dst_stride, src, offset, 16.0f, channel);
        return;
    }
    if (bpc == 16U) {
        picture_copy_hbd(dst, dst_stride, src, offset, 256.0f, channel);
        return;
    }

    const unsigned h = src->h[channel];
    const unsigned w = src->w[channel];
    const auto *src_row = static_cast<const uint8_t *>(src->data[channel]);
    const std::ptrdiff_t src_stride_bytes = static_cast<std::ptrdiff_t>(src->stride[channel]);

    for (unsigned i = 0; i < h; i++) {
        const std::span<const uint8_t> row_in(src_row, w);
        for (unsigned j = 0; j < w; j++) {
            dst[j] = static_cast<float>(row_in[j]) + static_cast<float>(offset);
        }
        dst += dst_stride / static_cast<std::ptrdiff_t>(sizeof(float));
        src_row += src_stride_bytes;
    }
}
