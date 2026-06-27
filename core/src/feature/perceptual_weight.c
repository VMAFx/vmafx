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
 * perceptual_weight.c — Pelorus-driven perceptual spatial-pooling weighting
 * (integration plan workstream B2/B4).
 *
 * Parses the per-frame Pelorus side-data blob (vendored interop ABI) and
 * derives a single normalized [0,1] "salience" per frame from the per-cell
 * banding-risk map (and, when present, the variance map). Frames whose spatial
 * regions carry more banding risk get a higher salience, hence a higher pooling
 * weight. When the producer also attaches a PEL_SEC_COMPLEXITY section (ABI
 * 1.3), the salience is additionally ATTENUATED as the per-frame complexity
 * rises — banding is masked in busy / textured frames and most visible in flat
 * / simple ones. The actual pooling re-weight is applied in libvmaf.c's
 * vmaf_feature_score_pooled; this module only stores and serves the weights.
 *
 * Robustness (NASA Power-of-10, CERT C):
 *   - Every blob is validated through the vendored pel_blob_find_section, which
 *     bounds-checks offsets/sizes against the declared length (R4/R5/R6).
 *   - Per-cell map reads are independently bounds-checked here against the blob
 *     image so a lying cell_data_offset/size cannot read out of bounds.
 *   - All loops are bounded by the validated grid dimensions.
 *   - NaN / Inf inputs are clamped; the derived salience is always finite in
 *     [0,1] and the served weight is always finite and positive.
 *   - grid==0 / absent banding section / absent maps degrade to frame-level
 *     scalars or to "no salience" (weight 1.0), never to a crash.
 */

#include "feature/perceptual_weight.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/pelorus/interop.h"
#include "libvmaf/pelorus/pelorus.h"

/* Default per-frame weight strength when the caller never set one explicitly.
 * weight = 1 + strength * salience; strength 1.0 doubles the contribution of a
 * maximally-salient (salience == 1) frame relative to a flat one. */
#define PEL_WEIGHT_DEFAULT_STRENGTH 1.0

