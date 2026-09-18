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
 * VENDORED FROM VMAFx/pelorus@818d844 — DO NOT EDIT. Append-only ABI; single
 * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.
 * See docs/adr/1113-vendor-pelorus-interop-abi.md.
 *
 * Local edit vs the pelorus original: the intra-pelorus #include below is
 * rewritten from "pelorus/denoise.h" to "libvmaf/pelorus/denoise.h" so it resolves
 * under core/include/. Nothing else is changed.
 */

/*
 * denoise_params.c — defaults and validation for the temporal denoise params.
 * Defaults are the conservative pre-encode preset: a safe floor the vmafx
 * autotune loop (ADR-0106) sweeps strength/sigma upward from. See denoise.h.
 */

#include "libvmaf/pelorus/denoise.h"

#include <stddef.h>

void pel_denoise_params_default(PelorusDenoiseParams *p)
{
    int i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < 4; i++) {
        p->sigma_s[i] = 0.03f;  /* luma; chroma raised below              */
        p->sigma_t[i] = 0.05f;  /* temporal gate bandwidth                */
        p->strength[i] = 0.30f; /* luma dry/wet; chroma lowered below     */
    }
    p->sigma_s[1] = p->sigma_s[2] = 0.04f; /* chroma is noisier           */
    p->strength[1] = p->strength[2] = 0.20f;
    p->blend = 0.6f;          /* temporal-leaning                          */
    p->temporal_decay = 0.8f; /* older frames trusted less                 */
    p->temporal_cut = 0.10f;  /* break the temporal walk past this delta   */
    p->patch_radius = 1;      /* 3x3 spatial window                        */
    p->n_prev = 3;            /* 3-frame causal temporal window            */
    p->denoiser_id = PEL_DENOISER_BILATERAL_TEMPORAL;
    p->planes = 0xFu;
    p->flags = PEL_DENOISE_FLAG_TEMPORAL | PEL_DENOISE_FLAG_PROTECT_DETAIL;
}

static int valid_norm(float v)
{
    return v >= 0.0f && v <= 1.0f;
}

pel_result pel_denoise_params_validate(const PelorusDenoiseParams *p, const char **what)
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
    for (i = 0; i < 4; i++) {
        if (!valid_norm(p->sigma_s[i]) || p->sigma_s[i] > 0.5f) {
            *what = "sigma_s";
            return PEL_ERR_RANGE;
        }
        if (!valid_norm(p->sigma_t[i]) || p->sigma_t[i] > 0.5f) {
            *what = "sigma_t";
            return PEL_ERR_RANGE;
        }
        if (!valid_norm(p->strength[i])) {
            *what = "strength";
            return PEL_ERR_RANGE;
        }
    }
    if (!valid_norm(p->blend)) {
        *what = "blend";
        return PEL_ERR_RANGE;
    }
    if (!valid_norm(p->temporal_decay)) {
        *what = "temporal_decay";
        return PEL_ERR_RANGE;
    }
    if (!valid_norm(p->temporal_cut) || p->temporal_cut > 0.5f) {
        *what = "temporal_cut";
        return PEL_ERR_RANGE;
    }
    if (p->patch_radius < 0 || p->patch_radius > 3) {
        *what = "patch_radius";
        return PEL_ERR_RANGE;
    }
    if (p->n_prev < 0 || p->n_prev > PEL_DENOISE_MAX_PREV) {
        *what = "n_prev";
        return PEL_ERR_RANGE;
    }
    if (p->denoiser_id != PEL_DENOISER_NONE && p->denoiser_id != PEL_DENOISER_BILATERAL_TEMPORAL) {
        *what = "denoiser_id";
        return PEL_ERR_RANGE;
    }
    return PEL_OK;
}
