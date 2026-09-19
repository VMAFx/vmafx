/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Shared multi-frame fixture for the HIP parity tests.
 *
 * Every frame has its own content and its own distortion, so each frame has
 * its own score and a frame scored against another frame's samples shows up
 * as a delta. hip_fixture_feed_frames() feeds N_FRAMES frames from a picture
 * pool sized like the CLI's, so a picture buffer is refilled with the next
 * frame as soon as vmaf_read_pictures() lets go of it. That is what exposes
 * an upload still in flight when a HIP extractor's submit() returns
 * (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18); a single-frame fixture cannot.
 *
 * The including test may define FIXTURE_W / FIXTURE_H / FIXTURE_BPC /
 * N_FRAMES before including this header; meson does so per registration.
 */

#ifndef LIBVMAF_TEST_HIP_POOLED_FIXTURE_H_
#define LIBVMAF_TEST_HIP_POOLED_FIXTURE_H_

#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#ifndef FIXTURE_BPC
#define FIXTURE_BPC 8u
#endif
#ifndef N_FRAMES
#define N_FRAMES 8u
#endif
/* 2 * (threads + 1) + 1 with no worker threads: the CLI's pool size. */
#define POOL_PICTURES 3u

/* Bit-depth generic sample writer: an 8-bit value in the high bits and a
 * second pattern in the low (bpc - 8) bits. */
static inline void hip_fixture_put(VmafPicture *pic, unsigned plane, unsigned row, unsigned col,
                                   unsigned v8, unsigned low_seed)
{
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
#if FIXTURE_BPC > 8u
    const unsigned low_mask = (1u << (FIXTURE_BPC - 8u)) - 1u;
    uint16_t *samples = (uint16_t *)line;
    samples[col] = (uint16_t)(((v8 & 0xFFu) << (FIXTURE_BPC - 8u)) | (low_seed & low_mask));
#else
    (void)low_seed;
    line[col] = (uint8_t)(v8 & 0xFFu);
#endif
}

/* Frame `frame` of the reference (salt 0) or the distorted clip (salt 1)
 * into `pic`. The luma ramp moves every frame and the distortion offset
 * grows with the frame index; chroma is a shallow ramp around mid-grey that
 * moves with them, so the chroma features see each frame too. */
static inline void hip_fixture_fill(VmafPicture *pic, unsigned frame, unsigned salt)
{
    const unsigned offset = frame * 11u + salt * (17u + 3u * frame);
    for (unsigned row = 0u; row < pic->h[0]; row++) {
        for (unsigned col = 0u; col < pic->w[0]; col++)
            hip_fixture_put(pic, 0u, row, col, row + col + offset, row * 7u + col * 3u + salt);
    }
    for (unsigned p = 1u; p < 3u; p++) {
        for (unsigned row = 0u; row < pic->h[p]; row++) {
            for (unsigned col = 0u; col < pic->w[p]; col++) {
                const unsigned ramp = (row + col + offset + p * 5u) & 31u;
                hip_fixture_put(pic, p, row, col, 112u + ramp, row + col + salt);
            }
        }
    }
}

static inline int hip_fixture_fetch(VmafContext *vmaf, VmafPicture *pic, unsigned frame,
                                    unsigned salt)
{
    const int err = vmaf_fetch_preallocated_picture(vmaf, pic);
    if (err)
        return err;
    hip_fixture_fill(pic, frame, salt);
    return 0;
}

/* Preallocate the CLI-sized pool and push N_FRAMES frame pairs through
 * vmaf_read_pictures(). Returns 0, or the error of the first failing call. */
static inline int hip_fixture_feed_frames(VmafContext *vmaf)
{
    const VmafPictureConfiguration pool = {
        .pic_params =
            {
                .w = FIXTURE_W,
                .h = FIXTURE_H,
                .bpc = FIXTURE_BPC,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = POOL_PICTURES,
    };
    int err = vmaf_preallocate_pictures(vmaf, pool);
    for (unsigned f = 0u; f < N_FRAMES && !err; f++) {
        VmafPicture ref;
        VmafPicture dist;
        err = hip_fixture_fetch(vmaf, &ref, f, 0u);
        if (err)
            break;
        err = hip_fixture_fetch(vmaf, &dist, f, 1u);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            break;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, f);
    }
    return err;
}

#endif /* LIBVMAF_TEST_HIP_POOLED_FIXTURE_H_ */
