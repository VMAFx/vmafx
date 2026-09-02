/**
 * Copyright 2026 Lusoris
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
 * tad_rust.c — C wrapper that registers the Rust TAD extractor as a
 * VmafFeatureExtractor (ADR-0707 cbindgen pilot).
 *
 * The Rust crate (core/src/feature/rust/tad) is compiled to a staticlib
 * (libvmafx_tad.a) by a Meson custom_target.  This file declares the three
 * Rust-exported symbols (vmafx_tad_init / _extract / _close) and wires them
 * into the VmafFeatureExtractor lifecycle callbacks.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "feature_collector.h"
#include "feature_extractor.h"
#include "log.h"
#include "picture.h"

/* ---------------------------------------------------------------------------
 * Gate on HAVE_RUST_TAD: defined by Meson when the Rust staticlib is linked.
 * When the define is absent (enable_rust_features=false or cargo not found),
 * the extern references to vmafx_tad_* are omitted entirely so the link
 * succeeds without the archive. The extractor struct then contains no-op
 * stubs that return -ENOSYS, and vmaf_fex_tad is still registered so that
 * --feature tad produces a clear "not available" error rather than an unknown-
 * feature error.
 * --------------------------------------------------------------------------- */

#ifdef HAVE_RUST_TAD

/* Declarations of the Rust-exported functions (generated header lives at
 * $OUT_DIR/include/vmafx_tad.h; the Meson rule copies it to the build tree).
 * We re-declare the signatures here directly to avoid a generated-file
 * dependency in the source tree — the signatures are trivial and stable. */

extern int vmafx_tad_init(void **state_out, unsigned int bpc);
extern int vmafx_tad_extract(void *state_ptr, const VmafPicture *ref_pic,
                             const VmafPicture *dis_pic, unsigned int index,
                             VmafFeatureCollector *feature_collector);
extern int vmafx_tad_close(void *state_ptr);

/* ---------------------------------------------------------------------------
 * VmafFeatureExtractor lifecycle callbacks — thin shims into Rust.
 * --------------------------------------------------------------------------- */

static int tad_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                    unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)w;
    (void)h;
    return vmafx_tad_init(&fex->priv, bpc);
}

static int tad_extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                       VmafPicture *dis_pic, VmafPicture *dis_pic_90, unsigned index,
                       VmafFeatureCollector *feature_collector)
{
    /* TAD operates on the un-rotated luma plane only. */
    (void)ref_pic_90;
    (void)dis_pic_90;
    return vmafx_tad_extract(fex->priv, ref_pic, dis_pic, index, feature_collector);
}

static int tad_close(VmafFeatureExtractor *fex)
{
    return vmafx_tad_close(fex->priv);
}

#else /* !HAVE_RUST_TAD — no-op stubs */

static int tad_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                    unsigned w, unsigned h)
{
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    (void)w;
    (void)h;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "tad: feature extractor disabled; rebuild libvmaf with -Denable_rust_features=true\n");
    return -ENOSYS;
}

static int tad_extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                       VmafPicture *dis_pic, VmafPicture *dis_pic_90, unsigned index,
                       VmafFeatureCollector *feature_collector)
{
    (void)fex;
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dis_pic;
    (void)dis_pic_90;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
}

static int tad_close(VmafFeatureExtractor *fex)
{
    (void)fex;
    return 0;
}

#endif /* HAVE_RUST_TAD */

/* ---------------------------------------------------------------------------
 * Feature names provided by this extractor.
 * --------------------------------------------------------------------------- */

static const char *const tad_provided_features[] = {
    "tad",
    "tad_sad",
    NULL,
};

/* ---------------------------------------------------------------------------
 * Public extractor descriptor — registered in feature_extractor.c.
 * --------------------------------------------------------------------------- */

VmafFeatureExtractor vmaf_fex_tad = {
    .name = "tad",
    .init = tad_init,
    .extract = tad_extract,
    .close = tad_close,
    .provided_features = tad_provided_features,
    /* TAD has no per-frame state beyond what Rust manages: no TEMPORAL flag,
     * no GPU flags.  The extractor is pure CPU, sequential per-frame. */
    .flags = 0,
    .priv_size = 0, /* Rust allocates state internally; priv_size is unused. */
    .chars = {0},
};
