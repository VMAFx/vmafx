/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#include "vmaf_per_shot_input.h"

#include <stdbool.h>

/* Fill one plane/chunk. EOF is only valid before the first luma byte of a new
 * frame; a short plane is corrupt input, while ferror is a distinct I/O fault. */
static int read_exact(FILE *fin, uint8_t *dst, size_t bytes, bool allow_initial_eof)
{
    size_t consumed = 0U;
    while (consumed < bytes) {
        const size_t read_count = fread(dst + consumed, 1U, bytes - consumed, fin);
        if (read_count > 0U) {
            consumed += read_count;
            continue;
        }
        if (ferror(fin) != 0)
            return VMAF_PER_SHOT_READ_ERROR;
        if (feof(fin) != 0) {
            return allow_initial_eof && consumed == 0U ? VMAF_PER_SHOT_READ_EOF :
                                                         VMAF_PER_SHOT_READ_PARTIAL;
        }
        return VMAF_PER_SHOT_READ_ERROR;
    }
    return VMAF_PER_SHOT_READ_FRAME;
}

/* Raw planar input has no container metadata, so a frame exists only after
 * every luma and chroma byte has been consumed. Seeking over regular files is
 * deliberately avoided: stdio permits seeking beyond EOF, which would accept
 * a luma-only tail as a complete frame. */
int vmaf_per_shot_read_luma(FILE *fin, uint8_t *luma, size_t luma_bytes, size_t chroma_bytes)
{
    int rc = read_exact(fin, luma, luma_bytes, true);
    if (rc != VMAF_PER_SHOT_READ_FRAME)
        return rc;

    size_t remaining = chroma_bytes;
    uint8_t scratch[4096];
    while (remaining > 0U) {
        const size_t want = remaining > sizeof(scratch) ? sizeof(scratch) : remaining;
        rc = read_exact(fin, scratch, want, false);
        if (rc != VMAF_PER_SHOT_READ_FRAME)
            return rc;
        remaining -= want;
    }
    return VMAF_PER_SHOT_READ_FRAME;
}
