/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * Implementation of core VPL decode retry loop and status classification.
 */

#include "vmaf_vpl_core.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr`
 * (C2065), so the fork's C sources spell the null pointer constant `NULL`
 * (ADR-1138). */

VplDecodeAction vpl_classify_decode_status(mfxStatus sts, int have_sync, int passing_null)
{
    if ((sts == MFX_ERR_NONE || sts > 0) && have_sync) {
        return VPL_DECODE_ACTION_FRAME_READY;
    }
    if (sts == MFX_ERR_MORE_DATA) {
        return passing_null ? VPL_DECODE_ACTION_EOF : VPL_DECODE_ACTION_REFILL;
    }
    if (sts == MFX_ERR_MORE_SURFACE || sts == MFX_WRN_DEVICE_BUSY ||
        sts == MFX_WRN_ALLOC_TIMEOUT_EXPIRED) {
        return VPL_DECODE_ACTION_RETRY_BUSY;
    }
    if (sts < 0) {
        return VPL_DECODE_ACTION_ERROR;
    }
    return VPL_DECODE_ACTION_RETRY_WARNING;
}

static void vpl_maybe_refill(void *ctx, const VplDecodeDriver *driver)
{
    if (driver->get_bitstream_length && driver->get_buffer_size && driver->is_eof &&
        driver->refill_bitstream) {
        const size_t len = driver->get_bitstream_length(ctx);
        const size_t cap = driver->get_buffer_size(ctx);
        if (len < cap / 2 && !driver->is_eof(ctx)) {
            (void)driver->refill_bitstream(ctx);
        }
    }
}

int vpl_decode_frame_loop(void *ctx, const VplDecodeDriver *driver, unsigned max_attempts,
                          unsigned retry_us)
{
    if (!driver || !driver->decode_async || !driver->publish_surface) {
        return -1;
    }

    for (unsigned attempt = 0; attempt < max_attempts; attempt++) {
        vpl_maybe_refill(ctx, driver);

        int passing_null = 0;
        if (driver->get_bitstream_length && driver->is_eof) {
            passing_null = (driver->get_bitstream_length(ctx) == 0 && driver->is_eof(ctx));
        }

        mfxFrameSurface1 *out_surf = NULL;
        mfxSyncPoint sync = NULL;
        const mfxStatus sts = driver->decode_async(ctx, passing_null, &out_surf, &sync);
        const VplDecodeAction act = vpl_classify_decode_status(sts, sync != NULL, passing_null);

        switch (act) {
        case VPL_DECODE_ACTION_FRAME_READY:
            return driver->publish_surface(ctx, sync, out_surf);
        case VPL_DECODE_ACTION_EOF:
            return 1;
        case VPL_DECODE_ACTION_REFILL:
            continue;
        case VPL_DECODE_ACTION_RETRY_BUSY:
            if (driver->backoff && retry_us > 0) {
                driver->backoff(ctx, retry_us);
            }
            continue;
        case VPL_DECODE_ACTION_RETRY_WARNING:
            continue;
        case VPL_DECODE_ACTION_ERROR:
        default:
            if (driver->log_error) {
                driver->log_error(ctx, "DecodeFrameAsync failed", (int)sts);
            }
            return -1;
        }
    }

    if (driver->log_exhaustion) {
        driver->log_exhaustion(ctx, max_attempts);
    }
    return -1;
}

/* NOLINTEND(modernize-use-nullptr) */
