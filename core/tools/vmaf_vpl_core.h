/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * Core decode retry loop and status classification for vmaf_vpl.
 * Kept independent of libvmaf link surface and GPU device handles
 * so the retry ceiling and frame-ordering contract can be tested
 * deterministically without hardware (ADR-1900).
 */

#ifndef LIBVMAF_TOOLS_VMAF_VPL_CORE_H_
#define LIBVMAF_TOOLS_VMAF_VPL_CORE_H_

#include <stddef.h>
#include <stdint.h>
#include <vpl/mfx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wall-clock ceiling handed to MFXVideoCORE_SyncOperation, in milliseconds. */
#ifndef VPL_SYNC_TIMEOUT_MS
#define VPL_SYNC_TIMEOUT_MS 60000U
#endif

/* Back-off between DecodeFrameAsync retries when the device reports busy or
 * asks for another surface, in microseconds. */
#ifndef VPL_DECODE_RETRY_US
#define VPL_DECODE_RETRY_US 1000U
#endif

/* Retry ceiling for one vpl_decode_frame() call: VPL_SYNC_TIMEOUT_MS of
 * VPL_DECODE_RETRY_US back-offs. ADR-1287 / ADR-1900. */
#ifndef VPL_DECODE_MAX_ATTEMPTS
#define VPL_DECODE_MAX_ATTEMPTS ((VPL_SYNC_TIMEOUT_MS * 1000u) / VPL_DECODE_RETRY_US)
#endif

typedef enum {
    VPL_DECODE_ACTION_FRAME_READY = 0,
    VPL_DECODE_ACTION_EOF = 1,
    VPL_DECODE_ACTION_REFILL = 2,
    VPL_DECODE_ACTION_RETRY_BUSY = 3,
    VPL_DECODE_ACTION_RETRY_WARNING = 4,
    VPL_DECODE_ACTION_ERROR = -1,
} VplDecodeAction;

/**
 * Classify a DecodeFrameAsync status code into the loop's next action.
 *
 * @param sts Return code from MFXVideoDECODE_DecodeFrameAsync.
 * @param have_sync Non-zero if DecodeFrameAsync yielded a valid sync point.
 * @param passing_null Non-zero if bitstream buffer is empty at EOF (drain phase).
 * @return The categorized decode action to execute.
 */
VplDecodeAction vpl_classify_decode_status(mfxStatus sts, int have_sync, int passing_null);

/**
 * Pluggable decode driver interface for device-free testing and hardware execution.
 */
typedef struct VplDecodeDriver {
    size_t (*get_bitstream_length)(void *ctx);
    size_t (*get_buffer_size)(void *ctx);
    int (*is_eof)(void *ctx);
    int (*refill_bitstream)(void *ctx);
    mfxStatus (*decode_async)(void *ctx, int passing_null, mfxFrameSurface1 **out_surf,
                              mfxSyncPoint *out_sync);
    int (*publish_surface)(void *ctx, mfxSyncPoint sync, mfxFrameSurface1 *out_surf);
    void (*backoff)(void *ctx, unsigned usec);
    void (*log_error)(void *ctx, const char *msg, int code);
    void (*log_exhaustion)(void *ctx, unsigned attempts);
} VplDecodeDriver;

/**
 * Execute the decode retry loop up to max_attempts attempts.
 *
 * @param ctx Driver context passed to all callbacks.
 * @param driver Function table implementing decode operations.
 * @param max_attempts Maximum attempts allowed before reporting exhaustion.
 * @param retry_us Microseconds to back off on busy/retryable conditions.
 * @return 0 on frame published, 1 on EOF, -1 on error or ceiling exhaustion.
 */
int vpl_decode_frame_loop(void *ctx, const VplDecodeDriver *driver, unsigned max_attempts,
                          unsigned retry_us);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_TOOLS_VMAF_VPL_CORE_H_ */
