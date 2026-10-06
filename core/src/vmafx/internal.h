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

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "ref.h"
#include "vmafx/import_convert.h"
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

/* The windows of a context (window.c, RC4 WP4). */
typedef struct VmafxWindowSet VmafxWindowSet;

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

/* "sha256:" + 64 hex digits + NUL (RC4 WP5 digests). */
#define VMAFX_DIGEST_TEXT_SIZE 72u

/* One caller annotation of a provenance record (vmafx_context_annotate()). */
typedef struct VmafxAnnotationEntry {
    char *key;
    char *value;
} VmafxAnnotationEntry;

/* The provenance state of a context (provenance.c, RC4 WP5): what the caller
 * added, and the text of the last record query, which the record's strings
 * point into. `lock` serialises queries, which may run on any thread. */
typedef struct VmafxProvenanceState {
    pthread_mutex_t lock;
    bool lock_ready;
    VmafxAnnotationEntry *annotations;
    uint32_t n_annotations;
    uint32_t annotations_capacity;
    char encode_record[VMAFX_DIGEST_TEXT_SIZE]; /* "" when none */
    char digest[VMAFX_DIGEST_TEXT_SIZE];
    char scores_digest[VMAFX_DIGEST_TEXT_SIZE];
    char *json; /* last vmafx_context_provenance_json() */
} VmafxProvenanceState;

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
    VmafLogSink sink;                /* delivers to log_callback; used when it is set */
    VmafxHeld models;                /* VmafxModel * (ADR-1755) */
    VmafxHeld model_sets;            /* VmafxModelSet * */
    VmafxDevice *device;             /* vmafx_context_use_device(); NULL: the CPU, no device held */
    void *lane_state;                /* engine state the lane imported (CUDA: a VmafCudaState) */
    uint64_t import_retry_wait_ns;   /* the import rule's host wait, resolved (never 0) */
    bool have_frame;                 /* a frame was submitted: the fields below are set */
    uint64_t last_index;             /* indices increase strictly (ADR-0152) */
    VmafxFrameDesc first_desc;       /* every frame keeps the first frame's geometry */
    VmafxProvenanceState provenance; /* RC4 WP5 */
    /* RC4 WP4 (window.c, ADR-2074): the windows, their completion thread
     * and the engine lock; created with the context, never NULL after
     * vmafx_context_create() succeeded. */
    VmafxWindowSet *windows;
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
#define VMAFX_MIN_CONVERT_DESC ((uint32_t)sizeof(VmafxConvertDesc))                     /* 0.1.6 */
#define VMAFX_MIN_DNN_CONFIG ((uint32_t)sizeof(VmafxDnnConfig))                         /* 0.1.6 */
#define VMAFX_MIN_MCP_CONFIG ((uint32_t)sizeof(VmafxMcpConfig))                         /* 0.1.6 */
#define VMAFX_MIN_MCP_SSE_CONFIG ((uint32_t)sizeof(VmafxMcpSseConfig))                  /* 0.1.6 */
#define VMAFX_MIN_MCP_UDS_CONFIG ((uint32_t)sizeof(VmafxMcpUdsConfig))                  /* 0.1.6 */
#define VMAFX_MIN_MCP_STDIO_CONFIG ((uint32_t)sizeof(VmafxMcpStdioConfig))              /* 0.1.6 */
#define VMAFX_MIN_WINDOW_REQUEST ((uint32_t)sizeof(VmafxWindowRequest))                 /* 0.1.8 */
#define VMAFX_MIN_WINDOW_CLOCK_CONFIG ((uint32_t)sizeof(VmafxWindowClockConfig))        /* 0.1.8 */

VmafxStatus vmafx_read_sized(const VmafxReport *report, void *local, uint32_t full, const void *in,
                             uint32_t min, const char *subject);
VmafxStatus vmafx_write_sized(const VmafxReport *report, void *out, const void *record,
                              uint32_t full, const char *subject);
