/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

#ifndef LIBVMAF_HIP_SHARED_FRAME_H_
#define LIBVMAF_HIP_SHARED_FRAME_H_

#include <stddef.h>
#include <stdint.h>

#include "libvmaf/picture.h"
#include "picture_hip.h"

/*
 * The frame's planes, uploaded once and read by every HIP twin (ADR-1408).
 *
 * The HIP backend is host-picture only (ADR-0530): every twin used to copy
 * the planes it reads into device buffers of its own, so a run with several
 * twins uploaded the same frame several times, each upload waiting until the
 * copy had read the pageable picture (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).
 * A VmafContext now owns the device copy of the frame its twins read:
 *
 *   - vmaf_read_pictures() announces each frame with
 *     vmaf_hip_shared_frame_begin() before any twin's submit(), and ends it
 *     with vmaf_hip_shared_frame_end() once every twin has submitted.
 *   - A twin asks for the planes it reads with
 *     vmaf_hip_plane_source_acquire(). The first request for a plane uploads
 *     it, with the wait vmaf_hip_picture_upload() has always had; every later
 *     request in the same frame gets the same device pointer and waits for
 *     nothing. An upload takes along the planes the twins asked for in the
 *     frame before, so a frame is usually one upload call and one wait. A
 *     plane no twin asked for in this frame or the one before is not
 *     uploaded.
 *
 * Planes are packed: `width * bytes-per-sample` bytes per row, no padding.
 *
 * Lifetime of a device plane. Frames alternate between two slots. The plane
 * a twin acquired for frame N stays untouched while that twin can still have
 * kernels reading it: libvmaf collects a twin's frame N before it submits
 * its frame N + 1 (dispatch_gpu_double_buffer()), and a twin's hold on a slot
 * moves to the new slot at its next acquire. Slot N % 2 is written again for
 * frame N + 2, by which time every twin that ran frame N + 1 has let go of
 * it. A twin that skipped a frame (`--subsample`) still holds the slot; the
 * upload then waits for the device to go idle first, so nothing is
 * overwritten under a running kernel. A twin that needs a frame's plane for
 * longer than that (the motion twins' previous frame) keeps a copy.
 *
 * The pageable-upload race stays closed: the pictures of a frame are read
 * only between begin() and end(), inside the vmaf_read_pictures() call that
 * owns them, and each upload returns only after the copy has read them.
 *
 * A twin reached without a shared frame (the extractor API used directly),
 * outside begin() / end(), with a picture that is not the announced one, or
 * asking for anything but a whole packed plane uploads into buffers of its
 * own, exactly as before.
 *
 * Not thread-safe: everything here runs on the thread that calls
 * vmaf_read_pictures(), like the twins' submit() and collect().
 */

/* The shared planes of one VmafContext. */
typedef struct VmafHipSharedFrame VmafHipSharedFrame;

/* Planes one twin may read from a frame: Y, U and V of both pictures. */
#define VMAF_HIP_SOURCE_MAX_PLANES 6u

/* A twin's handle on the planes it reads. Zero-initialise it (the extractor's
 * private state is); vmaf_hip_plane_source_close() releases it. */
typedef struct VmafHipPlaneSource {
    /* The shared frame whose slot this twin holds, NULL while it holds
     * none. */
    VmafHipSharedFrame *held_frame;
    unsigned held_slot;
    /* Device buffers of the twin's own, allocated on the first frame that is
     * not served by a shared frame. */
    void *private_dev[VMAF_HIP_SOURCE_MAX_PLANES];
    size_t private_bytes[VMAF_HIP_SOURCE_MAX_PLANES];
} VmafHipPlaneSource;

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate a shared frame. No device memory is claimed before a twin asks
 * for a plane. Returns 0 or -ENOMEM (-ENOSYS without device kernels). */
int vmaf_hip_shared_frame_create(VmafHipSharedFrame **out);

/* Free the planes and the frame, and clear `*frame`. Every twin that
 * acquired from it must have been closed. Safe on NULL. */
void vmaf_hip_shared_frame_destroy(VmafHipSharedFrame **frame);

/*
 * Announce a frame: the next slot becomes current and none of its planes
 * counts as uploaded. `ref` and `dist` must stay valid until
 * vmaf_hip_shared_frame_end(). Returns 0 or a negative errno; a NULL frame
 * is not an error (there is nothing to share).
 */
int vmaf_hip_shared_frame_begin(VmafHipSharedFrame *frame, const VmafPicture *ref,
                                const VmafPicture *dist);

/* The announced pictures stop being readable: a later acquire uploads
 * nothing from them. Device planes already uploaded stay valid. */
void vmaf_hip_shared_frame_end(VmafHipSharedFrame *frame);

/*
 * submit(): the device planes behind `planes[0 .. n_planes)`.
 *
 * Each entry names a host plane (`pic`, `plane`) and its packed geometry
 * (`row_bytes`, `rows`); `dst` and `dst_pitch` are ignored. device[i]
 * receives a packed device plane holding entry i. All entries come from
 * `frame` when it serves them; otherwise all are uploaded into the twin's
 * own buffers. Uploads run on `stream`, the twin's private stream (a
 * hipStream_t carried as uintptr_t, the kernel-template convention), which
 * the twin's collect() has drained. Either way the pictures have been read
 * when this returns.
 *
 * device[] stays valid until the twin's next acquire or close; the twin must
 * not write to it. Returns 0 or a negative errno.
 */
int vmaf_hip_plane_source_acquire(VmafHipPlaneSource *src, VmafHipSharedFrame *frame,
                                  const VmafHipPlaneUpload *planes, unsigned n_planes,
                                  uintptr_t stream, void **device);

/*
 * The request most twins make: the luma of both pictures, packed at
 * `ref->w[0]` samples of one byte (8 bpc) or two (above) per row. `dist` may
 * be NULL for a twin that reads the reference only; `*dis_dev` is then left
 * alone. Same contract as vmaf_hip_plane_source_acquire().
 */
int vmaf_hip_plane_source_acquire_luma(VmafHipPlaneSource *src, VmafHipSharedFrame *frame,
                                       const VmafPicture *ref, const VmafPicture *dist,
                                       uintptr_t stream, void **ref_dev, void **dis_dev);

/* close(): let go of the held slot and free the twin's own buffers. The
 * twin's stream must be drained first. Safe on a zeroed handle. */
void vmaf_hip_plane_source_close(VmafHipPlaneSource *src);

/* Number of planes `frame` has uploaded since it was created; 0 for NULL.
 * For tests. */
uint64_t vmaf_hip_shared_frame_upload_count(const VmafHipSharedFrame *frame);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_HIP_SHARED_FRAME_H_ */
