/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
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

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"

#include "vif.h"
#include "vif_options.h"
#include "vif_tools.h"
#include "picture_copy.h"

/* Default minimum value allowed for the feature */
#define DEFAULT_VIF_MIN_VAL (0.0)
#define VIF_MAX(x, y) ((x) > (y) ? (x) : (y))

/* Number of float-plane-sized scratch buffers required by compute_vif. */
#define VIF_SCRATCH_BUF_CNT 10

typedef struct VifState {
    size_t float_stride;
    size_t scaled_float_stride;
    size_t scaled_w;
    size_t scaled_h;
    float *ref;
    float *ref_scaled;
    float *dist;
    float *dist_scaled;
    /*
     * vif_buf: single allocation of VIF_SCRATCH_BUF_CNT scratch planes,
     * each ALIGN_CEIL(scaled_w * sizeof(float)) * scaled_h bytes.
     * Allocated once in init(), freed in close(), reused every frame.
     * Per-frame heap traffic on this path is zero.
     */
    float *vif_buf;
    bool debug;
    double vif_sigma_nsq;
    bool vif_skip_scale0;
    double vif_enhn_gain_limit;
    double vif_kernelscale;
    double vif_prescale;
    double vif_scale1_min_val;
    double vif_scale2_min_val;
    double vif_scale3_min_val;
    char *vif_prescale_method;
    VmafDictionary *feature_name_dict;
    /*
     * Pre-computed Gaussian filters for all 4 VIF scales (ADR-0500 Win #3).
     * Computed once in init() from vif_kernelscale, eliminating 4×
     * vif_get_filter() transcendental calls per frame.
     */
    float filter_cache[4][128];
    int filter_width_cache[4];
} VifState;

#define VIF_BOOL_OPTION(name_, alias_, help_, member_, flags_)                                     \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(VifState, member_),                                                     \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = false,                                                                    \
        .flags = flags_,                                                                           \
    }
#define VIF_DOUBLE_OPTION(name_, alias_, help_, member_, default_, min_, max_)                     \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(VifState, member_),                                                     \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = default_,                                                                 \
        .min = min_,                                                                               \
        .max = max_,                                                                               \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define VIF_STRING_OPTION(name_, alias_, help_, member_, default_)                                 \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(VifState, member_),                                                     \
        .type = VMAF_OPT_TYPE_STRING,                                                              \
        .default_val.s = default_,                                                                 \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }

static const VmafOption options[] = {
    VIF_BOOL_OPTION("debug", NULL, "debug mode: enable additional output", debug, 0),
    VIF_DOUBLE_OPTION("vif_enhn_gain_limit", "egl",
                      "enhancement gain imposed on vif, must be >= 1.0, where 1.0 means the gain "
                      "is completely disabled",
                      vif_enhn_gain_limit, DEFAULT_VIF_ENHN_GAIN_LIMIT, 1.0,
                      DEFAULT_VIF_ENHN_GAIN_LIMIT),
    VIF_DOUBLE_OPTION("vif_kernelscale", "ks",
                      "scaling factor for the gaussian kernel (2.0 means multiplying the standard "
                      "deviation by 2 and enlarge the kernel size accordingly",
                      vif_kernelscale, DEFAULT_VIF_KERNELSCALE, 0.1, 4.0),
    VIF_DOUBLE_OPTION("vif_prescale", "ps",
                      "scaling factor for the frame (2.0 means making the image twice as large on "
                      "each dimension)",
                      vif_prescale, DEFAULT_VIF_PRESCALE, 0.1, 4.0),
    VIF_DOUBLE_OPTION("vif_scale1_min_val", "s1miv",
                      "minimum value allowed; smaller values will be set to this value",
                      vif_scale1_min_val, DEFAULT_VIF_MIN_VAL, 0.0, 1.0),
    VIF_DOUBLE_OPTION("vif_scale2_min_val", "s2miv",
                      "minimum value allowed; smaller values will be set to this value",
                      vif_scale2_min_val, DEFAULT_VIF_MIN_VAL, 0.0, 1.0),
    VIF_DOUBLE_OPTION("vif_scale3_min_val", "s3miv",
                      "minimum value allowed; smaller values will be set to this value",
                      vif_scale3_min_val, DEFAULT_VIF_MIN_VAL, 0.0, 1.0),
    VIF_STRING_OPTION(
        "vif_prescale_method", "pm",
        "scaling method for the frame, supported options: [nearest, bilinear, bicubic, lanczos4]",
        vif_prescale_method, DEFAULT_VIF_PRESCALE_METHOD),
    VIF_DOUBLE_OPTION("vif_sigma_nsq", "snsq", "neural noise variance", vif_sigma_nsq, 2.0, 0.0,
                      5.0),
    VIF_BOOL_OPTION("vif_skip_scale0", "ssclz", "skip scale 0 calculations", vif_skip_scale0,
                    VMAF_OPT_FLAG_FEATURE_PARAM),
    {0}};