/* vmafx_write_sized() into an output whose struct_size was already checked. */
void vmafx_store_sized(void *out, const void *record, uint32_t full);

/* ---- Engine access ------------------------------------------------------- */

/* Enter the engine of `context`: takes the context's engine lock (the
 * window completion thread calls the engine too, ADR-2074) and installs the
 * context's log sink on this thread until vmafx_engine_leave(context,
 * previous). Never nested: an entered section calls no other VMAFx function
 * that enters. */
const VmafLogSink *vmafx_engine_enter(const VmafxContext *context);
void vmafx_engine_leave(const VmafxContext *context, const VmafLogSink *previous);
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

/* ---- libvmaf pictures as frames (bridge.c, RC4 WP6) ----------------------- */

/* The frame a libvmaf picture is a view of, or NULL when an engine path made
 * it without one (`pic` has a slot: priv and ref set). */
VmafxFrame *vmafx_frame_of_picture(const VmafPicture *pic);
/* The frame of `pic`, adopting a picture the engine made without one: the
 * frame takes over the picture's release (no reference is added; the count
 * stays the picture's). VMAFX_E_NOMEM, with the picture unchanged. */
VmafxStatus vmafx_frame_adopt_picture(const VmafxReport *report, const VmafPicture *pic,
                                      VmafxFrame **out);
/* True when `release` is the release of a pool frame (frame_pool.c). */
bool vmafx_frame_pool_release_is(int (*release)(VmafPicture *pic, void *cookie));

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

/* ---- Release events of the device lanes (release_events.c) --------------- */

/* Release events handed out and not yet both recorded and destroyed by
 * every holder: frames with a pending device-event release fence, per
 * backend (HISS-02 bound; a decoder pool holds a few dozen). */
#define VMAFX_RELEASE_EVENTS 4096u

typedef struct VmafxReleaseEvent {
    uintptr_t event; /* the runtime's event; 0: a free slot */
    uint32_t refs;   /* the frame's until it records it, one per fence handed out */
    bool recorded;
} VmafxReleaseEvent;

/* One backend's table (a static object of the lane, ADR-2023 item 3). */
typedef struct VmafxReleaseEvents {
    pthread_mutex_t lock;
    uint32_t high; /* slots below it were used at least once */
    void (*destroy)(uintptr_t event);
    VmafxReleaseEvent slot[VMAFX_RELEASE_EVENTS];
} VmafxReleaseEvents;

/* A frame's release event with one more reference for the caller: `fresh`
 * (an unrecorded event the lane just created, holding the frame's reference
 * too) when `*slot1` is 0, else the event of slot `*slot1 - 1`. 0, or -EBUSY
 * when the table is full (`fresh` is then not taken). */
int vmafx_release_events_take(VmafxReleaseEvents *t, uint32_t *slot1, uintptr_t fresh,
                              uintptr_t *event);
/* Record the frame's release event with `record(event, arg)` (under the
 * table's lock, so a wait never sees it half recorded), mark it recorded and
 * drop the frame's reference; `*slot1` becomes 0. Nothing for slot 0. */
int vmafx_release_events_record(VmafxReleaseEvents *t, uint32_t *slot1,
                                int (*record)(uintptr_t event, void *arg), void *arg);
/* `event` is a release event handed out and not recorded yet. */
bool vmafx_release_events_unrecorded(VmafxReleaseEvents *t, uintptr_t event);
/* Drop a fence's reference of `event`: false when it is not in the table. */
bool vmafx_release_events_drop(VmafxReleaseEvents *t, uintptr_t event);

/* Producer fences checked on the host (GL syncs, sync_files, dma-buf implicit
 * fences): sync_object.h, one implementation for every lane (HISS-19). */

/* ---- GL textures as linear dma-bufs (egl_export.c) ------------------------ */

/* One plane to export: GL texture name, DRM fourcc of its format (R8, GR88,
 * R16, GR1616) and its size in samples. */
