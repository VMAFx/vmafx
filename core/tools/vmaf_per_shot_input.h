/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_TOOLS_VMAF_PER_SHOT_INPUT_H
#define VMAF_TOOLS_VMAF_PER_SHOT_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum VmafPerShotReadResult {
    VMAF_PER_SHOT_READ_ERROR = -2,
    VMAF_PER_SHOT_READ_PARTIAL = -1,
    VMAF_PER_SHOT_READ_EOF = 0,
    VMAF_PER_SHOT_READ_FRAME = 1,
};

int vmaf_per_shot_read_luma(FILE *fin, uint8_t *luma, size_t luma_bytes, size_t chroma_bytes);

#endif /* VMAF_TOOLS_VMAF_PER_SHOT_INPUT_H */
