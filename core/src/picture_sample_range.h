/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The opt-in sample range check of vmaf_read_pictures()
 * (vmaf_set_sample_range_check_enabled(), `vmaf --check-sample-range`;
 * ADR-1918). A picture of bit depth bpc must hold samples of at most
 * 2^bpc - 1; nothing on the default path checks it, and with samples above
 * it the CPU extractors and their GPU twins may give different scores
 * (T-OUT-OF-RANGE-SAMPLES-TWIN-DIVERGENCE-2026-10-05).
 */

#ifndef VMAF_SRC_PICTURE_SAMPLE_RANGE_H_
#define VMAF_SRC_PICTURE_SAMPLE_RANGE_H_

#include "libvmaf/picture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 0 when every sample of every plane of `pic` is at most 2^bpc - 1.
 * Otherwise -EINVAL after logging `which` ("reference" or "distorted"), the
 * plane, row and column of the first such sample in raster order, its value
 * and the limit. Pictures of 8 and 16 bits cannot hold such a sample and
 * return 0 at once. A picture in device memory returns -ENOTSUP after logging:
 * the host cannot read it.
 */
int vmaf_picture_check_sample_range(const VmafPicture *pic, const char *which);

/**
 * Non-zero when the host can read the samples of `pic`: a host or pinned-host
 * picture, or one with no private slot. Zero for a picture in device memory.
 */
int vmaf_picture_host_readable(const VmafPicture *pic);

#ifdef __cplusplus
}
#endif

#endif /* VMAF_SRC_PICTURE_SAMPLE_RANGE_H_ */