static int release_vif_resources(VifState *s)
{
    aligned_free(s->ref);
    aligned_free(s->dist);
    aligned_free(s->ref_scaled);
    aligned_free(s->dist_scaled);
    aligned_free(s->vif_buf);
    s->ref = NULL;
    s->dist = NULL;
    s->ref_scaled = NULL;
    s->dist_scaled = NULL;
    s->vif_buf = NULL;
    return vmaf_dictionary_free(&s->feature_name_dict);
}

static int set_vif_geometry(VifState *s, unsigned w, unsigned h)
{
    const int vif_min_dim = vif_get_min_dim((float)s->vif_kernelscale);
    if (w < (unsigned)vif_min_dim || h < (unsigned)vif_min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_vif requires width >= %d and height >= %d for the four-scale "
                 "ladder (got %ux%u)\n",
                 vif_min_dim, vif_min_dim, w, h);
        return -EINVAL;
    }

    enum vif_scaling_method scaling_method;
    if (vif_get_scaling_method(s->vif_prescale_method, &scaling_method))
        return -EINVAL;
    s->scaled_w = (size_t)(w * s->vif_prescale + 0.5);
    s->scaled_h = (size_t)(h * s->vif_prescale + 0.5);
    if (s->scaled_w < (size_t)vif_min_dim || s->scaled_h < (size_t)vif_min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_vif requires scaled width >= %d and height >= %d for the "
                 "four-scale ladder (got %zux%zu)\n",
                 vif_min_dim, vif_min_dim, s->scaled_w, s->scaled_h);
        return -EINVAL;
    }
    return 0;
}

static int get_vif_plane_size(size_t width, size_t height, size_t *stride, size_t *plane_size)
{
    if (width > (SIZE_MAX - (MAX_ALIGN - 1)) / sizeof(float))
        return -EOVERFLOW;
    *stride = ALIGN_CEIL(width * sizeof(float));
    if (height > SIZE_MAX / *stride)
        return -EOVERFLOW;
    *plane_size = *stride * height;
    return 0;
}

static int allocate_vif_resources(VifState *s, unsigned w, unsigned h)
{
    size_t source_plane_size;
    size_t scaled_plane_size;
    int err = get_vif_plane_size(w, h, &s->float_stride, &source_plane_size);
    if (err)
        return err;
    err = get_vif_plane_size(s->scaled_w, s->scaled_h, &s->scaled_float_stride, &scaled_plane_size);
    if (err)
        return err;
    if (scaled_plane_size > SIZE_MAX / VIF_SCRATCH_BUF_CNT)
        return -EOVERFLOW;

    s->ref = aligned_malloc(source_plane_size, MAX_ALIGN);
    if (!s->ref)
        return -ENOMEM;
    s->dist = aligned_malloc(source_plane_size, MAX_ALIGN);
    if (!s->dist)
        return -ENOMEM;
    s->ref_scaled = aligned_malloc(scaled_plane_size, MAX_ALIGN);
    if (!s->ref_scaled)
        return -ENOMEM;
    s->dist_scaled = aligned_malloc(scaled_plane_size, MAX_ALIGN);
    if (!s->dist_scaled)
        return -ENOMEM;
    s->vif_buf = aligned_malloc(scaled_plane_size * VIF_SCRATCH_BUF_CNT, MAX_ALIGN);
    if (!s->vif_buf)
        return -ENOMEM;
    return 0;
}

static void init_vif_filters(VifState *s)
{
    for (int sc = 0; sc < 4; ++sc) {
        s->filter_width_cache[sc] = vif_get_filter_size(sc, (float)s->vif_kernelscale);
        vif_get_filter(s->filter_cache[sc], sc, (float)s->vif_kernelscale);
    }
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    VifState *const s = fex->priv;
    int err = set_vif_geometry(s, w, h);
    if (!err)
        err = allocate_vif_resources(s, w, h);
    if (err) {
        const int cleanup_err = release_vif_resources(s);
        if (cleanup_err)
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_vif allocation cleanup failed: %d\n",
                     cleanup_err);
        return err;
    }
    init_vif_filters(s);
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict)
        return 0;
    err = release_vif_resources(s);
    if (err)
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_vif dictionary cleanup failed: %d\n", err);
    return -ENOMEM;
}

