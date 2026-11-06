/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * LD_PRELOAD fault injector for the FFmpeg filter tests under
 * ffmpeg-patches/test/, run against an FFmpeg that links libvmafx.so
 * dynamically. Unset variables inject nothing; every call that is not
 * injected goes to libvmafx. Build with -D_GNU_SOURCE (RTLD_NEXT) against the
 * installed vmafx headers, as the scripts do.
 *
 * vmafx_context_import_frame() (check-vmafx-import-failure.sh): calls number
 *   VMAF_TEST_IMPORT_FAIL_AT .. VMAF_TEST_IMPORT_FAIL_AT + VMAF_TEST_IMPORT_FAIL_COUNT - 1
 *   (1-based, over every call, reference and distorted) return VMAFX_E_BUSY.
 *
 * vmafx_submit() (check-vmafx-no-score-after-error.sh): submit calls
 *   number VMAF_TEST_READ_FAIL_AT .. VMAF_TEST_READ_FAIL_AT +
 *   VMAF_TEST_READ_FAIL_COUNT - 1 (1-based, one call per frame) return
 *   VMAFX_E_IO.
 *
 * vmafx_flush() (check-vmafx-no-score-after-error.sh): VMAF_TEST_FLUSH_FAIL=1
 *   makes the flush call return VMAFX_E_IO.
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <vmafx/context.h>
#include <vmafx/frame.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's C mode has
 * no `nullptr`. ADR-1138. */

typedef VmafxStatus (*import_frame_fn)(VmafxContext *context, VmafxDevice *device,
                                       const VmafxFrameImport *desc, const char *input,
                                       VmafxFrame **out, VmafxError **error);
typedef VmafxStatus (*submit_fn)(VmafxContext *context, VmafxFrame *reference,
                                 VmafxFrame *distorted, uint64_t index, VmafxError **error);
typedef VmafxStatus (*flush_fn)(VmafxContext *context, VmafxError **error);

static long env_long(const char *name)
{
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-0488: test shim, one caller thread. */
    const char *v = getenv(name);
    if (v == NULL || *v == '\0') {
        return 0;
    }
    char *end = NULL;
    const long n = strtol(v, &end, 10);
    return (end != NULL && *end == '\0' && n > 0) ? n : 0;
}

/* 1 when this call (counted in `counter`) falls in the window the two
 * variables name. */
static int injected(atomic_long *counter, const char *at_name, const char *count_name)
{
    const long call = atomic_fetch_add(counter, 1) + 1;
    const long fail_at = env_long(at_name);
    const long fail_count = env_long(count_name);
    return fail_at > 0 && call >= fail_at && call < fail_at + fail_count;
}

/* libvmafx's definition of `name`, copied into `*fn` (`size` bytes); -ENOSYS
 * when the symbol is missing. POSIX: a dlsym() result converts to a function
 * pointer; memcpy keeps ISO C quiet. */
static int real_symbol(const char *name, void *fn, size_t size)
{
    void *sym = dlsym(RTLD_NEXT, name);
    if (sym == NULL) {
        return -ENOSYS;
    }
    memcpy(fn, (const void *)&sym, size);
    return 0;
}

static atomic_long import_calls;
static atomic_long submit_calls;

VmafxStatus vmafx_context_import_frame(VmafxContext *context, VmafxDevice *device,
                                       const VmafxFrameImport *desc, const char *input,
                                       VmafxFrame **out, VmafxError **error)
{
    if (injected(&import_calls, "VMAF_TEST_IMPORT_FAIL_AT", "VMAF_TEST_IMPORT_FAIL_COUNT")) {
        if (out != NULL) {
            *out = NULL;
        }
        return VMAFX_E_BUSY;
    }
    import_frame_fn real = NULL;
    const int err = real_symbol("vmafx_context_import_frame", (void *)&real, sizeof(real));
    return err ? VMAFX_E_DEVICE : real(context, device, desc, input, out, error);
}

VmafxStatus vmafx_submit(VmafxContext *context, VmafxFrame *reference, VmafxFrame *distorted,
                         uint64_t index, VmafxError **error)
{
    if (injected(&submit_calls, "VMAF_TEST_READ_FAIL_AT", "VMAF_TEST_READ_FAIL_COUNT")) {
        if (reference != NULL) {
            (void)vmafx_frame_unref(reference);
        }
        if (distorted != NULL) {
            (void)vmafx_frame_unref(distorted);
        }
        return VMAFX_E_IO;
    }
    submit_fn real = NULL;
    const int err = real_symbol("vmafx_submit", (void *)&real, sizeof(real));
    return err ? VMAFX_E_DEVICE : real(context, reference, distorted, index, error);
}

VmafxStatus vmafx_flush(VmafxContext *context, VmafxError **error)
{
    if (env_long("VMAF_TEST_FLUSH_FAIL") > 0) {
        return VMAFX_E_IO;
    }
    flush_fn real = NULL;
    const int err = real_symbol("vmafx_flush", (void *)&real, sizeof(real));
    return err ? VMAFX_E_DEVICE : real(context, error);
}

/* NOLINTEND(modernize-use-nullptr) */
