/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The SYCL runtime half of the VMAFx SYCL lane (RC4 WP3, ADR-2091): every
 * call into SYCL or Level Zero the lane makes, behind a C ABI that the lane's
 * C files (import_*.c) call. Nothing here is exported.
 *
 * A device is one library queue (in-order, immediate command lists where
 * the implementation offers them, ADR-2091 item 1) on a SYCL context and
 * device. A frame of the device has a ready point on that queue (behind its
 * acquire fence and its conversions) and a list of the reads the engine made
 * of its planes (vmaf_sycl_picture_read_plane(), every reader on whatever
 * queue it runs); its release is one barrier over those reads on the library
 * queue, so the release fences follow the last reader of every context.
 */

#ifndef VMAF_SRC_SYCL_VMAFX_SYCL_RT_H_
#define VMAF_SRC_SYCL_VMAFX_SYCL_RT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf_sycl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using,performance-enum-size): C header included
 * by C and C++ translation units; `using` and fixed enum types are not C.
 * ADR-1138. */

typedef struct VmafxSyclRt VmafxSyclRt;
typedef struct VmafxSyclFrameRt VmafxSyclFrameRt;

/* What a USM pointer is in the device's context. */
enum VmafxSyclPointerKind {
    VMAFX_SYCL_POINTER_UNKNOWN = 0, /* not a USM allocation of the context */
    VMAFX_SYCL_POINTER_HOST = 1,
    VMAFX_SYCL_POINTER_DEVICE = 2,
    VMAFX_SYCL_POINTER_SHARED = 3,
};

/* Memory layouts the de-tiling kernel reads (DRM format modifiers of the
 * Intel kernel drivers; the 128 x 32 byte Y tile and the Tile4 of Xe-HPG). */
enum VmafxSyclTiling {
    VMAFX_SYCL_TILING_Y = 1,
    VMAFX_SYCL_TILING_4 = 2,
};

/* One 2-D operation on device memory: `rows` rows of `w` samples of
 * `bytes` bytes (1 or 2) from `src` (`src_pitch` bytes apart) into `dst0`
 * (and, for a de-interleave, Cr into `dst1`), `dst_pitch` bytes apart. A
 * 16-bit sample is little-endian and shifted right by `shift`. */
typedef struct VmafxSyclPlaneOp {
    const void *src;
    size_t src_pitch;
    void *dst0;
    void *dst1;
    size_t dst_pitch;
    unsigned w;
    unsigned rows;
    unsigned bytes;
    unsigned shift;
    /* A gather (vmafx_sycl_rt_frame_gather()): the plan of
     * vmafx_import_plane_read(); sample x of a row is the element
     * x * step + offset of `in_bytes` bytes, shifted right by `shift` and
     * masked with `mask` (0: none), stored as `bytes` bytes. Zero otherwise. */
    unsigned step;
    unsigned offset;
    unsigned in_bytes;
    unsigned mask;
} VmafxSyclPlaneOp;

/* Where a frame's release is reported. `signal(arg)` runs on a runtime
 * thread once the release barrier completed (a HOST release fence), or not
 * at all for NULL. `slot` is 1 + the release-event slot the barrier's event
 * is stored in, or 0; `idle_slot` likewise. `owned` (device USM) and `imports` (Level Zero
 * dma-buf imports) are freed once the barrier completed. */
typedef struct VmafxSyclRelease {
    void (*signal)(void *arg);
    void *arg;
    uint32_t slot;
    uint32_t idle_slot; /* a pool frame's: 1 + the slot its next acquire waits on, or 0 */
    void *owned;
    void *imports[3];
} VmafxSyclRelease;

/* ---- Devices ------------------------------------------------------------------ */

/* GPUs of every SYCL platform, in the order vmaf_sycl_state_init() indexes
 * them (VmafSyclConfiguration.device_index): 0, or -EIO when the runtime
 * throws. */
int vmafx_sycl_rt_count(uint32_t *count);
/* Name (kept for the process) and global memory of GPU `index`: 0, -ENOENT
 * past the last one, -EIO. */
int vmafx_sycl_rt_info(int32_t index, const char **name, uint64_t *memory);
/* Room for a PCI bus id ("dddd:bb:dd.f") and its terminator. */
#define VMAFX_SYCL_BUS_ID_SIZE 32u
/* The PCI bus id of GPU `index` ("" when the runtime reports none): 0,
 * -ENOENT, -EIO. RC4 WP3 Vulkan lane. */
int vmafx_sycl_rt_bus_id(int32_t index, char bus[VMAFX_SYCL_BUS_ID_SIZE]);
/* The library queue of GPU `index`, or on the context and device of the
 * caller's queue (`external_queue`, a sycl::queue *, when not 0): 0,
 * -ENOENT, -ENOMEM, -EIO. */
int vmafx_sycl_rt_open(int32_t index, uintptr_t external_queue, VmafxSyclRt **out);
/* Drain the library queue, free what releases deferred, free the device. */
void vmafx_sycl_rt_close(VmafxSyclRt *rt);
int32_t vmafx_sycl_rt_index(const VmafxSyclRt *rt);
const char *vmafx_sycl_rt_name(const VmafxSyclRt *rt);
uint64_t vmafx_sycl_rt_memory(const VmafxSyclRt *rt);
/* The PCI bus id of the library queue's device ("" when unknown). */
void vmafx_sycl_rt_device_bus_id(const VmafxSyclRt *rt, char bus[VMAFX_SYCL_BUS_ID_SIZE]);
/* Whether the library queue runs on immediate command lists. */
bool vmafx_sycl_rt_immediate(const VmafxSyclRt *rt);
/* Test switch (white-box): library queues opened from now on leave out the
 * immediate-command-list property (the evaluation of draft PR #2217, ADR-2091). */