static int append_vif_scale_scores(const VifState *s, VmafFeatureCollector *collector,
                                   unsigned index, const double scores[8])
{
    static const char *const names[] = {
        "VMAF_feature_vif_scale0_score", "VMAF_feature_vif_scale1_score",
        "VMAF_feature_vif_scale2_score", "VMAF_feature_vif_scale3_score"};
    const double values[] = {s->vif_skip_scale0 ? 0.0 : scores[0] / scores[1],
                             VIF_MAX(scores[2] / scores[3], s->vif_scale1_min_val),
                             VIF_MAX(scores[4] / scores[5], s->vif_scale2_min_val),
                             VIF_MAX(scores[6] / scores[7], s->vif_scale3_min_val)};
    int err = 0;
    for (unsigned i = 0; i < 4; i++)
        err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, names[i],
                                                       values[i], index);
    return err;
}

static int append_vif_debug_scores(const VifState *s, VmafFeatureCollector *collector,
                                   unsigned index, double score, double score_num, double score_den,
                                   const double scores[8])
{
    static const char *const names[] = {"vif",
                                        "vif_num",
                                        "vif_den",
                                        "vif_num_scale0",
                                        "vif_den_scale0",
                                        "vif_num_scale1",
                                        "vif_den_scale1",
                                        "vif_num_scale2",
                                        "vif_den_scale2",
                                        "vif_num_scale3",
                                        "vif_den_scale3"};
    const double values[] = {score,
                             score_num,
                             score_den,
                             s->vif_skip_scale0 ? 0.0 : scores[0],
                             s->vif_skip_scale0 ? -1.0 : scores[1],
                             scores[2],
                             scores[3],
                             scores[4],
                             scores[5],
                             scores[6],
                             scores[7]};
    int err = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, names[i],
                                                       values[i], index);
    return err;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    VifState *const s = fex->priv;
    (void)ref_pic_90;
    (void)dist_pic_90;
    picture_copy(s->ref, s->float_stride, ref_pic, -128, ref_pic->bpc, 0);
    picture_copy(s->dist, s->float_stride, dist_pic, -128, dist_pic->bpc, 0);

    enum vif_scaling_method scaling_method;
    const int scaling_err = vif_get_scaling_method(s->vif_prescale_method, &scaling_method);
    if (scaling_err)
        return scaling_err;
    vif_scale_frame_s(scaling_method, s->ref, s->ref_scaled, ref_pic->w[0], ref_pic->h[0],
                      s->float_stride / sizeof(float), s->scaled_w, s->scaled_h,
                      s->scaled_float_stride / sizeof(float));
    vif_scale_frame_s(scaling_method, s->dist, s->dist_scaled, dist_pic->w[0], dist_pic->h[0],
                      s->float_stride / sizeof(float), s->scaled_w, s->scaled_h,
                      s->scaled_float_stride / sizeof(float));

    double score, score_num, score_den;
    double scores[8];
    int err =
        compute_vif(s->ref_scaled, s->dist_scaled, s->scaled_w, s->scaled_h, s->scaled_float_stride,
                    s->scaled_float_stride, &score, &score_num, &score_den, scores,
                    s->vif_enhn_gain_limit, s->vif_kernelscale, s->vif_skip_scale0,
                    s->vif_sigma_nsq, (const float (*)[128])s->filter_cache, s->filter_width_cache);
    if (err)
        return err;
    err = append_vif_scale_scores(s, feature_collector, index, scores);
    if (!s->debug)
        return err;
    return err | append_vif_debug_scores(s, feature_collector, index, score, score_num, score_den,
                                         scores);
}

static int close(VmafFeatureExtractor *fex)
{
    return release_vif_resources(fex->priv);
}

static const char *provided_features[] = {"VMAF_feature_vif_scale0_score",
                                          "VMAF_feature_vif_scale1_score",
                                          "VMAF_feature_vif_scale2_score",
                                          "VMAF_feature_vif_scale3_score",
                                          "vif",
                                          "vif_num",
                                          "vif_den",
                                          "vif_num_scale0",
                                          "vif_den_scale0",
                                          "vif_num_scale1",
                                          "vif_den_scale1",
                                          "vif_num_scale2",
                                          "vif_den_scale2",
                                          "vif_num_scale3",
                                          "vif_den_scale3",
                                          NULL};

VmafFeatureExtractor vmaf_fex_float_vif = {
    .name = "float_vif",
    .init = init,
    .extract = extract,
    .options = options,
    .close = close,
    .priv_size = sizeof(VifState),
    .provided_features = provided_features,
};