typedef struct VmafxEglTarget {
    uintptr_t texture;
    uint32_t fourcc;
    uint32_t w;
    uint32_t h;
} VmafxEglTarget;

/* One exported plane: a dma-buf descriptor the caller closes
 * (vmafx_egl_close_planes()), its layout and size. */
typedef struct VmafxEglPlane {
    int fd;
    uint64_t offset;
    uint64_t pitch;
    uint64_t modifier;
    uint64_t size;
} VmafxEglPlane;

/* What vmafx_egl_export_planes() does with a texture the driver exports in
 * its own tiling. One EGL export for every lane (HISS-19): the HIP lane reads
 * linear rows (refuse, or copy with VMAFX_IMPORT_ALLOW_COPY, ADR-2132), the
 * SYCL lane de-tiles Intel tilings on the device (keep, ADR-2091). */
typedef enum VmafxEglTiled {
    VMAFX_EGL_TILED_REFUSE = 0, /* VMAFX_E_NOTSUP naming VMAFX_IMPORT_ALLOW_COPY */
    VMAFX_EGL_TILED_COPY = 1,   /* a GPU copy into a linear dma-buf (GBM, one blit) */
    VMAFX_EGL_TILED_KEEP = 2,   /* as exported, with its modifier; the importer reads it */
} VmafxEglTiled;

/* The `n` (1 to 3) textures of the EGL context current on the calling thread
 * as dma-bufs in `out`. A texture the driver exports linear is exported as it
 * is; one exported tiled is handled as `tiled` says; `*copied` tells whether
 * any was copied. A target's `fourcc` is checked against the export unless 0.
 * `device_pci` ("0000:0e:00.0") is the importing device's GPU, checked
 * against the context's; NULL skips the check. With REFUSE and COPY every
 * export waits (1 s at most) for the producer's writes; with KEEP the importer
 * honours the dma-buf's implicit fences. On a failure every descriptor is
 * closed. */
VmafxStatus vmafx_egl_export_planes(const VmafxReport *report, const char *backend,
                                    const char *device_pci, const VmafxEglTarget *targets,
                                    uint32_t n, VmafxEglTiled tiled, VmafxEglPlane out[3],
                                    bool *copied);
/* Close the descriptors of `n` exported planes. */
void vmafx_egl_close_planes(VmafxEglPlane *planes, uint32_t n);

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
    uint32_t packed;  /* VmafxImportPacked: one plane of packed Y, Cb, Cr */
    uint8_t elem[3];  /* packed: element of a group that holds Y, Cb, Cr */
    bool msb;         /* the `bpc` most significant bits of 16-bit words (shift 16 - bpc) */
} VmafxImportLayout;

/* Packed layouts (one producer plane, ADR-2133): 0 is none. */
typedef enum VmafxImportPacked {
    VMAFX_IMPORT_PACKED_NONE = 0,
    /* Y0 Cb Y1 Cr elements (bytes at 8 bits, else 16-bit words) per two pixels:
     * elem = Y0, Cb, Cr (0, 1, 3). */
    VMAFX_IMPORT_PACKED_YUYV = 1,
    /* Four elements per pixel (bytes at 8 bits, else 16-bit words); elem
     * gives the position of Y, Cb and Cr. */
    VMAFX_IMPORT_PACKED_UYV4 = 2,
    /* One 32-bit word per pixel: Cb | Y << 10 | Cr << 20 (Y410, XV30). */
    VMAFX_IMPORT_PACKED_XVYU2101010 = 3
} VmafxImportPacked;

/* The right shift of every sample of a frame of `bpc` bits in `layout`. */
static inline uint32_t vmafx_import_shift(const VmafxImportLayout *layout, uint32_t bpc)
{
    return layout->msb ? 16u - bpc : layout->shift;
}

/* Bytes of one row of producer plane `i` and its rows, for the planar
 * geometry `pw` / `ph` of the frame. */
void vmafx_import_plane_extent(const VmafxImportLayout *layout, uint32_t bpc, uint32_t i,
                               const unsigned pw[3], const unsigned ph[3], uint64_t *row,
                               uint64_t *rows);
