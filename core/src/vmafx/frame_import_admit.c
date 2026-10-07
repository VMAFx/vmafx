/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Admission and the import rule of the VMAFx API (RC4 WP3 common lane).
 *
 * Admission (ADR-1688 generalised to every backend, design section 2.7):
 * before a frame is counted, every registered extractor is checked against
 * the memory the frame's planes are in. A host frame is read by CPU
 * extractors and uploaded by device twins (the explicit host path). A frame
 * in device memory is read only by twins of that backend that read imported
 * frames; a CPU extractor would need a host copy of device memory and is
 * refused, as is a twin of another backend. Each refusing extractor is
 * named, with the reason.
 *
 * The import rule (ADR-1852 decision D8, design section 5.4):
 * vmafx_context_import_frame() imports and admits; a transient failure
 * (VMAFX_E_BUSY, VMAFX_E_TIMEOUT) is retried once after a host wait on the
 * acquire fence of at most the context's import_retry_wait_ns (10 s by
 * default); a second failure, or any other, fails with one message that
 * names the backend, device, input, memory kind, pixel format, modifiers and
 * the refusing extractors. Nothing falls back to a host copy.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "error_internal.h"
#include "frame_import_hooks.h"
#include "internal.h"
#include "vmafx/vmafx.h"
#ifdef HAVE_CUDA
#include "cuda/vmafx_cuda.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Bytes of the list of refusing extractors in one message. */
#define VMAFX_REFUSAL_TEXT 640u
/* Bytes of the description of an import in the import rule's message. */
#define VMAFX_IMPORT_TEXT 320u

/* ---- Admission ------------------------------------------------------------------ */

/* Why an extractor on `backend` cannot read a frame in the memory of
 * `residency`, or NULL when it can. */
static const char *refusal(const char *extractor, uint32_t backend, uint32_t residency)
{
    if (residency == VMAFX_BACKEND_CPU) {
        return NULL;
    }
    if (backend == VMAFX_BACKEND_CPU) {
        return "runs on the CPU and would need a host copy of device memory";
    }
    if (backend != residency) {
        return "runs on another backend than the memory the frame is in";
    }
    /* The backend lanes answer this per extractor and options (the
     * generalisation of reads_shared_luma_only(), ADR-1688). */
#ifdef HAVE_CUDA
    if (backend == VMAFX_BACKEND_CUDA) {
        return vmafx_cuda_refusal(extractor);
    }
#else
    (void)extractor;
#endif
    return "reads no imported frame on this backend in this build";
}

/* Append the formatted text to `buf` (size `size`, `len` used), truncating;
 * returns the new length. */
static size_t append(char *buf, size_t size, size_t len, const char *fmt, ...)
{
    if (len + 1u >= size) {
        return len;
    }
    va_list args;
#if defined(__clang__) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    /* As in error.c: Clang lowers the C23 va_start macro to
     * __builtin_c23_va_start, which its VAList analyzer does not model yet. */
    __builtin_va_start(args, fmt);
#else
    va_start(args, fmt);
#endif
    const int written = vsnprintf(buf + len, size - len, fmt, args);
    va_end(args);
    if (written < 0) {
        return len;
    }
    const size_t added = (size_t)written;
    return added < size - len ? len + added : size - 1u;
}

VmafxStatus vmafx_admit_residency(const VmafxReport *report, const VmafxContext *context,
                                  uint32_t residency)
{
    assert(report && context);
    if (residency == VMAFX_BACKEND_CPU) {
        return VMAFX_OK;
    }
    char list[VMAFX_REFUSAL_TEXT] = "";
    size_t len = 0;
    const char *first = NULL;
    unsigned refused = 0;
    const unsigned n = vmaf_engine_extractor_count(context->engine);
    for (unsigned i = 0; i < n; i++) {
        const char *name = NULL;
        enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
        if (vmaf_engine_registered_feature_extractor(context->engine, i, &name, &backend) != 0) {
            continue;
        }
        const char *const why = refusal(name, (uint32_t)backend, residency);
        if (why) {
            first = first ? first : name;
            len = append(list, sizeof(list), len, "%s%s (%s, %s)", refused ? "; " : "", name,
                         vmafx_backend_name((uint32_t)backend), why);
            refused++;
        }
    }
    if (!refused) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_EXTRACTOR, first,
                      "a frame in %s memory is refused by %u extractor%s: %s",
                      vmafx_backend_name(residency), refused, refused == 1u ? "" : "s", list);
}

/* The frame lives on the context's device (a host frame on any). */
static VmafxStatus admit_device(const VmafxReport *report, const VmafxContext *context,
                                const VmafxFrame *frame)
{
    if (frame->residency == VMAFX_BACKEND_CPU || context->device == frame->device) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "frame",
                      "the frame is in %s memory of a device the context does not score on",
                      vmafx_backend_name(frame->residency));
}

