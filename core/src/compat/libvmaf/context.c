/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf context functions on the VMAFx API (ADR-1852 design section 2.11,
 * decision D3; `manual` entries of [[compat]] in core/api/vmafx.toml). Each
 * keeps libvmaf's contract: the same argument checks in the same order, the
 * same ownership (vmaf_use_feature() and vmaf_read_pictures() consume their
 * arguments past those checks) and the same negative errno.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "compat_errno.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/perceptual_weight.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* "%.17g" of a double: every bit survives the text (perceptual strength). */
#define COMPAT_DOUBLE_TEXT 32

int vmaf_feature_dictionary_free(VmafFeatureDictionary **dict)
{
    if (!dict) {
        return -EINVAL;
    }
    vmafx_options_free((VmafxOptions *)*dict);
    *dict = NULL;
    return 0;
}

int vmaf_use_features_from_model(VmafContext *vmaf, VmafModel *model)
{
    if (!vmaf || !model) {
        return -EINVAL;
    }
    VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_context_use_model(context, vmafx_model_from_libvmaf(model), &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_use_features_from_model_collection(VmafContext *vmaf,
                                            VmafModelCollection *model_collection)
{
    if (!vmaf || !model_collection) {
        return -EINVAL;
    }
    VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_context_use_model_set(
        context, vmafx_model_set_from_libvmaf(model_collection), &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

/* libvmaf consumes opts_dict once the context and the extractor name are
 * known good; an unknown extractor (-EINVAL) leaves it with the caller. */
int vmaf_use_feature(VmafContext *vmaf, const char *feature_name, VmafFeatureDictionary *opts_dict)
{
    if (!vmaf || !feature_name) {
        return -EINVAL;
    }
    VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_context_use_feature(context, feature_name, (const VmafxOptions *)opts_dict, &error);
    if (status == VMAFX_E_NOTFOUND) {
        vmafx_error_free(error);
        return -EINVAL;
    }
    vmafx_options_free((VmafxOptions *)opts_dict);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

/* Both pictures, consumed: a frame handle takes each picture's reference and
 * the caller's struct is cleared, as libvmaf's release cleared it. A picture
 * libvmaf cannot read (no reference count) fails the call, and the other one
 * is still consumed, as libvmaf's cleanup consumed it. */
static int take_frames(VmafPicture *ref, VmafPicture *dist, VmafxFrame **rf, VmafxFrame **df)
{
    VmafxError *ref_error = NULL;
    VmafxError *dist_error = NULL;
    const VmafxStatus ref_status = vmafx_frame_from_picture(ref, rf, &ref_error);
    const VmafxStatus dist_status = vmafx_frame_from_picture(dist, df, &dist_error);
    /* Each handle now holds its own reference; drop the pictures'. */
    if (ref_status == VMAFX_OK) {
        vmafx_frame_unref(*rf);
        *ref = (VmafPicture){0};
    }
    if (dist_status == VMAFX_OK) {
        vmafx_frame_unref(*df);
        *dist = (VmafPicture){0};
    }
    if (ref_status == VMAFX_OK && dist_status == VMAFX_OK) {
        return 0;
    }
    vmafx_frame_unref(*rf);
    vmafx_frame_unref(*df);
    *rf = NULL;
    *df = NULL;
    vmafx_error_free(dist_error);
    vmafx_error_free(ref_error);
    return -EINVAL;
}

int vmaf_read_pictures(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist, unsigned index)
{
    if (!vmaf || !ref != !dist) {
        return -EINVAL;
    }
    VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    VmafxError *error = NULL;
    if (!ref) {
        const VmafxStatus status = vmafx_flush(context, &error);
        return status == VMAFX_OK ? 0 : compat_errno(status, error);
    }
    VmafxFrame *rf = NULL;
    VmafxFrame *df = NULL;
    const int err = take_frames(ref, dist, &rf, &df);
    if (err) {
        return err;
    }
    const VmafxStatus status = vmafx_submit(context, rf, df, (uint64_t)index, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_preallocate_pictures(VmafContext *vmaf, VmafPictureConfiguration cfg)
{
    if (!vmaf) {
        return -EINVAL;
    }
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    desc.pix_fmt = (uint32_t)cfg.pic_params.pix_fmt;
    desc.bpc = cfg.pic_params.bpc;
    desc.w = cfg.pic_params.w;
    desc.h = cfg.pic_params.h;
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_context_preallocate(vmafx_context_from_libvmaf(vmaf), &desc, cfg.pic_cnt, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_fetch_preallocated_picture(VmafContext *vmaf, VmafPicture *pic)
{
    if (!vmaf || !pic) {
        return -EINVAL;
    }
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    VmafxStatus status =
        vmafx_context_acquire_frame(vmafx_context_from_libvmaf(vmaf), &frame, &error);
    if (status == VMAFX_OK) {
        status = vmafx_frame_to_picture(frame, pic, &error);
        vmafx_frame_unref(frame);
    }
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_context_get_backend(VmafContext *vmaf, enum VmafBackend *out)
{
    if (!vmaf || !out) {
        return -EINVAL;
    }
    *out = (enum VmafBackend)vmafx_context_backend(vmafx_context_from_libvmaf(vmaf));
    return 0;
}

/* VmafPictureConfiguration as a frame description. */
static VmafxFrameDesc picture_config_desc(const VmafPictureConfiguration *pic_cfg)
{
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    desc.pix_fmt = (uint32_t)pic_cfg->pic_params.pix_fmt;
    desc.bpc = pic_cfg->pic_params.bpc;
    desc.w = pic_cfg->pic_params.w;
    desc.h = pic_cfg->pic_params.h;
    return desc;
}

/* The libvmaf answer of a resolution: the twin, -ENODEV when the context has
 * no device backend (the CPU extractor itself computes the feature), -EINVAL
 * for an unknown or a device extractor, else the engine's errno. */
static int twin_answer(VmafxStatus status, VmafxError *error, const VmafxFeatureResolution *res,
                       const char **twin_name, const char **unsupported_option)
{
    if (status == VMAFX_E_NOTFOUND || status == VMAFX_E_INVALID) {
        vmafx_error_free(error);
        return -EINVAL;
    }
    if (status == VMAFX_OK && res->backend == VMAFX_BACKEND_CPU) {
        return -ENODEV;
    }
    *twin_name = res->extractor;
    if (unsupported_option) {
        *unsupported_option = res->unsupported_option;
    }
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_feature_backend_twin(VmafContext *vmaf, const char *feature_name,
                              const VmafFeatureDictionary *opts_dict,
                              const VmafPictureConfiguration *pic_cfg, const char **twin_name,
                              const char **unsupported_option)
{
    if (unsupported_option) {
        *unsupported_option = NULL;
    }
    if (!vmaf || !feature_name || !twin_name) {
        return -EINVAL;
    }
    *twin_name = NULL;
    const VmafxFrameDesc desc = pic_cfg ? picture_config_desc(pic_cfg) : (VmafxFrameDesc){0};
    VmafxFeatureResolution res = VMAFX_FEATURE_RESOLUTION_INIT;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_feature_resolve(vmafx_context_from_libvmaf(vmaf), feature_name,
                                                     (const VmafxOptions *)opts_dict,
                                                     pic_cfg ? &desc : NULL, &res, &error);
    return twin_answer(status, error, &res, twin_name, unsupported_option);
}

int vmaf_registered_feature_extractor(VmafContext *vmaf, unsigned index, const char **name,
                                      enum VmafBackend *backend)
{
    if (!vmaf || !name || !backend) {
        return -EINVAL;
    }
    VmafxExtractorInfo info = VMAFX_EXTRACTOR_INFO_INIT;
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_context_extractor_info(vmafx_context_from_libvmaf(vmaf), index, &info, &error);
    if (status != VMAFX_OK) {
        return compat_errno(status, error);
    }
    *name = info.name;
    *backend = (enum VmafBackend)info.backend;
    return 0;
}

int vmaf_set_perceptual_weight_enabled(VmafContext *vmaf, int enabled)
{
    if (!vmaf) {
        return -EINVAL;
    }
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_context_set_option(
        vmafx_context_from_libvmaf(vmaf), "perceptual_weight", enabled ? "1" : "0", &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_set_perceptual_weight_strength(VmafContext *vmaf, double strength)
{
    if (!vmaf) {
        return -EINVAL;
    }
    char text[COMPAT_DOUBLE_TEXT];
    const int n = snprintf(text, sizeof(text), "%.17g", strength);
    if (n < 0 || (size_t)n >= sizeof(text)) {
        return -EINVAL;
    }
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_context_set_option(vmafx_context_from_libvmaf(vmaf),
                                                        "perceptual_weight_strength", text, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_set_perceptual_sidedata(VmafContext *vmaf, const uint8_t *blob, size_t len,
                                 unsigned pic_index)
{
    if (!vmaf || !blob) {
        return -EINVAL;
    }
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_context_attach_sidedata(
        vmafx_context_from_libvmaf(vmaf), (uint64_t)pic_index, blob, len, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

/* NOLINTEND(modernize-use-nullptr) */
