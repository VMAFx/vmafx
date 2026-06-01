/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/* Regression test for the y4m_input_open_impl malloc-NULL-not-checked bug.
 *
 * The vendored Daala y4m parser allocated dst_buf / aux_buf via
 *
 *      _y4m->dst_buf = (unsigned char *)malloc(_y4m->dst_buf_sz);
 *      _y4m->aux_buf = _y4m->aux_buf_sz ? malloc(_y4m->aux_buf_sz) : NULL;
 *      return 0;
 *
 * — returning success even when one or both malloc()s returned NULL.  The
 * caller (y4m_input_open → video_input_open) then surfaced the partially-
 * initialised reader as a valid video_input.  At the very next
 * video_input_fetch_frame, the parser called fread(_y4m->dst_buf, …) with
 * dst_buf == NULL, faulting inside libc with a SIGSEGV.
 *
 * This test drives the failure path by RLIMIT_AS-restricting the process
 * address space below the buffer size demanded by an absurd (but parser-
 * accepted) y4m header (W = H = 65535, 4:4:4, 12-bit ≈ 24 GiB).  Under
 * the fix, video_input_open returns -1 cleanly.  Pre-fix the same call
 * returned 0 and the subsequent fetch_frame would SIGSEGV.
 *
 * The test is POSIX-only (fmemopen + setrlimit), mirrors the
 * test_y4m_411_oob gating.
 */

/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — POSIX feature-test macro */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "test.h"
#include "vidinput.h"

/* "YUV4MPEG2 W65535 H65535 F30:1 Ip C444p12\n" + "FRAME\n"
 * Parser accepts (no upper-bound check on W/H); dst_buf_sz computes to
 * ≈ 25 GiB which is well beyond the RLIMIT_AS we set below.  No frame
 * payload is needed — the test exercises the open path only. */
static const char kY4mAbsurdHeader[] = "YUV4MPEG2 W65535 H65535 F30:1 Ip C444p12\nFRAME\n";

/* AddressSanitizer / ThreadSanitizer / MemorySanitizer reserve a multi-TB
 * shadow virtual region on process startup. Setting RLIMIT_AS=256 MiB after
 * that causes every subsequent ASan internal mmap to fail with "Failed to
 * mmap" — the test process aborts before the y4m parser even runs. The
 * regression this test guards (dst_buf-NULL bug) is C-level, not sanitizer-
 * surfaceable, so a clean skip under sanitizers preserves coverage on the
 * non-sanitised legs without false positives on the ASan leg. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define VMAF_TEST_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define VMAF_TEST_SANITIZER_BUILD 1
#endif
#endif
#ifndef VMAF_TEST_SANITIZER_BUILD
#define VMAF_TEST_SANITIZER_BUILD 0
#endif

static char *test_y4m_open_returns_error_on_oom(void)
{
#if VMAF_TEST_SANITIZER_BUILD
    /* See comment above: setrlimit(RLIMIT_AS) is incompatible with sanitizer
     * shadow reservations. The pre-fix bug this test guards is not sanitizer-
     * detectable, so skipping under sanitizers gives up no coverage. */
    (void)fprintf(stderr, "(sanitizer build; skipping RLIMIT_AS test) ");
    return NULL;
#endif

    /* Cap virtual address space at 256 MiB.  The header demands ≈ 25 GiB
     * which the y4m parser will try to malloc — must fail under this cap.
     * Skip the test (passing) if setrlimit isn't permitted (some sandboxed
     * CI environments). */
    struct rlimit lim = {.rlim_cur = (rlim_t)(256ULL * 1024ULL * 1024ULL),
                         .rlim_max = (rlim_t)(256ULL * 1024ULL * 1024ULL)};
    if (setrlimit(RLIMIT_AS, &lim) != 0) {
        /* Sandbox / container blocks RLIMIT_AS — treat as a skip. */
        (void)fprintf(stderr, "(setrlimit denied; skipping) ");
        return NULL;
    }

    FILE *fin = fmemopen((void *)kY4mAbsurdHeader, sizeof(kY4mAbsurdHeader) - 1U, "rb");
    mu_assert("fmemopen failed", fin != NULL);

    video_input vid;
    memset(&vid, 0, sizeof(vid));
    /* Pre-fix: video_input_open returned 0 because y4m_input_open_impl
     * ignored the failed malloc and returned 0; the very next
     * video_input_fetch_frame would dereference dst_buf == NULL.
     * Post-fix: y4m_input_open_impl returns -1, y4m_input_open returns
     * NULL, video_input_open returns -1. */
    int rc = video_input_open(&vid, fin);
    mu_assert("y4m_input_open_impl ignored a failed malloc and reported success "
              "— regression of the dst_buf-NULL bug",
              rc != 0);

    /* fmemopen-backed FILE: close manually since video_input_open failed. */
    (void)fclose(fin);
    return NULL;
}

int mu_tests_run;

char *run_tests(void)
{
    mu_run_test(test_y4m_open_returns_error_on_oom);
    return NULL;
}

int main(void)
{
    char *msg = run_tests();
    if (msg) {
        (void)fprintf(stderr, "\033[31m%s\n%d tests run, 1 failed\033[0m\n", msg, mu_tests_run);
    } else {
        (void)fprintf(stderr, "\033[32m%d tests run, %d passed\033[0m\n", mu_tests_run,
                      mu_tests_run);
    }
    return msg != NULL;
}