/* How plane `i` of the planar frame a layout makes is read from the
 * producer's planes (the CPU reference in vmafx/import_convert.h). */
void vmafx_import_plane_read(const VmafxImportLayout *layout, uint32_t bpc, uint32_t i,
                             VmafxImportRead *out);
/* One plane of linear memory (`memory` names it in messages): an address, a
 * linear layout, a pitch that holds a row and rows that lie inside the given
 * size and the address space. */
VmafxStatus vmafx_import_check_linear_plane(const VmafxReport *report, const VmafxImportPlane *p,
                                            uint32_t i, uint64_t row, uint64_t rows,
                                            const char *memory);
/* Subject names of the fields of plane `i` (handle, pitch, modifier, size,
 * offset, fd, plane_index). */
const char *vmafx_import_plane_field(uint32_t i, const char *field);

/* Wait until `fence` is signalled or `timeout_ns` passed (UINT64_MAX: no
 * limit): true when signalled. Polls a monotonic clock (fence.c). */
bool vmafx_host_fence_wait(const VmafxHostFence *fence, uint64_t timeout_ns);

/* ---- Pooling and windows (score.c, window.c; RC4 WP4, ADR-2074) ---------- */

/* What a pooled score pools: `kind` is a VmafxWindowTarget and names the one
 * of `model`, `set` and `feature` that is set. */
typedef struct VmafxPoolTarget {
    uint32_t kind;
    const VmafxModel *model;
    const VmafxModelSet *set;
    const char *feature;
} VmafxPoolTarget;

/* A pooled score: `value` for a model or a feature, the four bootstrap values
 * for a model set (`value` is its bagging score). */
typedef struct VmafxPoolValue {
    double value;
    double stddev;
    double ci95_lo;
    double ci95_hi;
} VmafxPoolValue;

/* The one pooling implementation of vmafx_score_pooled(),
 * vmafx_feature_score_pooled(), vmafx_score_pooled_model_set() and the window
 * scores (HISS-19): `target` pooled with the VmafxPool `pool` over
 * [first, last] (first <= last <= UINT_MAX) by the engine, on the calling
 * thread with the context's log sink installed. Returns the engine's errno
 * (-EAGAIN: a frame is not final). */
int vmafx_pool_engine(VmafxContext *context, const VmafxPoolTarget *target, uint32_t pool,
                      uint64_t first, uint64_t last, VmafxPoolValue *out);
/* The model's, the model set's or the feature's name. */
const char *vmafx_pool_target_name(const VmafxPoolTarget *target);

/* The window set of a new context (its engine lock included), and the
 * frame listener the engine's worker jobs call; false when it cannot be
 * allocated. Called before the context first enters its engine. */
bool vmafx_windows_init(VmafxContext *context);
void vmafx_windows_frame_final(void *user);
/* The context's engine lock (vmafx_engine_enter() / _leave()). */
void vmafx_context_lock(const VmafxContext *context);
void vmafx_context_unlock(const VmafxContext *context);

/* Window hooks, called on the thread that feeds the context after the
 * engine call: a frame `index` was submitted or a score of frame `index`
 * imported; the context was flushed. Each wakes the completion thread. */
void vmafx_windows_note_index(VmafxContext *context, uint64_t index);
void vmafx_windows_note_flush(VmafxContext *context);
/* vmafx_context_destroy(): pause stops the completion thread's engine work
 * before the engine closes (resume undoes it when the close fails and the
 * context stays valid, ADR-1336); close completes the open windows with
 * VMAFX_E_INVALID, runs every callback, joins the thread and drops the set;
 * free drops a set whose context never got an engine. */
void vmafx_windows_pause(VmafxContext *context);
void vmafx_windows_resume(VmafxContext *context);
void vmafx_windows_close(VmafxContext *context);

