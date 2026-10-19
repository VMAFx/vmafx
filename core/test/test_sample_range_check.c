/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The opt-in sample range check of vmaf_read_pictures() (ADR-1918,
 * T-OUT-OF-RANGE-SAMPLES-TWIN-DIVERGENCE-2026-10-05).
 *
 *   - off (the default): a 10-bit picture with a sample of 1024 is read as
 *     before;
 *   - on: the same call returns -EINVAL, for a sample in the reference or the
 *     distorted picture, in any plane, at the last row and column too; an
 *     in-range picture, and 8- and 16-bit pictures, are read;
 *   - the setter needs a context.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define W 48u
#define H 32u

/* One sample to write into a picture: `value` at (plane, row, column). */
typedef struct Poke {
    unsigned p;
    unsigned y;
    unsigned x;
    unsigned value;
} Poke;

/* One row of mid-grey samples of a `bpc`-bit plane. */
static void fill_row(uint8_t *row, unsigned w, unsigned bpc)
{
    for (unsigned x = 0u; x < w; x++) {
        if (bpc == 8u) {
            row[x] = 128u;
        } else {
            ((uint16_t *)row)[x] = (uint16_t)(1u << (bpc - 1u));
        }
    }
}

/* A `bpc`-bit 4:2:0 picture of mid-grey, with `poke` (when not NULL) written. */
static int make_picture(VmafPicture *pic, unsigned bpc, const Poke *poke)
{
    if (vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, W, H)) {
        return -ENOMEM;
    }
    for (unsigned p = 0u; p < 3u; p++) {
        for (unsigned y = 0u; y < pic->h[p]; y++) {
            fill_row((uint8_t *)pic->data[p] + ((size_t)y * (size_t)pic->stride[p]), pic->w[p],
                     bpc);
        }
    }
    if (poke && bpc > 8u) {
        uint8_t *row =
            (uint8_t *)pic->data[poke->p] + ((size_t)poke->y * (size_t)pic->stride[poke->p]);
        ((uint16_t *)row)[poke->x] = (uint16_t)poke->value;
    }
    return 0;
}

/* vmaf_read_pictures() of one picture pair under the check, `enabled` or not;
 * `ref_poke` / `dist_poke` change one sample of either. Returns its status,
 * or a positive value when the setup failed. */
static int read_pair(bool enabled, unsigned bpc, const Poke *ref_poke, const Poke *dist_poke)
{
    VmafContext *vmaf = NULL;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    if (vmaf_init(&vmaf, cfg) || vmaf_use_feature(vmaf, "psnr", NULL) ||
        vmaf_set_sample_range_check_enabled(vmaf, enabled ? 1 : 0)) {
        if (vmaf) {
            (void)vmaf_close(vmaf);
        }
        return 1;
    }
    VmafPicture ref;
    VmafPicture dist;
    if (make_picture(&ref, bpc, ref_poke)) {
        (void)vmaf_close(vmaf);
        return 1;
    }
    if (make_picture(&dist, bpc, dist_poke)) {
        (void)vmaf_picture_unref(&ref);
        (void)vmaf_close(vmaf);
        return 1;
    }
    /* The context owns both pictures whatever this returns. */
    const int err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    const int flush = err ? 0 : vmaf_read_pictures(vmaf, NULL, NULL, 0u);
    const int close = vmaf_close(vmaf);
    return err ? err : (flush || close) ? 1 : 0;
}

static char *test_off_by_default_reads_out_of_range(void)
{
    const Poke bad = {0u, 3u, 5u, 1024u};
    mu_assert("an out-of-range sample was refused with the check off",
              read_pair(false, 10u, &bad, NULL) == 0);
    return NULL;
}

static char *test_on_refuses_every_plane_and_both_pictures(void)
{
    const Poke cases[] = {
        {0u, 0u, 0u, 1024u},
        {0u, H - 1u, W - 1u, 1024u},
        {1u, 2u, 3u, 65535u},
        {2u, (H / 2u) - 1u, (W / 2u) - 1u, 1025u},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mu_assert("an out-of-range reference sample was read with the check on",
                  read_pair(true, 10u, &cases[i], NULL) == -EINVAL);
        mu_assert("an out-of-range distorted sample was read with the check on",
                  read_pair(true, 10u, NULL, &cases[i]) == -EINVAL);
    }
    const Poke over12 = {0u, 1u, 1u, 4096u};
    mu_assert("a 12-bit sample of 4096 was read with the check on",
              read_pair(true, 12u, &over12, NULL) == -EINVAL);
    return NULL;
}

static char *test_on_reads_in_range_pictures(void)
{
    const Poke top10 = {0u, 1u, 1u, 1023u};
    mu_assert("a 10-bit sample of 1023 was refused", read_pair(true, 10u, &top10, &top10) == 0);
    mu_assert("an 8-bit picture was refused", read_pair(true, 8u, NULL, NULL) == 0);
    const Poke top16 = {0u, 1u, 1u, 65535u};
    mu_assert("a 16-bit sample of 65535 was refused", read_pair(true, 16u, &top16, NULL) == 0);
    return NULL;
}

static char *test_the_setter_needs_a_context(void)
{
    mu_assert("the setter accepted a missing context",
              vmaf_set_sample_range_check_enabled(NULL, 1) == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_off_by_default_reads_out_of_range);
    mu_run_test(test_on_refuses_every_plane_and_both_pictures);
    mu_run_test(test_on_reads_in_range_pictures);
    mu_run_test(test_the_setter_needs_a_context);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
