/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * LD_PRELOAD fault injector for the FFmpeg filter tests under
 * ffmpeg-patches/test/, run against an FFmpeg that links libvmaf.so
 * dynamically. Unset variables inject nothing; every call that is not
 * injected goes to libvmaf. Build with -D_GNU_SOURCE (RTLD_NEXT) against the
 * installed libvmaf headers, as the scripts do.
 *
 * vmaf_sycl_import_va_surface() (check-sycl-import-retry.sh): calls number
 *   VMAF_TEST_IMPORT_FAIL_AT .. VMAF_TEST_IMPORT_FAIL_AT + VMAF_TEST_IMPORT_FAIL_COUNT - 1
 *   (1-based, over every call, reference and distorted) return -EIO, which is
 *   what the real call returns when a VA surface cannot be read.
 *
 * vmaf_read_pictures() (check-libvmaf-no-score-after-error.sh): picture calls
 *   number VMAF_TEST_READ_FAIL_AT .. VMAF_TEST_READ_FAIL_AT +
 *   VMAF_TEST_READ_FAIL_COUNT - 1 (1-based, one call per frame, the flush not
 *   counted) return -EIO; VMAF_TEST_FLUSH_FAIL=1 makes the flush call
 *   (ref == NULL) return -EIO.
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <libvmaf/libvmaf.h>
#include <libvmaf/libvmaf_sycl.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's C mode has
 * no `nullptr`. ADR-1138. */

typedef int (*import_fn)(VmafSyclState *state, void *va_display, unsigned int va_surface,
                         int is_ref, unsigned w, unsigned h, unsigned bpc);
typedef int (*read_fn)(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist, unsigned index);

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

/* libvmaf's definition of `name`, copied into `*fn` (`size` bytes); -ENOSYS
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
static atomic_long read_calls;

int vmaf_sycl_import_va_surface(VmafSyclState *state, void *va_display, unsigned int va_surface,
                                int is_ref, unsigned w, unsigned h, unsigned bpc)
{
    if (injected(&import_calls, "VMAF_TEST_IMPORT_FAIL_AT", "VMAF_TEST_IMPORT_FAIL_COUNT")) {
        return -EIO;
    }
    import_fn real = NULL;
    const int err = real_symbol("vmaf_sycl_import_va_surface", (void *)&real, sizeof(real));
    return err ? err : real(state, va_display, va_surface, is_ref, w, h, bpc);
}

int vmaf_read_pictures(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist, unsigned index)
{
    if (ref == NULL) {
        if (env_long("VMAF_TEST_FLUSH_FAIL") > 0) {
            return -EIO;
        }
    } else if (injected(&read_calls, "VMAF_TEST_READ_FAIL_AT", "VMAF_TEST_READ_FAIL_COUNT")) {
        return -EIO;
    }
    read_fn real = NULL;
    const int err = real_symbol("vmaf_read_pictures", (void *)&real, sizeof(real));
    return err ? err : real(vmaf, ref, dist, index);
}

/* NOLINTEND(modernize-use-nullptr) */