/* The last value of VmafxMemoryKind and VmafxFenceKind this ABI declares. */
#define VMAFX_MEMORY_KIND_LAST VMAFX_MEMORY_VULKAN
#define VMAFX_FENCE_KIND_LAST VMAFX_FENCE_VULKAN_SEMAPHORE

/* ---- Vulkan imports (frame_import_vulkan.c, RC4 WP3 Vulkan lane) --------- */

/* Names of VmafxVulkanHandleType / VmafxVulkanTiling values ("unknown"). */
const char *vmafx_vulkan_handle_name(uint32_t type);
const char *vmafx_vulkan_tiling_name(uint32_t tiling);
/* The device-independent checks of a VULKAN descriptor: handle type, tiling,
 * flags, and each plane's descriptor, allocation size and rows. */
VmafxStatus vmafx_import_check_vulkan(const VmafxReport *report, const VmafxFrameImport *d,
                                      const VmafxImportLayout *layout);
/* A PCI bus id "dddd:bb:dd.f" (cuDeviceGetPCIBusId(), hipDeviceGetPCIBusId())
 * as domain, bus, device and function; UINT32_MAX in each for NULL or text
 * of another form. */
void vmafx_parse_pci_bus_id(const char *bus_id, uint32_t pci[4]);
/* The producer's GPU (`d->vulkan_pci`) is the device's (`pci`): else
 * VMAFX_E_NOTSUP naming desc.vulkan_pci. */
VmafxStatus vmafx_import_check_vulkan_device(const VmafxReport *report, const VmafxFrameImport *d,
                                             const uint32_t pci[4], const char *backend);
/* The further acquire fences: kinds the API declares, and on a device of
 * `backend` that waits on several (CUDA) only. */
VmafxStatus vmafx_import_check_acquire_more(const VmafxReport *report, const VmafxFrameImport *d,
                                            uint32_t backend);
/* Subject of acquire fence `i` of an import: desc.acquire, then the further
 * ones (desc.acquire_more[0], [1]). */
const char *vmafx_import_acquire_name(uint32_t i);
/* A LINEAR or DRM_FORMAT_MODIFIER VULKAN descriptor as the DMABUF one a
 * dma-buf lane imports (SYCL, HIP): OPTIMAL tiling and an OPAQUE_FD that is
 * no dma-buf are refused naming the field. */
VmafxStatus vmafx_import_vulkan_as_dmabuf(const VmafxReport *report, const VmafxFrameImport *d,
                                          const char *backend, VmafxFrameImport *out);

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

/* ---- Provenance (provenance*.c, RC4 WP5) ---------------------------------- */

/* Set up / release a context's provenance state: 0 or a negative errno. */
int vmafx_provenance_init(VmafxProvenanceState *state);
void vmafx_provenance_release(VmafxProvenanceState *state);

/* The build description of the library (provenance_build.c). */
typedef struct VmafxBuildInfo {
    const char *commit;
    const char *compiler;
    const char *flags;
    const char *fp_policy;
    const char *backends;
    const char *arch;
    uint32_t rust_twins;
} VmafxBuildInfo;

/* This library's build description; `build_id` its digest (both static). */
const VmafxBuildInfo *vmafx_build_info(void);
const char *vmafx_build_id(void);
/* `sha256:` and the SHA-256 of the canonical JSON of `info` (commit and arch
 * excluded) into `out`: 0, or -ENOMEM. */
int vmafx_build_id_of(const VmafxBuildInfo *info, char out[VMAFX_DIGEST_TEXT_SIZE]);
/* Highest SIMD level of the CPU flags in effect (`avx512`, `neon`, `scalar`). */
const char *vmafx_simd_level(void);

/* Text of a VmafxPixelFormat / VmafxFeatureSource value as the proto JSON
 * mapping names it ("yuv420p", "extractor"; "unknown"). */
const char *vmafx_pixel_format_name(uint32_t pix_fmt);
const char *vmafx_feature_source_name(uint32_t source);

#endif /* VMAFX_INTERNAL_H */
