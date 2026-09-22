/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Shared scaffold-skip teardown for the HIP parity tests.
 *
 * Every HIP parity test drives the same sequence -- initialise a HIP state,
 * import it into a VmafContext, register the HIP extractor, feed frames, read
 * the score back -- and every one of those steps can report the documented
 * scaffold contract: a HIP extractor that is not built yet (enable_hipcc =
 * false, see the extractors under core/src/feature/hip/) returns -ENOSYS.
 * That is a not-built-yet signal rather than a regression, so the run is torn
 * down and reported as a skip instead of a failure.
 *
 * Holding one definition of that teardown keeps the parity tests in step with
 * each other and keeps every run_hip_*() body inside the 60-line function
 * budget the repository applies to the whole tree (ADR-1142).
 */

#ifndef LIBVMAF_TEST_HIP_PARITY_SKIP_H_
#define LIBVMAF_TEST_HIP_PARITY_SKIP_H_

#include <stdio.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"

/*
 * Close `vmaf`, release `*hip_state`, record the skip in `*skipped`, and
 * report it on stderr. `where` names the step that returned -ENOSYS and is
 * appended to the message verbatim: "" for extractor registration,
 * " on feed", " on EOS", " on submit".
 *
 * Always returns NULL, so a call site reads
 *
 *     if (err == -ENOSYS)
 *         return hip_parity_skip(vmaf, &hip_state, skipped, " on feed");
 */
static inline mu_message_t hip_parity_skip(VmafContext *vmaf, VmafHipState **hip_state,
                                           int *skipped, const char *where)
{
    (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS%s] ", where);
    *skipped = 1;
    (void)vmaf_close(vmaf);
    vmaf_hip_state_free(hip_state);
    return NULL;
}

#endif /* LIBVMAF_TEST_HIP_PARITY_SKIP_H_ */
