/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The Metal side of the shared twin-parity headers, and the case runner of
 * the Metal parity tests (ADR-1496).
 *
 * The shared headers (float_psnr_twin_parity.h, ssim_twin_parity.h, ...) take
 * a backend as three callbacks; metal_twin_open(), metal_twin_import() and
 * metal_twin_close() are Metal's. A missing device makes open() fail, the
 * case reports the skip and the run exits 77.
 *
 * The macOS tester bundle runs these executables on the tester's Mac and
 * reads one line per case from their standard error:
 *
 *     @case <case function> pass|fail|skip
 *
 * (tools/rc1-tester/src/vmaf_rc1_tester/hw_suites.py). metal_run_case() runs
 * a case, prints that line and keeps going after a failure, so one run gives
 * the verdict of every case and the report can say which state rows it
 * measures; the run still fails with the first failure's message.
 *
 * Self-test: built with VMAF_METAL_TWIN_SELFTEST, METAL_TWIN() names the CPU
 * extractor, open() hands out a placeholder state and import() does nothing,
 * so every case compares the CPU extractor with itself on any host. A case
 * whose `==` fails there has a fixture or a key wrong, not a twin; a case that
 * only means something on a device (a twin's own refusal of a frame size)
 * returns early under metal_twin_device_only().
 */

#ifndef LIBVMAF_TEST_METAL_TWIN_H_
#define LIBVMAF_TEST_METAL_TWIN_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

#include "dict.h"
#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#ifdef VMAF_METAL_TWIN_SELFTEST
#define METAL_TWIN(metal_name, cpu_name) (cpu_name)
#define METAL_TWIN_BACKEND "CPU (Metal self-test)"
#else
#define METAL_TWIN(metal_name, cpu_name) (metal_name)
#define METAL_TWIN_BACKEND "Metal"
#endif

#ifdef VMAF_METAL_TWIN_SELFTEST
static int metal_twin_placeholder;

static inline int metal_twin_open(void **state)
{
    *state = &metal_twin_placeholder;
    return 0;
}

static inline int metal_twin_import(VmafContext *vmaf, void *state)
{
    (void)vmaf;
    (void)state;
    return 0;
}

static inline int metal_twin_close(void *state)
{
    (void)state;
    return 0;
}
#else
static inline int metal_twin_open(void **state)
{
    VmafMetalState *metal = NULL;
    /* -1: the system default Metal device (MTLCreateSystemDefaultDevice). */
    const VmafMetalConfiguration cfg = {.device_index = -1, .flags = 0};
    const int err = vmaf_metal_state_init(&metal, cfg);
    *state = metal;
    return err;
}

static inline int metal_twin_import(VmafContext *vmaf, void *state)
{
    return vmaf_metal_import_state(vmaf, (VmafMetalState *)state);
}

static inline int metal_twin_close(void *state)
{
    VmafMetalState *metal = (VmafMetalState *)state;
    vmaf_metal_state_free(&metal);
    return 0;
}
#endif

/* True when a Metal device is there; otherwise reports the skip. For a case
 * that needs no device state of its own (an init() call) but measures a twin:
 * without a device it skips like every other case, so a run on the hosted
 * macOS runner, which has none, measures nothing and fails nothing. */
static inline bool metal_twin_have_device(void)
{
    void *state = NULL;
    if (metal_twin_open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        mu_skipped = 1;
        return false;
    }
    return metal_twin_close(state) == 0;
}

/* Set by metal_twin_device_only() for the case that is running. */
static bool metal_case_device_only;

/* True, after reporting the skip, when a case means something only on a
 * Metal device: the self-test runs the CPU extractor in the twin's place. The
 * case reports `skip`, but the self-test run still exits 0: nothing it could
 * check was left out. */
static inline bool metal_twin_device_only(void)
{
#ifdef VMAF_METAL_TWIN_SELFTEST
    (void)fprintf(stderr, "[self-test: device-only case] ");
    metal_case_device_only = true;
    return true;
#else
    return false;
#endif
}

/* The first failure of the run; metal_run_case() goes on after it. */
static mu_message_t metal_first_failure;

/* One case: the verdict line the tester report reads, the message of a
 * failure, and the run continues. `mu_skipped` is per case here and sticky
 * for the run, so a case without a device still makes the run exit 77. */
static inline void metal_run_case_named(const char *name, mu_message_t (*test)(void))
{
    const int skipped_before = mu_skipped;
    mu_skipped = 0;
    metal_case_device_only = false;
    mu_message_t message = mu_report(name, test);
    const bool skipped = mu_skipped || metal_case_device_only;
    const char *verdict = message ? "fail" : (skipped ? "skip" : "pass");
    (void)fprintf(stderr, "@case %s %s\n", name, verdict);
    if (message) {
        (void)fprintf(stderr, "@message %s %s\n", name, message);
        if (!metal_first_failure) {
            metal_first_failure = message;
        }
    }
    mu_skipped = mu_skipped || skipped_before;
}

#define metal_run_case(test) metal_run_case_named(#test, (test))

/* init() status of extractor `name` on a `w` x `h` 8-bit 4:2:0 frame with one
 * option `key` = `value` (none when `key` is NULL). No picture is allocated:
 * for a frame the twin refuses, init() returns before the twin creates a
 * Metal context or a buffer (T-METAL-UINT-PLANE-INDEX-2026-10-05). */
static inline int metal_twin_init_status(const char *name, const char *key, const char *value,
                                         unsigned w, unsigned h)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(name);
    if (!fex) {
        return -ENOENT;
    }
    VmafDictionary *dict = NULL;
    if (key && vmaf_dictionary_set(&dict, key, value, 0)) {
        (void)vmaf_dictionary_free(&dict);
        return -ENOMEM;
    }
    VmafFeatureExtractorContext *ctx = NULL;
    /* create() takes ownership of `dict`, and frees it on failure. */
    int err = vmaf_feature_extractor_context_create(&ctx, fex, dict);
    if (err) {
        return err;
    }
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    return err;
}

/* The largest picture VMAFx accepts (VMAF_PIC_DIM_MAX, core/src/picture.c). */
#define METAL_TWIN_PIC_DIM_MAX (32768u)

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_TEST_METAL_TWIN_H_ */