/* Clamp a float into [lo, hi], mapping NaN to lo (treat unknown as benign). */
static float clampf(float v, float lo, float hi)
{
    if (isnan(v)) {
        return lo;
    }
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* Locate the start of the flat PelorusSideData image and its readable length,
 * after the 16-byte UUID prefix. Returns 0 and fills *image / *image_len on a
 * valid Pelorus blob; a negative errno otherwise (caller maps to the public
 * contract). Mirrors the framing checks the vendored parser performs so the
 * per-cell map reads below can be bounds-checked. */
/* Copy the flat header out of the blob image by value. The blob is only
 * guaranteed byte-aligned (it arrives as raw side-data bytes), so we memcpy
 * rather than cast the byte pointer to PelorusSideData* — that also sidesteps
 * the bugprone-casting-through-void / cast-align lint families. Caller has
 * already validated image_len >= sizeof(PelorusSideData) via pel_blob_*. */
static void read_header(const uint8_t *image, PelorusSideData *out)
{
    memcpy(out, image, sizeof(*out));
}

static int locate_image(const uint8_t *blob, size_t len, const uint8_t **image, size_t *image_len)
{
    PelorusSideData hdr;

    /* Callers (vmaf_perceptual_weight_ingest) already reject a NULL blob;
     * the out-params are stack locals. Pin those invariants. */
    assert(blob != NULL);
    assert(image != NULL && image_len != NULL);

    if (!pel_blob_is_present(blob, len)) {
        /* Distinguish "not ours" from "ours but wrong ABI major": is_present
         * returns 0 for both, so re-check the UUID to separate them. */
        if (len >= (size_t)PELORUS_SIDEDATA_UUID_LEN &&
            memcmp(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN) == 0) {
            return -EPROTO; /* our UUID, but magic/abi rejected (R6) */
        }
        return -ENOENT; /* a foreign buffer — cleanly ignored */
    }

    *image = blob + PELORUS_SIDEDATA_UUID_LEN;
    *image_len = len - (size_t)PELORUS_SIDEDATA_UUID_LEN;
    read_header(*image, &hdr);

    /* total_size must fit the declared image (defensive; the vendored parser
     * also checks this, but we read maps directly). */
    if ((size_t)hdr.total_size > *image_len) {
        return -EPROTO;
    }
    return 0;
}

/* Mean of a uint8 per-cell risk map (values 0..255 -> [0,1]), bounds-checked.
 * Returns a finite salience in [0,1], or a negative sentinel when the map is
 * absent/out of bounds (caller falls back to the frame-level scalar). */
static double mean_u8_cell_map(const uint8_t *image, size_t image_len, uint32_t offset,
                               uint32_t size, uint32_t n_cells)
{
    uint64_t sum = 0;
    uint32_t i;

    if (n_cells == 0 || offset == 0 || size == 0) {
        return -1.0;
    }
    /* The map must hold at least n_cells bytes and lie inside the image. */
    if ((size_t)offset > image_len || (size_t)size > image_len - (size_t)offset) {
        return -1.0;
    }
    if (size < n_cells) {
        return -1.0;
    }
    for (i = 0; i < n_cells; i++) {
        sum += image[(size_t)offset + i];
    }
    /* sum <= 255 * n_cells; n_cells bounded by grid (uint16*uint16) so no
     * overflow of uint64. */
    return (double)sum / ((double)n_cells * 255.0);
}

/* Mean of a float per-cell variance map, bounds-checked and NaN-guarded. The
 * variance section's per-cell values are already normalized activity in [0,1]
 * by the producer; we clamp defensively. Returns [0,1] or a negative sentinel. */
static double mean_f32_cell_map(const uint8_t *image, size_t image_len, uint32_t offset,
                                uint32_t size, uint32_t n_cells)
{
    double sum = 0.0;
    uint32_t i;

    if (n_cells == 0 || offset == 0 || size == 0) {
        return -1.0;
    }
    if ((size_t)offset > image_len || (size_t)size > image_len - (size_t)offset) {
        return -1.0;
    }
    /* Overflow-safe form of `size < n_cells * sizeof(float)`: on a platform
     * with a 32-bit size_t the product n_cells*4 can wrap (n_cells is up to
     * uint16*uint16 = ~4.29e9), silently passing the check and admitting an
     * out-of-bounds read. The division form cannot overflow (CERT INT30-C) and
     * is exactly equivalent for the reject condition. */
    if ((size_t)n_cells > (size_t)size / sizeof(float)) {
        return -1.0;
    }
    for (i = 0; i < n_cells; i++) {
        float v;
        /* memcpy avoids an unaligned float load; offset+i*4 is in bounds. */
        memcpy(&v, image + (size_t)offset + (size_t)i * sizeof(float), sizeof(float));
        sum += (double)clampf(v, 0.0f, 1.0f);
    }
    return sum / (double)n_cells;
}

/* Banding salience in [0,1], or a negative sentinel when the banding section is
 * absent (caller treats that as "no salience"). Prefers the per-cell risk-map
 * mean (true spatial input); falls back to frame-level scalars when the grid is
 * empty (grid==0 placeholder) or the map is absent/truncated. `blob`/`blob_len`
 * are the UUID-prefixed buffer so pel_blob_find_section can re-validate. */
static double banding_salience(const uint8_t *blob, size_t blob_len, const uint8_t *image,
                               size_t image_len, uint32_t n_cells)
{
    const void *ptr = NULL;
    size_t got = 0;
    PelorusBandingSection b;
    double banding = -1.0;

    if (pel_blob_find_section(blob, blob_len, PEL_SEC_BANDING, sizeof(PelorusBandingSection), &ptr,
                              &got) != PEL_OK ||
        ptr == NULL) {
        return -1.0;
    }

    memset(&b, 0, sizeof(b));
    /* R4: copy only the bytes the producer actually wrote (got <= sizeof). */
    memcpy(&b, ptr, got);

    if (n_cells > 0) {
        banding = mean_u8_cell_map(image, image_len, b.cell_data_offset, b.cell_data_size, n_cells);
    }
    if (banding < 0.0) {
        /* grid==0, or map absent/truncated -> frame-level scalars. Blend the
         * global banding risk with the flat-area fraction (banding shows in
         * flat areas), both already [0,1]; clamp defensively. */
        double risk = (double)clampf(b.global_banding_risk, 0.0f, 1.0f);
        double flat = (double)clampf(b.flat_area_fraction, 0.0f, 1.0f);
        banding = 0.75 * risk + 0.25 * (risk * flat);
    }
    return banding;
}

/* Variance activity in [0,1], or a negative sentinel when the variance section
 * is absent. Per-cell variance-map mean when available, else the frame scalar. */
static double variance_activity(const uint8_t *blob, size_t blob_len, const uint8_t *image,
                                size_t image_len, uint32_t n_cells)
{
    const void *ptr = NULL;
    size_t got = 0;
    PelorusVarianceSection v;
    double activity = -1.0;

    if (pel_blob_find_section(blob, blob_len, PEL_SEC_VARIANCE, sizeof(PelorusVarianceSection),
                              &ptr, &got) != PEL_OK ||
        ptr == NULL) {
        return -1.0;
    }

    memset(&v, 0, sizeof(v));
    memcpy(&v, ptr, got);

    if (n_cells > 0) {
        activity = mean_f32_cell_map(image, image_len, v.var_cell_offset, v.var_cell_size, n_cells);
    }
    if (activity < 0.0) {
        activity = (double)clampf(v.global_variance, 0.0f, 1.0f);
    }
    return activity;
}

/* Maximum fraction of the banding salience that frame complexity may attenuate.
 * The modulation factor is (1 - PEL_COMPLEXITY_ATTEN * complexity), so at
 * complexity == 1 the salience is scaled by (1 - 0.5) = 0.5 and at complexity ==
 * 0 it is unchanged. A flat constant (not a tunable knob) keeps the behaviour
 * predictable and the golden-isolation reasoning simple. */
#define PEL_COMPLEXITY_ATTEN 0.5

/* Hard floor on the modulation factor so even a maximal, out-of-contract
 * complexity can never erase the banding salience entirely (a busy frame still
 * carries SOME perceptual weight). With ATTEN 0.5 and complexity clamped to
 * [0,1] the unclamped factor already bottoms out at 0.5, so this floor is a
 * belt-and-braces guard against a future ATTEN > 1.0. */
#define PEL_COMPLEXITY_FLOOR 0.25

/*
 * Complexity-driven attenuation factor for the banding salience, in
 * [PEL_COMPLEXITY_FLOOR, 1.0]. Returns 1.0 (no modulation, exact legacy
 * behaviour) when the PEL_SEC_COMPLEXITY section is absent or its `complexity`
 * field is non-finite. Rationale: banding / artefacts are MORE perceptually
 * visible in low-complexity (flat / simple) content and MASKED in high-
 * complexity (busy / textured) content, so the banding-salience boost is
 * attenuated as frame complexity rises. The section is per-frame (grid-
 * independent), so this path engages even when grid == 0.
 *
 * `complexity` and `texture_energy` are documented in [0,1]; clampf maps NaN to
 * the benign 0.0 (-> factor 1.0, no modulation) and bounds the field to [0,1].
 */
static double complexity_modulation(const uint8_t *blob, size_t blob_len)
{
    const void *ptr = NULL;
    size_t got = 0;
    PelorusComplexitySection cx;
    double complexity;
    double factor;

    if (pel_blob_find_section(blob, blob_len, PEL_SEC_COMPLEXITY, sizeof(PelorusComplexitySection),
                              &ptr, &got) != PEL_OK ||
        ptr == NULL) {
        return 1.0; /* no complexity section -> no modulation (legacy behaviour) */
    }

    memset(&cx, 0, sizeof(cx));
    /* R4: copy only the bytes the producer actually wrote (got <= sizeof). */
    memcpy(&cx, ptr, got);

    complexity = (double)clampf(cx.complexity, 0.0f, 1.0f);
    factor = 1.0 - PEL_COMPLEXITY_ATTEN * complexity;
    if (isnan(factor) || !isfinite(factor) || factor < PEL_COMPLEXITY_FLOOR) {
        factor = PEL_COMPLEXITY_FLOOR;
    }
    if (factor > 1.0) {
        factor = 1.0;
    }
    return factor;
}

/*
 * Derive a frame salience in [0,1] from a validated Pelorus image. Banding is
 * the primary driver; variance modulates it (flat regions make banding more
 * visible) and frame complexity attenuates it (busy frames mask banding). No
 * banding section -> salience 0 (weight 1.0). Always finite [0,1].
 */
static double derive_salience(const uint8_t *blob, size_t blob_len, const uint8_t *image,
                              size_t image_len)
{
    PelorusSideData hdr;
    uint32_t n_cells;
    double banding;
    double activity;
    double salience;

    /* locate_image has already validated framing and that `image` lies
     * UUID_LEN bytes into `blob`. */
    assert(blob != NULL && image != NULL);
    assert(image == blob + PELORUS_SIDEDATA_UUID_LEN);

    read_header(image, &hdr);
    n_cells = (uint32_t)hdr.grid_cols * (uint32_t)hdr.grid_rows;

    banding = banding_salience(blob, blob_len, image, image_len, n_cells);
    if (banding < 0.0) {
        return 0.0; /* no banding section -> nothing salient to weight on */
    }
    activity = variance_activity(blob, blob_len, image, image_len, n_cells);

    /* Low spatial variance (flat) amplifies banding visibility; high variance
     * (texture) masks it. Modulation factor in [0.75, 1.25] keeps the salience
     * dominated by the banding term while letting variance nudge it. */
    salience = banding;
    if (activity >= 0.0) {
        double flatness = 1.0 - activity;          /* [0,1], 1 == perfectly flat */
        double modulation = 0.75 + 0.5 * flatness; /* [0.75, 1.25] */
        salience *= modulation;
    }

    /* Per-frame complexity attenuation (PEL_SEC_COMPLEXITY, ABI 1.3): busy /
     * textured frames mask banding, so the salience boost is scaled DOWN as the
     * producer's aggregate frame complexity rises. Absent section -> factor 1.0
     * (exact legacy behaviour). This section is grid-independent, so it also
     * modulates the grid==0 frame-level-scalar path above. */
    salience *= complexity_modulation(blob, blob_len);

    if (isnan(salience) || !isfinite(salience)) {
        return 0.0;
    }
    if (salience < 0.0) {
        salience = 0.0;
    }
    if (salience > 1.0) {
        salience = 1.0;
    }
    return salience;
}

/* Grow the summary array so index `pic_index` is addressable; new slots are
 * zero-initialised (present == false -> weight 1.0). */
static int ensure_capacity(VmafPerceptualWeightStore *store, unsigned pic_index)
{
    size_t needed = (size_t)pic_index + 1u;
    size_t new_cap;
    VmafPerceptualFrameSummary *grown;

    assert(store != NULL);

    if (needed <= store->capacity) {
        return 0;
    }

    /* Geometric growth, but never below the requested index. Bound the doubling
     * so a hostile pic_index cannot overflow size_t. */
    new_cap = (store->capacity == 0) ? 16u : store->capacity;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u)) {
            new_cap = needed; /* clamp; one final exact-size growth */
            break;
        }
        new_cap *= 2u;
    }
    if (new_cap < needed) {
        new_cap = needed;
    }
    if (new_cap > SIZE_MAX / sizeof(*grown)) {
        return -ENOMEM;
    }

    grown = realloc(store->summaries, new_cap * sizeof(*grown));
    if (grown == NULL) {
        return -ENOMEM;
    }
    /* Zero the freshly added tail. */
    memset(grown + store->capacity, 0, (new_cap - store->capacity) * sizeof(*grown));
    store->summaries = grown;
    store->capacity = new_cap;
    return 0;
}

