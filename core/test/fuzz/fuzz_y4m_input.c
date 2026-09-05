/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * libFuzzer harness for the vendored Daala Y4M parser
 * (libvmaf/tools/y4m_input.c) — exercised through the public
 * `video_input_open` / `video_input_fetch_frame` /
 * `video_input_close` surface.
 *
 * Rationale: the Y4M parser is the only attacker-reachable text
 * parser in the libvmaf tool stack. It runs `sscanf` on header
 * tags (`W`/`H`/`F`/`A`/`I`/`C`), `memcpy`s a tag value of
 * caller-controlled length into a 16-byte scratch buffer
 * (`chroma_type`), and `malloc`s a frame-buffer of size
 * `pic_w * pic_h * <up to 6 bytes/pix>`. All three are classic
 * fuzz-amenable shapes (parse → bounded copy → size-controlled
 * allocate).
 *
 * Strategy: wrap the fuzzer's input bytes as a `FILE *` via
 * POSIX `fmemopen`, drive the public C entry points, and let
 * AddressSanitizer + UBSan flag any heap or arithmetic crash.
 *
 * Build is opt-in via `-Dfuzz=true`; CI nightly runs a 5-minute
 * smoke. See [docs/development/fuzzing.md] and ADR-0270.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vidinput.h"

/* Hard cap on input size: a real Y4M frame can be enormous, but
 * the fuzzer only needs the *header* + one short frame's worth of
 * bytes to exercise the parser's branches. 256 KiB gives the fuzzer
 * enough room to construct a minimal 4:2:0 frame at modest
 * dimensions while still bounding peak RSS independent of
 * libFuzzer's `-rss_limit_mb` setting. (Raised from 64 KiB; at
 * 64 KiB the largest dimension the filter passes — 99999 px wide —
 * was unreachable because even a 1-row 4:2:0 frame needs more bytes
 * than the old cap allowed, so the overflow path was dead code.) */
#define FUZZ_MAX_INPUT_BYTES 262144u

/* The y4m parser computes `dst_buf_sz = pic_w * pic_h * <up to 6>`
 * and `malloc`s it. Without a sanity cap the fuzzer will burn most
 * of its time inside libc allocator paths probing huge sizes the
 * parser blindly accepts (e.g. `W2147483647 H2147483647` parses
 * cleanly via sscanf and overflows the int multiplication). Cap
 * dimensions to a quarter-megapixel-equivalent envelope by
 * pre-screening the header text; anything outside is dropped
 * before `video_input_open` ever sees it. This is a *fuzzer*
 * concern (not a real-world cap) — production callers can pass
 * arbitrary dimensions. Real overflow / oversize bugs reachable
 * through this path remain in scope: we only filter the trivially
 * unbounded inputs that would otherwise mask interesting finds
 * behind allocator timeouts.
 *
 * 5 digits (max "99999") keeps the upper bound consistent with
 * FUZZ_MAX_INPUT_BYTES: a 99999-wide × 1-tall 4:2:0 frame needs
 * ~150 KiB of luma+chroma payload, which fits comfortably inside
 * 256 KiB. The old 6-digit limit ("999999") was dead: 65536 bytes
 * was too small to deliver even a single-row frame at that width,
 * so the overflow-path was unreachable during fuzzing. */
#define FUZZ_MAX_DIM_DIGITS 5 /* up to "99999" — well past 4K, fits in 256 KiB */

static int header_dimensions_in_bounds(const uint8_t *data, size_t size)
{
    /* Best-effort early reject: if the first newline-terminated
     * header line contains a `W<digits>` or `H<digits>` tag with
     * more than FUZZ_MAX_DIM_DIGITS consecutive digits, skip the
     * input. Returns 1 if the input is acceptable to drive into
     * the parser, 0 otherwise. */
    size_t i = 0;
    /* Locate the end of the first line, capped to the y4m header
     * buffer size (256). */
    size_t line_end = size;
    if (line_end > 256u)
        line_end = 256u;
    for (i = 0; i < line_end; i++) {
        if (data[i] == (uint8_t)'\n') {
            line_end = i;
            break;
        }
    }
    for (i = 0; i < line_end; i++) {
        const uint8_t c = data[i];
        if ((c == (uint8_t)'W' || c == (uint8_t)'H') && i + 1 < line_end) {
            size_t digits = 0;
            size_t j = i + 1;
            while (j < line_end && data[j] >= (uint8_t)'0' && data[j] <= (uint8_t)'9') {
                digits++;
                j++;
                if (digits > FUZZ_MAX_DIM_DIGITS)
                    return 0;
            }
        }
    }
    return 1;
}

/* libFuzzer's contract requires `LLVMFuzzerTestOneInput` to have
 * external linkage; the runtime resolves it by name at link time
 * (`-fsanitize=fuzzer`). Cannot be static — the `misc-use-internal-
 * linkage` warning is load-bearing-wrong here. */
/* NOLINTNEXTLINE(misc-use-internal-linkage) — libFuzzer entry-point ABI (ADR-0141 / ADR-0278) */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0u || size > FUZZ_MAX_INPUT_BYTES)
        return 0;
    if (!header_dimensions_in_bounds(data, size))
        return 0;

    /* fmemopen() returns a read-only stream over the supplied
     * buffer. The y4m parser only calls `fread` on it, so the
     * `(void *)` cast (dropping const) is safe. */
    /* NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — fmemopen reads only (ADR-0141 / ADR-0278) */
    FILE *fp = fmemopen((void *)(const void *)data, size, "rb");
    if (fp == NULL)
        return 0;

    video_input vid;
    memset(&vid, 0, sizeof(vid));

    if (video_input_open(&vid, fp) == 0) {
        /* `video_input_open` succeeded — drive one frame fetch
         * to exercise `y4m_input_fetch_frame` and any chroma-
         * conversion convert callback the header selected. The
         * fetch may legitimately fail (truncated frame data);
         * we only care about crash-freedom. */
        video_input_info info;
        memset(&info, 0, sizeof(info));
        video_input_get_info(&vid, &info);

        video_input_ycbcr ycbcr;
        memset(&ycbcr, 0, sizeof(ycbcr));
        char tag[5] = {0};
        (void)video_input_fetch_frame(&vid, ycbcr, tag);

        video_input_close(&vid);
        /* video_input_close calls fclose on the FILE*, so do not
         * fclose again. */
    } else {
        /* Parser rejected the header. Close the FILE* ourselves —
         * `video_input_open` only takes ownership on success. */
        (void)fclose(fp);
    }

    return 0;
}
