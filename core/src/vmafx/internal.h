/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Objects of the VMAFx API (ADR-1852, RC4 WP2) as the modules of
 * core/src/vmafx/ share them. Nothing here is exported.
 *
 * Reference counts: VmafxDevice, VmafxModel and VmafxModelSet carry their own
 * VmafRef. A VmafxFrame has none of its own: one reference is one count of
 * its picture's VmafRef, so a reference the engine keeps (frame n-1 / n-2,
 * ADR-1478) and a reference a caller holds are the same kind, and the frame
 * is released by whichever drops the last one (frame_host.c). Imported
 * frames (frame_import.c, RC4 WP3) follow the same rule: the release fence of
 * an imported frame is signalled where its last count is dropped.
 */

#ifndef VMAFX_INTERNAL_H
#define VMAFX_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "ref.h"
#include "vmafx/vmafx.h"

/* The import rule's host wait on the acquire fence before its one retry
 * (VmafxContextConfig.import_retry_wait_ns, ADR-1929): the default for 0,
 * and the largest accepted value (a larger one is refused, not clamped). */
#define VMAFX_IMPORT_RETRY_WAIT_DEFAULT_NS 10000000000ull
#define VMAFX_IMPORT_RETRY_WAIT_MAX_NS 600000000000ull

/* Hex SHA-256 plus its terminator. */
#define VMAFX_SHA256_HEX_SIZE 65u

struct VmafxDevice {
    VmafRef *refs;    /* NULL for the process CPU device, which is never freed */
    uint32_t backend; /* VmafxBackend */
    int32_t index;
    uint32_t flags; /* VmafxDeviceFlags it was created with */
    /* The backend lane's device (CUDA: cuda/vmafx_cuda.h), NULL for the CPU. */
    void *lane;
};

/* A host fence (fence.c): a reference-counted flag set once. The `handle` of
 * a VMAFX_FENCE_HOST VmafxFence points to one; each VmafxFence the library
 * hands out carries one reference. */
typedef struct VmafxHostFence VmafxHostFence;

/* The frame pool a frame returns to (frame_pool.c). */
typedef struct VmafxFramePool VmafxFramePool;

struct VmafxModel {
    VmafRef *refs;
    VmafModel *engine; /* this wrapper holds one owner of it (ADR-1755) */
    char sha256[VMAFX_SHA256_HEX_SIZE];
};

struct VmafxModelSet {
    VmafRef *refs;
    VmafModelCollection *engine; /* owns the member models */
    VmafxModel *lead;            /* the set holds one reference */
    char sha256[VMAFX_SHA256_HEX_SIZE];
};

struct VmafxFrame {
    VmafPicture pic; /* template; its VmafRef counts every reference */
    VmafxDevice *device;
    /* The picture's own release (vmaf_picture_alloc's buffer pool), or NULL
     * for wrapped planes. */
    int (*inner_release)(VmafPicture *pic, void *cookie);
    void *inner_cookie;
    VmafxFrameReleaseCallback release;
    void *user;
    /* RC4 WP3. Backend whose memory holds the planes (VmafxBackend): what
     * admission checks every registered extractor against. */
    uint32_t residency;
    /* VmafxHostFence * signalled when the last reference is dropped; 0 until
     * vmafx_frame_release_fence() asks for one (set once, atomically). */
    _Atomic(VmafxHostFence *) released;
    /* Planes vmafx_frame_import() converted (NV12 / P010 / P016), freed with
     * the frame; NULL when every plane is the producer's memory. */
    void *owned;
    /* The pool the frame returns to instead of being freed, or NULL. */
    VmafxFramePool *pool;
    /* RC4 WP3 backend lanes: the lane's per-frame state (device memory the
     * import converted into, gates of release fences, interop handles) and
     * the lane's release, which vmafx_frame_release() / the pool's release
     * call in place of vmafx_frame_signal_released(): it signals the release
     * fences once the device has run the frame's last reader. NULL for host
     * frames. */
    void *lane;
    int (*lane_release)(VmafxFrame *frame, VmafPicture *pic);
    /* Lane state that lives as long as the frame: a pool frame's (CUDA: the
     * event its last readers are recorded behind, which an acquire waits
     * on). */
    void *lane_persistent;
};

/* References a context holds until it is destroyed: one list per handle type. */
typedef struct VmafxHeld {
    void **items;
    uint32_t count;
    uint32_t capacity;
} VmafxHeld;

struct VmafxContext {
    VmafContext *engine;
    VmafxLogCallback log_callback;
    void *log_user;
    VmafLogSink sink;     /* delivers to log_callback; used when it is set */
    VmafxHeld models;     /* VmafxModel * (ADR-1755) */
    VmafxHeld model_sets; /* VmafxModelSet * */
    VmafxDevice *device;  /* vmafx_context_use_device(); NULL: the CPU, no device held */
    void *lane_state;     /* the engine state the device's lane imported (CUDA: a VmafCudaState) */
    uint64_t import_retry_wait_ns; /* the import rule's host wait, resolved (never 0) */
    bool have_frame;               /* a frame was submitted: the fields below are set */
    uint64_t last_index;           /* indices increase strictly (ADR-0152) */
    VmafxFrameDesc first_desc;     /* every frame keeps the first frame's geometry */
};

