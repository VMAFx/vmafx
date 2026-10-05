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
 * is released by whichever drops the last one (frame_host.c).
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

/* Hex SHA-256 plus its terminator. */
#define VMAFX_SHA256_HEX_SIZE 65u

struct VmafxDevice {
    VmafRef *refs;    /* NULL for the process CPU device, which is never freed */
    uint32_t backend; /* VmafxBackend */
    int32_t index;
};

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
    VmafLogSink sink;          /* delivers to log_callback; used when it is set */
    VmafxHeld models;          /* VmafxModel * (ADR-1755) */
    VmafxHeld model_sets;      /* VmafxModelSet * */
    bool have_frame;           /* a frame was submitted: the fields below are set */
    uint64_t last_index;       /* indices increase strictly (ADR-0152) */
    VmafxFrameDesc first_desc; /* every frame keeps the first frame's geometry */
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
#define VMAFX_MIN_DEVICE_DESC ((uint32_t)sizeof(VmafxDeviceDesc))                       /* 0.1.1 */
#define VMAFX_MIN_MODEL_CONFIG ((uint32_t)sizeof(VmafxModelConfig))                     /* 0.1.1 */
#define VMAFX_MIN_FRAME_DESC ((uint32_t)sizeof(VmafxFrameDesc))                         /* 0.1.1 */
#define VMAFX_MIN_HOST_PLANES ((uint32_t)sizeof(VmafxHostPlanes))                       /* 0.1.1 */

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

/* The process CPU device (never freed; ref / unref are no-ops on it). */
VmafxDevice *vmafx_device_cpu(void);

/* The engine picture that carries one reference of `frame`: the caller's
 * reference moves into the returned picture, which the engine releases. */
VmafPicture vmafx_frame_take_picture(VmafxFrame *frame);

/* The engine's pixel format of a VmafxPixelFormat (UNKNOWN for any other
 * value; values equal). */
enum VmafPixelFormat vmafx_engine_pixel_format(uint32_t pix_fmt);

/* A model set's engine collection. */
VmafModelCollection *vmafx_model_set_engine(const VmafxModelSet *set);

#endif /* VMAFX_INTERNAL_H */
