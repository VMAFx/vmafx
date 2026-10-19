/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* ---------------------------------------------------------------------------
 * Gate on HAVE_RUST_FEATURES (config.h, ADR-1713): 1 when Meson builds the
 * Rust archive (enable_rust_features=true and cargo found). core/src/meson.build
 * compiles this TU only in that case, and the Rust shim
 * (core/src/rust/shim/rust_twins.cpp) registers vmaf_fex_tad at vmaf_init().
 * Without it the extractor does not exist: `--feature tad` fails with the
 * generic "problem loading feature extractor: tad", and nothing here runs.
 * The `#else` stubs below return -ENOSYS and exist only so a tool that
 * compiles this file without the define (an IDE, clang-tidy) still parses;
 * the shipped build graph never links them.
 * --------------------------------------------------------------------------- */

#if HAVE_RUST_FEATURES

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

#else /* !HAVE_RUST_FEATURES — no-op stubs */

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

#endif /* HAVE_RUST_FEATURES */

/* ---------------------------------------------------------------------------
 * Feature names provided by this extractor.
 * --------------------------------------------------------------------------- */

static const char *tad_provided_features[] = {
    "tad",
    "tad_sad",
    NULL,
};

/* ---------------------------------------------------------------------------
 * Public extractor descriptor — registered in feature_extractor.c.
 * --------------------------------------------------------------------------- */

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
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

/* NOLINTEND(modernize-use-nullptr) */
