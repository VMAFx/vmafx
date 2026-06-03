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

/* Regression test for the heap-buffer-overflow in
 * y4m_convert_411_422jpeg() that was discovered by the libFuzzer harness
 * shipped in PR #348. The 4:1:1 -> 4:2:2-jpeg chroma upsample writes
 * the second sub-pixel of every output position unconditionally in its
 * first two sub-loops. When the destination chroma width dst_c_w == 1
 * (the W=2 / 4:1:1 corner case) only _dst[0] is in-bounds, so the
 * unconditional `_dst[1]` store is a 1-byte heap-buffer-overflow.
 *
 * The third sub-loop of the same routine already carries the
 * `(x << 1 | 1) < dst_c_w` guard. The fix mirrors that guard onto the
 * first two sub-loops. This test drives the parser directly via
 * fmemopen() against an in-memory 4:1:1 W=2 H=4 stream, so it exercises
 * y4m_convert_411_422jpeg without having to round-trip through the full
 * VMAF feature stack (which has its own minimum-dimension constraints).
 *
 * Under AddressSanitizer this test will fault before the assertions
 * ever run if the OOB regresses; under a non-sanitizer build it will
 * simply pass on a fixed parser and may pass-and-corrupt-heap on a
 * broken one. ASan in CI is the load-bearing gate.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>

#include "test.h"
#include "vidinput.h"

/* The minimal 4:1:1 W=2 H=4 stream the fuzzer parked in PR #348's
 * known-crashes corpus. 26-byte header + "FRAME\n" tag + one frame:
 *   luma   = 2 * 4 = 8 bytes
 *   chroma = c_w * c_h * 2 planes = 1 * 4 * 2 = 8 bytes
 * Header geometry: W2 H4 F30:1 Ip C411 (4:1:1 with src_c_dec_h = 4
 * gives c_w = (2 + 4 - 1) / 4 = 1; the 4:2:2-jpeg destination has
 * dst_c_dec_h = 2 so dst_c_w = (2 + 2 - 1) / 2 = 1). */
static const unsigned char kY4m411W2H4[] = {
    /* "YUV4MPEG2 W2 H4 F30:1 Ip C411\n" */
    'Y',
    'U',
    'V',
    '4',
    'M',
    'P',
    'E',
    'G',
    '2',
    ' ',
    'W',
    '2',
    ' ',
    'H',
    '4',
    ' ',
    'F',
    '3',
    '0',
    ':',
    '1',
    ' ',
    'I',
    'p',
    ' ',
    'C',
    '4',
    '1',
    '1',
    '\n',
    /* "FRAME\n" */
    'F',
    'R',
    'A',
    'M',
    'E',
    '\n',
    /* luma 2 * 4 = 8 */
    0x10,
    0x20,
    0x30,
    0x40,
    0x50,
    0x60,
    0x70,
    0x80,
    /* Cb plane 1 * 4 = 4 */
    0x90,
    0xA0,
    0xB0,
    0xC0,
    /* Cr plane 1 * 4 = 4 */
    0xD0,
    0xE0,
    0xF0,
    0x11,
};

static char *test_y4m_411_w2_h4_no_oob(void)
{
    FILE *fin = fmemopen((void *)kY4m411W2H4, sizeof(kY4m411W2H4), "rb");
    mu_assert("fmemopen failed", fin != NULL);

    video_input vid;
    int err = video_input_open(&vid, fin);
    mu_assert("video_input_open rejected the W=2 H=4 4:1:1 stream", err == 0);

    video_input_info info;
    video_input_get_info(&vid, &info);
    mu_assert("expected pic_w == 2", info.pic_w == 2);
    mu_assert("expected pic_h == 4", info.pic_h == 4);

    /* The 4:1:1 -> 4:2:2-jpeg conversion runs inside fetch_frame and
     * writes into y4m->dst_buf. ASan flags the 1-byte OOB on the
     * unfixed parser; on the fixed parser the call returns success. */
    video_input_ycbcr ycbcr;
    char tag[5];
    int fr = video_input_fetch_frame(&vid, ycbcr, tag);
    mu_assert("video_input_fetch_frame failed on W=2 H=4 4:1:1 frame", fr > 0);

    video_input_close(&vid);
    /* fmemopen-backed FILE is closed by video_input_close via fclose. */
    return NULL;
}

int mu_tests_run;

char *run_tests(void)
{
    mu_run_test(test_y4m_411_w2_h4_no_oob);
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