VmafxStatus vmafx_admit_frame(const VmafxReport *report, const VmafxContext *context,
                              const VmafxFrame *frame)
{
    const VmafxStatus status = vmafx_admit_residency(report, context, frame->residency);
    return status == VMAFX_OK ? admit_device(report, context, frame) : status;
}

VmafxStatus vmafx_context_admit(const VmafxContext *context, const VmafxFrame *frame,
                                VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !frame) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "frame", "NULL argument");
    }
    return vmafx_admit_frame(&report, context, frame);
}

/* ---- The import rule (D8) ----------------------------------------------------------- */

static bool transient(VmafxStatus status)
{
    return status == VMAFX_E_BUSY || status == VMAFX_E_TIMEOUT;
}

/* Import and admit; on any failure `*out` is NULL and `*error` set. */
static VmafxStatus import_admitted(VmafxContext *context, VmafxDevice *device,
                                   const VmafxFrameImport *desc, VmafxFrame **out,
                                   VmafxError **error)
{
    VmafxStatus status = vmafx_frame_import(device, desc, out, error);
    if (status == VMAFX_OK) {
        status = vmafx_context_admit(context, *out, error);
        if (status != VMAFX_OK) {
            vmafx_frame_unref(*out);
            *out = NULL;
        }
    }
    return status;
}

/* "main: backend cpu device 0, memory HOST, nv12 8-bit 1920x1080, modifiers
 * 0x0 0x0" for the import rule's message. */
static void describe_import(char *buf, size_t size, const VmafxDevice *device,
                            const VmafxFrameImport *d, const char *input)
{
    const VmafxDevice *const dev = device ? device : vmafx_device_cpu();
    size_t len = append(buf, size, 0, "%s: backend %s device %d, memory %s, %s %u-bit %ux%u",
                        input ? input : "frame", vmafx_backend_name(dev->backend), (int)dev->index,
                        vmafx_memory_kind_name(d->memory), vmafx_import_format_name(d->pix_fmt),
                        (unsigned)d->bpc, (unsigned)d->w, (unsigned)d->h);
    len = append(buf, size, len, ", modifiers");
    const uint32_t n = d->n_planes < 3u ? d->n_planes : 3u;
    for (uint32_t i = 0; i < n; i++) {
        len = append(buf, size, len, " 0x%llx", (unsigned long long)d->plane[i].modifier);
    }
}

/* Fail under the import rule, naming the import and the last failure
 * (released here). */
static VmafxStatus fail_named(const VmafxReport *report, const VmafxDevice *device,
                              const VmafxFrameImport *d, const char *input, VmafxError *last,
                              unsigned attempts)
{
    char what[VMAFX_IMPORT_TEXT];
    describe_import(what, sizeof(what), device, d, input);
    const VmafxStatus status = vmafx_error_status(last);
    const VmafxStatus failed =
        VMAFX_FAIL(report, status, vmafx_error_errno(last), vmafx_error_subject_kind(last),
                   vmafx_error_subject(last), "%s: %s (%u attempt%s; no host copy was made)", what,
                   vmafx_error_message(last), attempts, attempts == 1u ? "" : "s");
    vmafx_error_free(last);
    return failed;
}

/* The descriptor as far as the caller's struct_size covers it (for the
 * message and the acquire fence); zeroes when it is unreadable. */
static VmafxFrameImport readable_desc(const VmafxFrameImport *desc)
{
    VmafxFrameImport d = VMAFX_FRAME_IMPORT_INIT;
    uint32_t size = 0;
    memcpy(&size, desc, sizeof(size));
    if (size >= VMAFX_MIN_FRAME_IMPORT) {
        /* The prefix the caller's ABI has; the fields after it keep their
         * initialiser's values. */
        memcpy(&d, desc, size < sizeof(d) ? size : sizeof(d));
        d.struct_size = (uint32_t)sizeof(d);
    }
    return d;
}

VmafxStatus vmafx_context_import_frame(VmafxContext *context, VmafxDevice *device,
                                       const VmafxFrameImport *desc, const char *input,
                                       VmafxFrame **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (out) {
        *out = NULL;
    }
    if (!context || !desc || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" :
                          !desc    ? "desc" :
                                     "out",
                          "NULL argument");
    }
    const VmafxFrameImport d = readable_desc(desc);
    VmafxError *last = NULL;
    VmafxStatus status = import_admitted(context, device, desc, out, &last);
    unsigned attempts = 1u;
    if (transient(status)) {
        VmafxError *waited = NULL;
        vmafx_test_note_retry_wait(context->import_retry_wait_ns);
        (void)vmafx_fence_wait(&d.acquire, context->import_retry_wait_ns, &waited);
        vmafx_error_free(waited);
        vmafx_error_free(last);
        last = NULL;
        status = import_admitted(context, device, desc, out, &last);
        attempts = 2u;
    }
    if (status == VMAFX_OK) {
        return VMAFX_OK;
    }
    return fail_named(&report, device, &d, input, last, attempts);
}

/* NOLINTEND(modernize-use-nullptr) */