void vmaf_perceptual_weight_store_destroy(VmafPerceptualWeightStore *store)
{
    if (store == NULL) {
        return;
    }
    free(store->summaries);
    store->summaries = NULL;
    store->capacity = 0;
}

int vmaf_perceptual_weight_ingest(VmafPerceptualWeightStore *store, const uint8_t *blob, size_t len,
                                  unsigned pic_index)
{
    const uint8_t *image = NULL;
    size_t image_len = 0;
    double salience;
    int err;

    if (store == NULL || blob == NULL) {
        return -EINVAL;
    }

    err = locate_image(blob, len, &image, &image_len);
    if (err != 0) {
        return err; /* -ENOENT (foreign) or -EPROTO (ABI) — frame stays unweighted */
    }

    salience = derive_salience(blob, len, image, image_len);

    err = ensure_capacity(store, pic_index);
    if (err != 0) {
        return err;
    }

    store->summaries[pic_index].present = true;
    store->summaries[pic_index].salience = (float)salience;
    return 0;
}

bool vmaf_perceptual_weight_active(const VmafPerceptualWeightStore *store)
{
    if (store == NULL || !store->enabled || store->summaries == NULL) {
        return false;
    }
    /* Strength 0 makes the weighting an identity even when summaries exist. */
    {
        double strength = store->strength_set ? store->strength : PEL_WEIGHT_DEFAULT_STRENGTH;
        if (!(strength > 0.0)) {
            return false;
        }
    }
    for (size_t i = 0; i < store->capacity; i++) {
        if (store->summaries[i].present) {
            return true;
        }
    }
    return false;
}

double vmaf_perceptual_weight_at_index(const VmafPerceptualWeightStore *store, unsigned pic_index)
{
    double strength;
    double salience;
    double weight;

    /* Golden-gate default: any disabled / empty / absent path returns 1.0. */
    if (store == NULL || !store->enabled || store->summaries == NULL) {
        return 1.0;
    }
    if ((size_t)pic_index >= store->capacity) {
        return 1.0;
    }
    if (!store->summaries[pic_index].present) {
        return 1.0;
    }

    strength = store->strength_set ? store->strength : PEL_WEIGHT_DEFAULT_STRENGTH;
    if (isnan(strength) || !isfinite(strength) || strength < 0.0) {
        return 1.0;
    }

    salience = (double)store->summaries[pic_index].salience;
    if (isnan(salience) || !isfinite(salience) || salience < 0.0) {
        return 1.0;
    }

    weight = 1.0 + strength * salience;
    if (isnan(weight) || !isfinite(weight) || weight <= 0.0) {
        return 1.0;
    }
    /* Post-condition: a served weight is always finite and strictly positive,
     * so the weighted-mean denominator Σwᵢ can never be zero or non-finite. */
    assert(isfinite(weight) && weight > 0.0);
    return weight;
}