void vmafx_sycl_rt_test_omit_immediate(bool omit);
/* The library queue (a sycl::queue *). */
void *vmafx_sycl_rt_queue(VmafxSyclRt *rt);
/* A new engine state on the device's context and device: 0 or a negative
 * errno. Freed with vmaf_sycl_state_free(). */
int vmafx_sycl_rt_engine_state(VmafxSyclRt *rt, VmafSyclState **out);

/* ---- Memory ------------------------------------------------------------------- */

enum VmafxSyclPointerKind vmafx_sycl_rt_pointer_kind(VmafxSyclRt *rt, const void *ptr);
/* Device USM of `bytes` bytes, or NULL. */
void *vmafx_sycl_rt_alloc(VmafxSyclRt *rt, size_t bytes);
/* Free device USM now (nothing reads it any more). */
void vmafx_sycl_rt_free(VmafxSyclRt *rt, void *ptr);
/* Import `size` bytes of the dma-buf `fd` as device memory (Level Zero
 * external memory; the caller keeps `fd`): 0, -EIO. */
int vmafx_sycl_rt_dmabuf_import(VmafxSyclRt *rt, int fd, size_t size, void **ptr);
/* Free deferred memory whose release completed (bounded; called per import). */
void vmafx_sycl_rt_collect(VmafxSyclRt *rt);

/* ---- Frames ------------------------------------------------------------------- */

VmafxSyclFrameRt *vmafx_sycl_rt_frame_new(VmafxSyclRt *rt);
/* The frame's conversions and readers wait on the producer's event
 * (`event`, a sycl::event * of the device's context): one barrier on the
 * library queue. 0 or -EIO. */
int vmafx_sycl_rt_frame_after_event(VmafxSyclFrameRt *f, uintptr_t event);
/* Conversions on the library queue: 0 or -EIO. */
int vmafx_sycl_rt_frame_deinterleave(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op);
int vmafx_sycl_rt_frame_shift(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op);
/* One plane of a packed layout or of MSB planar words (ADR-2133). */
int vmafx_sycl_rt_frame_gather(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op);
/* De-tile a plane of `op->rows` rows of `op->w` samples of `op->bytes` bytes
 * (an interleaved chroma plane: 2 * width samples of one byte, or of two)
 * whose tiled pitch is `op->src_pitch` (a multiple of 128) into linear rows
 * at `op->dst0`; 16-bit samples are shifted right by `op->shift`. */
int vmafx_sycl_rt_frame_detile(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op,
                               enum VmafxSyclTiling tiling);
/* The planted host copy (VMAFX_TEST_FORCE_HOST_COPY): `op` staged through
 * host memory, synchronously. 0, -ENOMEM, -EIO. */
int vmafx_sycl_rt_frame_stage_host(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op);
/* Everything enqueued for the frame so far is its ready point: readers
 * wait on it. 0 or -EIO. */
int vmafx_sycl_rt_frame_ready(VmafxSyclFrameRt *f);
/* The frame's release: one barrier on the library queue over every read of
 * its planes; `r` says what follows it. Frees `f`. 0 or -EIO (the frame's
 * memory is then freed after a queue drain). */
int vmafx_sycl_rt_frame_release(VmafxSyclFrameRt *f, const VmafxSyclRelease *r);
/* An import that failed after `f` was made (or a release that could not be
 * enqueued): drain the library queue, free `owned` / `imports`, run
 * `signal`, free `f`. */
void vmafx_sycl_rt_frame_discard(VmafxSyclFrameRt *f, const VmafxSyclRelease *r);
/* Reads recorded so far (white-box tests). */
uint32_t vmafx_sycl_rt_frame_reads(VmafxSyclFrameRt *f);

/* ---- Events ------------------------------------------------------------------- */

/* A slot of the release-event table: its handle is a sycl::event * the
 * caller may wait on once `recorded`. A slot made for a caller to fill
 * (vmafx_fence_create()) is recorded from the start. 0, -ENOMEM (table
 * full or no memory). */
int vmafx_sycl_rt_slot_new(bool recorded, uint32_t *slot);
uintptr_t vmafx_sycl_rt_slot_handle(uint32_t slot);
/* The slot whose handle is `handle`: 0, or -ENOENT. */
int vmafx_sycl_rt_slot_of(uintptr_t handle, uint32_t *slot);
void vmafx_sycl_rt_slot_ref(uint32_t slot);
/* Drop one reference; the last frees the slot. */
void vmafx_sycl_rt_slot_unref(uint32_t slot);
/* Mark the slot recorded with no work behind it (the planted early release). */
void vmafx_sycl_rt_slot_open(uint32_t slot);
/* 1 when the slot's event completed, 0 when it is pending or not recorded
 * yet, -EIO on a device fault. */
int vmafx_sycl_rt_slot_poll(uint32_t slot);
/* 1 when the caller's event (a sycl::event *) completed, 0 pending, -EIO. */
int vmafx_sycl_rt_event_poll(uintptr_t event);

/* NOLINTEND(modernize-use-using,performance-enum-size) */

#ifdef __cplusplus
}
#endif

#endif /* VMAF_SRC_SYCL_VMAFX_SYCL_RT_H_ */