/* Where a failure is reported: the caller's error out-parameter, the log sink
 * of the context the call is about (NULL: the process log) and the API
 * function's name. */
typedef struct VmafxReport {
    VmafxError **error;
    const VmafLogSink *sink;
    const char *function;
} VmafxReport;

/* The log sink of a context with a log callback, else NULL (also for NULL). */
const VmafLogSink *vmafx_context_log_sink(const VmafxContext *context);

/* The report of the API function this expands in. */
#define VMAFX_REPORT(context, error)                                                               \
    {.error = (error), .sink = vmafx_context_log_sink(context), .function = __func__}

/* ---- Struct size negotiation (design section 2.6) ------------------------
 * Inputs: the library reads the fields the caller's struct_size covers into
 * `local`, which holds the defaults of every field (an _INIT value) and is
 * `full` bytes; struct_size below `min` (the size the struct had when it was
 * introduced) is VMAFX_E_ABI naming `subject`.
 * Outputs: the library writes min(struct_size, full) bytes of `record` and
 * stores that size, so an older caller receives the prefix it knows; a
 * struct_size below 4 is VMAFX_E_ABI. */
/* Size of each input struct when it was introduced: a caller's struct_size
 * below it is VMAFX_E_ABI. A struct that grows keeps its entry; a new input
 * struct adds one (sizeof while it is new). */
#define VMAFX_MIN_CONTEXT_CONFIG ((uint32_t)offsetof(VmafxContextConfig, log_callback)) /* 0.1.0 */
#define VMAFX_MIN_DEVICE_DESC ((uint32_t)offsetof(VmafxDeviceDesc, flags))              /* 0.1.1 */
#define VMAFX_MIN_MODEL_CONFIG ((uint32_t)sizeof(VmafxModelConfig))                     /* 0.1.1 */
#define VMAFX_MIN_FRAME_DESC ((uint32_t)sizeof(VmafxFrameDesc))                         /* 0.1.1 */
#define VMAFX_MIN_HOST_PLANES ((uint32_t)sizeof(VmafxHostPlanes))                       /* 0.1.1 */
#define VMAFX_MIN_FRAME_IMPORT ((uint32_t)offsetof(VmafxFrameImport, release))          /* 0.1.2 */
#define VMAFX_MIN_FENCE ((uint32_t)sizeof(VmafxFence))                                  /* 0.1.2 */

VmafxStatus vmafx_read_sized(const VmafxReport *report, void *local, uint32_t full, const void *in,
                             uint32_t min, const char *subject);
VmafxStatus vmafx_write_sized(const VmafxReport *report, void *out, const void *record,
                              uint32_t full, const char *subject);
/* vmafx_write_sized() into an output whose struct_size was already checked. */
void vmafx_store_sized(void *out, const void *record, uint32_t full);

/* ---- Engine access ------------------------------------------------------- */

/* The engine context; the context's log sink is installed on this thread
 * until vmafx_engine_leave(previous). */
const VmafLogSink *vmafx_engine_enter(const VmafxContext *context);
void vmafx_engine_leave(const VmafLogSink *previous);
VmafContext *vmafx_context_engine(const VmafxContext *context);

/* Make room for one more reference in `held`, so that the push after an
 * engine call that succeeded cannot fail. */
VmafxStatus vmafx_held_reserve(const VmafxReport *report, VmafxHeld *held);
/* Append `item`; a slot was reserved. */
void vmafx_held_push(VmafxHeld *held, void *item);

/* The engine's log level of a VmafxLogLevel (NONE for any other value;
 * values equal). */
enum VmafLogLevel vmafx_engine_log_level(uint32_t level);

/* After a successful engine close: free the engine state the device's lane
 * imported and drop the context's device reference (device_context.c). */
void vmafx_context_release_device(VmafxContext *context);

/* The process CPU device (never freed; ref / unref are no-ops on it). */
VmafxDevice *vmafx_device_cpu(void);

/* The engine picture that carries one reference of `frame`: the caller's
 * reference moves into the returned picture, which the engine releases. */
VmafPicture vmafx_frame_take_picture(VmafxFrame *frame);

/* Read and check a frame descriptor (pixel format, depth, size). */
VmafxStatus vmafx_frame_read_desc(const VmafxReport *report, const VmafxFrameDesc *desc,
                                  VmafxFrameDesc *d);
/* `device`, or the CPU device for NULL; VMAFX_E_NOTSUP naming `device` for a
 * device whose frames are not host frames. */
VmafxStatus vmafx_frame_host_device(const VmafxReport *report, VmafxDevice *device,
                                    VmafxDevice **resolved);
/* Point the frame's picture at `data` / `stride` (planes of the planar
 * geometry `d`), with a fresh reference count of 1 and vmafx_frame_release()
 * as its release: 0, or a negative errno with the picture unchanged. */
