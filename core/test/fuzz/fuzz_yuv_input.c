/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * libFuzzer harness for the vendored raw-YUV reader
 * (libvmaf/tools/yuv_input.c) — exercised through the public
 * `raw_input_open` / `video_input_fetch_frame` /
 * `video_input_close` surface.
 *
 * Rationale: complementing fuzz_y4m_input (ADR-0270), this harness
 * exercises the headerless raw-YUV path. Where Y4M input is parsed
 * (sscanf + dimension-derived malloc), raw YUV is *unparsed*: the
 * caller supplies (width, height, pix_fmt, bitdepth) and the reader
 * fread()s `dst_buf_sz = width * height * <up to 6>` bytes into a
 * malloc'd buffer. The interesting fuzz surface is therefore the
 * frame body (truncated reads, padding, chroma-subsampling
 * arithmetic in `yuv_input_fetch_frame`) rather than a header
 * parse — the parameters are picked at compile time and the fuzzer
 * varies only the bytes the reader sees.
 *
 * Strategy: wrap fuzzer bytes as a `FILE *` via POSIX `fmemopen`,
 * drive `raw_input_open` against a fixed (small) resolution, fetch
 * up to 8 frames, and let AddressSanitizer + UBSan flag any heap
 * or arithmetic crash.
 *
 * Build is opt-in via `-Dfuzz=true`; CI nightly runs a 60-second
 * smoke per harness. See [docs/development/fuzzing.md] and
 * ADR-0311 (extends ADR-0270).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "vidinput.h"

/* Hard cap on input size: the raw-YUV reader will fread the full
 * `dst_buf_sz` per frame; cap at 256 KiB so the fuzzer can drive
 * up to 8 fetches against the chosen 32x32 / 8-bit / 4:2:0
 * envelope (1.5 KiB/frame) without burning iterations on
 * large-buffer copies. */
#define FUZZ_MAX_INPUT_BYTES 262144u

/* Fuzzer parameter envelope. The dimensions are deliberately
 * tiny (32x32) so each iteration is cheap; the reader's chroma-
 * subsampling arithmetic still exercises every branch the
 * production-size path does. The mode rotates per-input over
 * 8/10-bit and 420/422/444 to cover all six (depth, fmt) combos
 * exposed by `pix_fmt_map`. */
#define FUZZ_W 32u
#define FUZZ_H 32u
#define FUZZ_MAX_FRAMES 8u

/* libFuzzer's contract requires `LLVMFuzzerTestOneInput` to have
 * external linkage; the runtime resolves it by name at link time
 * (`-fsanitize=fuzzer`). Cannot be static — the
 * `misc-use-internal-linkage` warning is load-bearing-wrong. */
/* NOLINTNEXTLINE(misc-use-internal-linkage) — libFuzzer entry-point ABI */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0u || size > FUZZ_MAX_INPUT_BYTES)
        return 0;

    /* Pick a (pix_fmt, bitdepth) deterministic from the first
     * input byte so libFuzzer's coverage feedback can drive the
     * mode selection. Six combinations are exposed; collapse the
     * other byte values onto them via modulo. */
    static const struct {
        enum VmafPixelFormat pix_fmt;
        unsigned bitdepth;
    } modes[6] = {
        {VMAF_PIX_FMT_YUV420P, 8u},  {VMAF_PIX_FMT_YUV422P, 8u},  {VMAF_PIX_FMT_YUV444P, 8u},
        {VMAF_PIX_FMT_YUV420P, 10u}, {VMAF_PIX_FMT_YUV422P, 10u}, {VMAF_PIX_FMT_YUV444P, 10u},
    };
    const unsigned mode_idx = (unsigned)(data[0]) % 6u;
    const enum VmafPixelFormat pix_fmt = modes[mode_idx].pix_fmt;
    const unsigned bitdepth = modes[mode_idx].bitdepth;

    /* fmemopen() returns a read-only stream over the supplied
     * buffer. The yuv reader only calls `fread` on it, so the
     * `(void *)` cast (dropping const) is safe. */
    /* NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — fmemopen reads only */
    FILE *fp = fmemopen((void *)(const void *)data, size, "rb");
    if (fp == NULL)
        return 0;

    video_input vid;
    memset(&vid, 0, sizeof(vid));

    /* `raw_input_open` returns 0 on success and takes ownership of
     * `fp` via `video_input_close`'s `fclose`. */
    if (raw_input_open(&vid, fp, FUZZ_W, FUZZ_H, (int)pix_fmt, bitdepth) == 0) {
        video_input_info info;
        memset(&info, 0, sizeof(info));
        video_input_get_info(&vid, &info);

        for (unsigned i = 0u; i < FUZZ_MAX_FRAMES; i++) {
            video_input_ycbcr ycbcr;
            memset(&ycbcr, 0, sizeof(ycbcr));
            char tag[5] = {0};
            int rc = video_input_fetch_frame(&vid, ycbcr, tag);
            /* rc == 0 is end-of-file, rc < 0 is a parser error;
             * either way the next fetch will repeat the same
             * branch and waste fuzzer time. Bail early. */
            if (rc <= 0)
                break;
        }

        video_input_close(&vid);
        /* video_input_close calls fclose on the FILE*, so do not
         * fclose again. */
    } else {
        /* Open failed (unsupported pix_fmt enum, malloc failure).
         * Close the FILE* ourselves — `raw_input_open` only takes
         * ownership on success. */
        (void)fclose(fp);
    }

    return 0;
}
