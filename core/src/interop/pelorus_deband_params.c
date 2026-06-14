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

/*
 * VENDORED FROM VMAFx/pelorus@835e097 — DO NOT EDIT. Append-only ABI; single
 * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.
 * See docs/adr/1113-vendor-pelorus-interop-abi.md.
 *
 * Local edit vs the pelorus original: the intra-pelorus #include below is
 * rewritten from "pelorus/deband.h" to "libvmaf/pelorus/deband.h" so it resolves
 * under core/include/. Nothing else is changed.
 */

/*
 * deband_params.c — defaults and validation for the smart-deband parameters.
 * Defaults are the dark-scene pre-encode preset from
 * docs/research/0101-smart-deband.md §11.
 */

#include "libvmaf/pelorus/deband.h"

#include <stddef.h>

void pel_deband_params_default(PelorusDebandParams *p)
{
    int i;

    if (p == NULL) {
        return;
    }
    p->range = 15;
    for (i = 0; i < 4; i++) {
        p->thr[i] = 0.012f;  /* ~1.5x f3kdb default; tune per content       */
        p->grain[i] = 0.0f;
    }
    p->grain[0] = 0.006f;    /* ~1.5 LSB at 8-bit on luma; chroma grain off  */
    p->softness = 0.5f;
    p->detail_thr = 0.06f;
    p->sample_mode = PEL_DEBAND_SAMPLE_SQUARE;
    p->blur_mode = PEL_DEBAND_BLUR_AVERAGE;
    p->dither_mode = PEL_DEBAND_DITHER_BLUENOISE;
    p->planes = 0xFu;
    p->flags = PEL_DEBAND_FLAG_DYNAMIC_GRAIN | PEL_DEBAND_FLAG_PROTECT_DETAIL;
    p->out_depth = 0; /* inherit input depth */
}

static int valid_norm(float v)
{
    return v >= 0.0f && v <= 1.0f;
}

pel_result pel_deband_params_validate(const PelorusDebandParams *p,
                                      const char **what)
{
    int i;
    const char *dummy;

    if (what == NULL) {
        what = &dummy;
    }
    *what = NULL;

    if (p == NULL) {
        *what = "params";
        return PEL_ERR_INVALID;
    }
    if (p->range < 1 || p->range > 31) {
        *what = "range";
        return PEL_ERR_RANGE;
    }
    for (i = 0; i < 4; i++) {
        if (!valid_norm(p->thr[i]) || p->thr[i] > 0.25f) {
            *what = "thr";
            return PEL_ERR_RANGE;
        }
        if (!valid_norm(p->grain[i]) || p->grain[i] > 0.4f) {
            *what = "grain";
            return PEL_ERR_RANGE;
        }
    }
    if (!valid_norm(p->softness)) {
        *what = "softness";
        return PEL_ERR_RANGE;
    }
    if (!valid_norm(p->detail_thr) || p->detail_thr > 0.25f) {
        *what = "detail_thr";
        return PEL_ERR_RANGE;
    }
    if (p->sample_mode < PEL_DEBAND_SAMPLE_COLUMN ||
        p->sample_mode > PEL_DEBAND_SAMPLE_SQUARE_ROT) {
        *what = "sample_mode";
        return PEL_ERR_RANGE;
    }
    if (p->blur_mode < PEL_DEBAND_BLUR_AVERAGE ||
        p->blur_mode > PEL_DEBAND_BLUR_ALLREFS) {
        *what = "blur_mode";
        return PEL_ERR_RANGE;
    }
    if (p->dither_mode < PEL_DEBAND_DITHER_NONE ||
        p->dither_mode > PEL_DEBAND_DITHER_BLUENOISE) {
        *what = "dither_mode";
        return PEL_ERR_RANGE;
    }
    if (p->out_depth != 0 && p->out_depth != 8 && p->out_depth != 10 &&
        p->out_depth != 12 && p->out_depth != 16) {
        *what = "out_depth";
        return PEL_ERR_RANGE;
    }
    return PEL_OK;
}