int vmafx_frame_bind(VmafxFrame *frame, const VmafxFrameDesc *d, void *const data[3],
                     const ptrdiff_t stride[3]);

/* The release of a frame that is not in a pool: runs once, on the thread
 * that drops the last reference; frees the frame (frame_host.c). */
int vmafx_frame_release(VmafPicture *pic, void *cookie);

/* Signal the frame's release fence, if one was asked for, and drop the
 * frame's reference to it (the frame's memory is no longer read). */
void vmafx_frame_signal_released(VmafxFrame *frame);

/* ---- Host fences (fence.c) ---------------------------------------------- */

/* A new unsignalled host fence with one reference, or NULL (no memory). */
VmafxHostFence *vmafx_host_fence_new(void);
VmafxHostFence *vmafx_host_fence_ref(VmafxHostFence *fence);
void vmafx_host_fence_unref(VmafxHostFence *fence);
void vmafx_host_fence_signal(VmafxHostFence *fence);
bool vmafx_host_fence_signalled(const VmafxHostFence *fence);
/* The host fence a VMAFX_FENCE_HOST VmafxFence names (borrowed), or
 * VMAFX_E_INVALID naming `subject` when its handle is not a live one. */
VmafxStatus vmafx_host_fence_of(const VmafxReport *report, const VmafxFence *fence,
                                const char *subject, VmafxHostFence **out);
/* Signal `fence` and drop one reference to it (a backend lane's completion
 * callback; no allocation, no lock). */
void vmafx_host_fence_signal_unref(VmafxHostFence *fence);
/* Poll `done(arg)` until it answers 1 or `timeout_ns` passed, sleeping between
 * looks on the clock host fences wait on (the virtual test clock included):
 * 1 when done, 0 at the timeout, a negative value as soon as `done` returns
 * one (a runtime failure). The backend lanes wait on their fences with it. */
int vmafx_fence_poll(int (*done)(const void *arg), const void *arg, uint64_t timeout_ns);

/* ---- Imports (frame_import.c) -------------------------------------------- */

/* How a producer lays out one pixel format vmafx_frame_import() takes. */
typedef struct VmafxImportLayout {
    uint32_t pix_fmt;    /* VmafxPixelFormat the producer hands over */
    uint32_t planar_fmt; /* VmafxPixelFormat of the frame it makes */
    uint32_t n_planes;   /* planes the producer hands over */
    uint32_t bpc_min;
    uint32_t bpc_max;
    uint32_t shift;   /* right shift of every sample (P010: 6) */
    bool interleaved; /* plane 1 holds Cb / Cr pairs */
    const char *name; /* FFmpeg's name, for messages */
} VmafxImportLayout;

/* Bytes of one row of producer plane `i` and its rows, for the planar
 * geometry `pw` / `ph` of the frame. */
void vmafx_import_plane_extent(const VmafxImportLayout *layout, uint32_t bpc, uint32_t i,
                               const unsigned pw[3], const unsigned ph[3], uint64_t *row,
                               uint64_t *rows);
/* One plane of linear memory (`memory` names it in messages): an address, a
 * linear layout, a pitch that holds a row and rows that lie inside the given
 * size and the address space. */
VmafxStatus vmafx_import_check_linear_plane(const VmafxReport *report, const VmafxImportPlane *p,
                                            uint32_t i, uint64_t row, uint64_t rows,
                                            const char *memory);
/* Subject names of the fields of plane `i` (handle, pitch, modifier, size). */
const char *vmafx_import_plane_field(uint32_t i, const char *field);

/* ---- Admission (frame_import_admit.c) ------------------------------------ */

/* Every extractor registered on `context` can read a frame whose planes are
 * in the memory of `residency` (a VmafxBackend) without a host copy, or
 * VMAFX_E_NOTSUP naming each one that cannot and why (ADR-1688
 * generalised). */
VmafxStatus vmafx_admit_residency(const VmafxReport *report, const VmafxContext *context,
                                  uint32_t residency);
/* vmafx_admit_residency() for `frame`, and a device-resident frame lives on
 * the context's device. */
VmafxStatus vmafx_admit_frame(const VmafxReport *report, const VmafxContext *context,
                              const VmafxFrame *frame);

/* Lower-case name of a VmafxBackend ("cpu", "cuda", ...; "unknown"). */
const char *vmafx_backend_name(uint32_t backend);
/* Name of a VmafxMemoryKind ("HOST", ...; "unknown"). */
const char *vmafx_memory_kind_name(uint32_t memory);
/* FFmpeg's name of a pixel format vmafx_frame_import() takes ("nv12", ...;
 * "unknown"). */
const char *vmafx_import_format_name(uint32_t pix_fmt);

/* The engine's pixel format of a VmafxPixelFormat (UNKNOWN for any other
 * value; values equal). */
enum VmafPixelFormat vmafx_engine_pixel_format(uint32_t pix_fmt);

/* A model set's engine collection. */
VmafModelCollection *vmafx_model_set_engine(const VmafxModelSet *set);

#endif /* VMAFX_INTERNAL_H */
