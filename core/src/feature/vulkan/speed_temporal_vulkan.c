/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  speed_temporal feature extractor — Vulkan backend with real on-device
 *  GPU kernels (ADR-0567).
 *
 *  Temporal design: two ping-pong host float buffers hold converted luma
 *  planes.  Each frame the GPU runs the full SpEED pipeline on the
 *  temporal difference (prev − cur) rather than the raw plane.
 *  Frame 0 emits score 0 (no previous frame).
 *
 *  Algorithm split: identical to speed_chroma_vulkan.c.  See that file
 *  and ADR-0567 for the full GPU/CPU split rationale.
 *
 *  Output feature: Speed_temporal_feature_speed_temporal_score.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_copy.h"

#include "../../vulkan/kernel_template.h"
#include "../../vulkan/vulkan_common.h"
#include "../../vulkan/vulkan_internal.h"

#include "feature/speed_internal.h"
#include "speed_score_spv.h"

#define SP_VK_NUM_BINDINGS 13u

typedef struct SpeedVkPushConstsT {
    uint32_t pass;
    uint32_t num_blocks;
    uint32_t num_blocks_h;
    uint32_t submatrix_w;
    uint32_t submatrix_h;
    uint32_t stride_px;
    float sigma_nn;
} SpeedVkPushConstsT;

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct SpeedTemporalVulkanState {
    VmafVulkanContext *ctx;
    int owns_ctx;
    VmafVulkanKernelPipeline pl;
    VmafVulkanKernelSubmitPool sub_pool;
    VkDescriptorSet pre_set;

    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride;

    /* Ping-pong host luma plane buffers. */
    float *h_ref[2];
    float *h_dis[2];

    /* GPU buffers (same layout as speed_chroma_vulkan). */
    VmafVulkanBuffer *b_plane;
    VmafVulkanBuffer *b_means;
    VmafVulkanBuffer *b_cov_mat;
    VmafVulkanBuffer *b_iref;
    VmafVulkanBuffer *b_idis;
    VmafVulkanBuffer *b_sol_ref;
    VmafVulkanBuffer *b_sol_dis;
    VmafVulkanBuffer *b_R;
    VmafVulkanBuffer *b_eig;
    VmafVulkanBuffer *b_ref_ent;
    VmafVulkanBuffer *b_ref_var;
    VmafVulkanBuffer *b_dis_ent;
    VmafVulkanBuffer *b_dis_var;

    /* CPU scratch. */
    float *h_eigenvalues;
    float *h_eig_scratch;
    float *h_Q;
    float *h_R;
    float *h_qr_scratch;
    float *h_indterm_ref;
    float *h_indterm_dis;
    float *h_qt_scratch;

    unsigned frame_index;

    double speed_temporal_kernelscale;
    double speed_temporal_prescale;
    char *speed_temporal_prescale_method;
    double speed_temporal_sigma_nn;
    double speed_temporal_nn_floor;
    double speed_temporal_max_val;
    bool speed_temporal_use_ref_diff;

    VmafDictionary *feature_name_dict;
} SpeedTemporalVulkanState;

static const VmafOption options_temporal[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedTemporalVulkanState, speed_temporal_kernelscale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ks",
    },
    {
        .name = "speed_prescale",
        .help = "scaling factor for the frame",
        .offset = offsetof(SpeedTemporalVulkanState, speed_temporal_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ps",
    },
    {
        .name = "speed_prescale_method",
        .help = "scaling method [nearest, bilinear, bicubic, lanczos4]",
        .offset = offsetof(SpeedTemporalVulkanState, speed_temporal_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = "nearest",
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedTemporalVulkanState, speed_temporal_sigma_nn),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.29,
        .min = 0.1,
        .max = 2.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "snn",
    },
    {
        .name = "speed_nn_floor",
        .help = "neural noise floor fraction",
        .offset = offsetof(SpeedTemporalVulkanState, speed_temporal_nn_floor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "nnf",
    },
    {
        .name = "speed_max_val",
        .help = "clip output to this maximum",
        .offset = offsetof(SpeedTemporalVulkanState, speed_temporal_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1000.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mxv",
    },
    {
        .name = "speed_use_ref_diff",
        .help = "use reference frame difference instead of distorted",
        .offset = offsetof(SpeedTemporalVulkanState, speed_temporal_use_ref_diff),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "urd",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Helpers (mirror speed_chroma_vulkan.c; kept as separate TU)        */
/* ------------------------------------------------------------------ */

static void subtract_plane_st(float *a, const float *b, int w, int h, size_t stride_bytes)
{
    const size_t stride_px = stride_bytes / sizeof(float);
    for (int i = 0; i < h; ++i)
        for (int j = 0; j < w; ++j)
            a[(size_t)i * stride_px + (size_t)j] -= b[(size_t)i * stride_px + (size_t)j];
}

static int create_pipeline_st(SpeedTemporalVulkanState *s)
{
    const VmafVulkanKernelPipelineDesc desc = {
        .ssbo_binding_count = SP_VK_NUM_BINDINGS,
        .push_constant_size = sizeof(SpeedVkPushConstsT),
        .spv_bytes = speed_score_spv,
        .spv_size = speed_score_spv_size,
        .pipeline_create_info =
            {
                .stage = {.pName = "main"},
            },
        .max_descriptor_sets = 4u,
    };
    return vmaf_vulkan_kernel_pipeline_create(s->ctx, &desc, &s->pl);
}

static int alloc_buffers_st(SpeedTemporalVulkanState *s)
{
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = 25u * nb * sizeof(float);
    const size_t cov_bytes = 25u * 25u * sizeof(float);
    const size_t score_bytes = nb * sizeof(float);

    int err = 0;
    err = err ?: vmaf_vulkan_buffer_alloc(s->ctx, &s->b_plane, plane_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc(s->ctx, &s->b_means, indterm_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->b_cov_mat, cov_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->b_iref, indterm_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->b_idis, indterm_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc(s->ctx, &s->b_sol_ref, indterm_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc(s->ctx, &s->b_sol_dis, indterm_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc(s->ctx, &s->b_R, cov_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc(s->ctx, &s->b_eig, 25u * sizeof(float));
    err = err ?: vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->b_ref_ent, score_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->b_ref_var, score_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->b_dis_ent, score_bytes);
    err = err ?: vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->b_dis_var, score_bytes);
    return err;
}

static void write_descriptor_set_st(SpeedTemporalVulkanState *s, VkDescriptorSet set)
{
    VmafVulkanBuffer *bufs[SP_VK_NUM_BINDINGS] = {
        s->b_plane,   s->b_means,   s->b_cov_mat, s->b_iref, s->b_idis,
        s->b_sol_ref, s->b_sol_dis, s->b_R,       s->b_eig,  s->b_ref_ent,
        s->b_ref_var, s->b_dis_ent, s->b_dis_var,
    };
    VkDescriptorBufferInfo dbi[SP_VK_NUM_BINDINGS];
    VkWriteDescriptorSet writes[SP_VK_NUM_BINDINGS];
    for (uint32_t i = 0u; i < SP_VK_NUM_BINDINGS; ++i) {
        dbi[i] = (VkDescriptorBufferInfo){
            .buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(bufs[i]),
            .offset = 0,
            .range = VK_WHOLE_SIZE,
        };
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &dbi[i],
        };
    }
    vkUpdateDescriptorSets(s->ctx->device, SP_VK_NUM_BINDINGS, writes, 0, NULL);
}

static int dispatch_pass_st(SpeedTemporalVulkanState *s, SpeedVkPushConstsT *pc_vals, uint32_t gx,
                            uint32_t gy)
{
    VmafVulkanKernelSubmit submit = {0};
    int err = vmaf_vulkan_kernel_submit_acquire(s->ctx, &s->sub_pool, 0, &submit);
    if (err)
        return err;
    VkCommandBuffer cmd = submit.cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pl.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pl.pipeline_layout, 0, 1,
                            &s->pre_set, 0, NULL);
    vkCmdPushConstants(cmd, s->pl.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*pc_vals),
                       pc_vals);
    vkCmdDispatch(cmd, gx, gy, 1);
    err = vmaf_vulkan_kernel_submit_end_and_wait(s->ctx, &submit);
    vmaf_vulkan_kernel_submit_free(s->ctx, &submit);
    return err;
}

static int upload_floats_st(SpeedTemporalVulkanState *s, VmafVulkanBuffer *buf, const float *src,
                            size_t bytes)
{
    float *dst = vmaf_vulkan_buffer_host(buf);
    memcpy(dst, src, bytes);
    return vmaf_vulkan_buffer_flush(s->ctx, buf);
}

/* GPU passes 0+1+2: upload plane, run means+cov+indterm_ref, readback. */
static int run_gpu_k012(SpeedTemporalVulkanState *s, const float *h_plane)
{
    const uint32_t nb = (uint32_t)s->dim.num_blocks;
    const uint32_t nbh = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t sw = (uint32_t)s->dim.submatrix_width;
    const uint32_t sh = (uint32_t)s->dim.submatrix_height;
    const uint32_t spx = (uint32_t)(s->float_stride / sizeof(float));
    const float snn = (float)s->opt.speed_sigma_nn;
    const size_t plane_bytes = s->dim.truncated_height * spx * sizeof(float);

    int err = upload_floats_st(s, s->b_plane, h_plane, plane_bytes);
    if (err)
        return err;

    SpeedVkPushConstsT pc = {0u, nb, nbh, sw, sh, spx, snn};

    err = dispatch_pass_st(s, &pc, (nb + 255u) / 256u, 1u);
    if (err)
        return err;
    pc.pass = 1u;
    err = dispatch_pass_st(s, &pc, 25u, 25u);
    if (err)
        return err;
    pc.pass = 2u;
    err = dispatch_pass_st(s, &pc, (25u * nb + 255u) / 256u, 1u);
    if (err)
        return err;

    const size_t cov_bytes = 25u * 25u * sizeof(float);
    const size_t indterm_bytes = 25u * (size_t)nb * sizeof(float);
    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_cov_mat);
    if (err)
        return err;
    memcpy(s->h_R, vmaf_vulkan_buffer_host(s->b_cov_mat), cov_bytes);

    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_iref);
    if (err)
        return err;
    memcpy(s->h_indterm_ref, vmaf_vulkan_buffer_host(s->b_iref), indterm_bytes);
    return 0;
}

/* GPU pass 3: indterm_dis; readback. */
static int run_gpu_k3(SpeedTemporalVulkanState *s, const float *h_plane)
{
    const uint32_t nb = (uint32_t)s->dim.num_blocks;
    const uint32_t nbh = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t sw = (uint32_t)s->dim.submatrix_width;
    const uint32_t sh = (uint32_t)s->dim.submatrix_height;
    const uint32_t spx = (uint32_t)(s->float_stride / sizeof(float));
    const float snn = (float)s->opt.speed_sigma_nn;
    const size_t plane_bytes = s->dim.truncated_height * spx * sizeof(float);
    const size_t indterm_bytes = 25u * (size_t)nb * sizeof(float);

    int err = upload_floats_st(s, s->b_plane, h_plane, plane_bytes);
    if (err)
        return err;

    SpeedVkPushConstsT pc = {3u, nb, nbh, sw, sh, spx, snn};
    err = dispatch_pass_st(s, &pc, (25u * nb + 255u) / 256u, 1u);
    if (err)
        return err;

    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_idis);
    if (err)
        return err;
    memcpy(s->h_indterm_dis, vmaf_vulkan_buffer_host(s->b_idis), indterm_bytes);
    return 0;
}

/* CPU linalg + upload R + upload solution rhs + GPU solve pass. */
static int run_linalg_and_solve_st(SpeedTemporalVulkanState *s, float *h_indterm,
                                   VmafVulkanBuffer *b_sol, uint32_t solve_pass)
{
    const int sz = 25;
    const int nb = (int)s->dim.num_blocks;
    const size_t cov_bytes = 25u * 25u * sizeof(float);
    const size_t indterm_bytes = 25u * (size_t)nb * sizeof(float);

    speed_internal_compute_eigenvalues(s->h_R, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, (size_t)sz);

    if (!regular) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_temporal_vulkan: covariance matrix singular, zeroing solution\n");
        memset(h_indterm, 0, indterm_bytes);
        float *dst = vmaf_vulkan_buffer_host(b_sol);
        memset(dst, 0, indterm_bytes);
        return vmaf_vulkan_buffer_flush(s->ctx, b_sol);
    }

    (void)speed_internal_qr_factorize(s->h_R, sz, s->h_Q, s->h_R, s->h_qr_scratch);
    speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);

    float *dst_R = vmaf_vulkan_buffer_host(s->b_R);
    memcpy(dst_R, s->h_R, cov_bytes);
    int err = vmaf_vulkan_buffer_flush(s->ctx, s->b_R);
    if (err)
        return err;

    float *dst_sol = vmaf_vulkan_buffer_host(b_sol);
    memcpy(dst_sol, h_indterm, indterm_bytes);
    err = vmaf_vulkan_buffer_flush(s->ctx, b_sol);
    if (err)
        return err;

    float *dst_eig = vmaf_vulkan_buffer_host(s->b_eig);
    memcpy(dst_eig, s->h_eigenvalues, 25u * sizeof(float));
    err = vmaf_vulkan_buffer_flush(s->ctx, s->b_eig);
    if (err)
        return err;

    const uint32_t nb_u = (uint32_t)nb;
    const uint32_t nbh = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t sw = (uint32_t)s->dim.submatrix_width;
    const uint32_t sh = (uint32_t)s->dim.submatrix_height;
    const uint32_t spx = (uint32_t)(s->float_stride / sizeof(float));
    const float snn = (float)s->opt.speed_sigma_nn;
    SpeedVkPushConstsT pc = {solve_pass, nb_u, nbh, sw, sh, spx, snn};
    return dispatch_pass_st(s, &pc, nb_u, 1u);
}

/* GPU pass 6 score + readback + CPU aggregate. */
static int run_score_st(SpeedTemporalVulkanState *s, float *score_out)
{
    const uint32_t nb = (uint32_t)s->dim.num_blocks;
    const uint32_t nbh = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t sw = (uint32_t)s->dim.submatrix_width;
    const uint32_t sh = (uint32_t)s->dim.submatrix_height;
    const uint32_t spx = (uint32_t)(s->float_stride / sizeof(float));
    const float snn = (float)s->opt.speed_sigma_nn;

    SpeedVkPushConstsT pc = {6u, nb, nbh, sw, sh, spx, snn};
    int err = dispatch_pass_st(s, &pc, (nb + 255u) / 256u, 1u);
    if (err)
        return err;

    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_ref_ent);
    if (err)
        return err;
    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_ref_var);
    if (err)
        return err;
    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_dis_ent);
    if (err)
        return err;
    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_dis_var);
    if (err)
        return err;

    const float *h_ref_ent = vmaf_vulkan_buffer_host(s->b_ref_ent);
    const float *h_ref_var = vmaf_vulkan_buffer_host(s->b_ref_var);
    const float *h_dis_ent = vmaf_vulkan_buffer_host(s->b_dis_ent);
    const float *h_dis_var = vmaf_vulkan_buffer_host(s->b_dis_var);

    const float base_entropy =
        25.0f * (log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
                 log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f));

    float total = 0.0f;
    for (uint32_t i = 0; i < nb; ++i) {
        const float re = h_ref_ent[i];
        const float de = h_dis_ent[i];
        if (re < base_entropy && de < base_entropy)
            continue;
        const float rv = h_ref_var[i];
        const float dv = h_dis_var[i];
        /* speed_temporal uses weight_var_mode = 0. */
        total += fabsf(re * log2f(1.0f + rv) - de * log2f(1.0f + dv));
    }
    *score_out = total / (float)nb;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static int init_temporal_vk(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                            unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    SpeedTemporalVulkanState *s = fex->priv;

    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };

    int err = speed_internal_init_dimensions(&s->dim, (int)w, (int)h, s->opt.speed_prescale);
    if (err)
        return err;
    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);

    s->ctx = vmaf_vulkan_state_get_context(fex->vulkan_state);
    if (s->ctx) {
        s->owns_ctx = 0;
    } else {
        err = vmaf_vulkan_context_new(&s->ctx, -1);
        if (err)
            return err;
        s->owns_ctx = 1;
    }

    err = create_pipeline_st(s);
    if (err)
        return err;
    err = alloc_buffers_st(s);
    if (err)
        return err;

    err = vmaf_vulkan_kernel_submit_pool_create(s->ctx, 1, &s->sub_pool);
    if (err)
        return err;
    err = vmaf_vulkan_kernel_descriptor_sets_alloc(s->ctx, s->pl.desc_pool, s->pl.dsl, 1,
                                                   &s->pre_set);
    if (err)
        return err;
    write_descriptor_set_st(s, s->pre_set);

    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = 25u * nb * sizeof(float);
    const size_t cov_bytes = 25u * 25u * sizeof(float);

#define AA(f, sz) s->f = (float *)aligned_malloc((sz), 32)
    AA(h_ref[0], plane_bytes);
    AA(h_ref[1], plane_bytes);
    AA(h_dis[0], plane_bytes);
    AA(h_dis[1], plane_bytes);
    AA(h_eigenvalues, 25u * sizeof(float));
    AA(h_eig_scratch, (25u * 25u + 75u) * sizeof(float));
    AA(h_Q, cov_bytes);
    AA(h_R, cov_bytes);
    AA(h_qr_scratch, 4u * cov_bytes);
    AA(h_indterm_ref, indterm_bytes);
    AA(h_indterm_dis, indterm_bytes);
    AA(h_qt_scratch, indterm_bytes);
#undef AA

    if (!s->h_ref[0] || !s->h_ref[1] || !s->h_dis[0] || !s->h_dis[1] || !s->h_eigenvalues ||
        !s->h_Q || !s->h_R)
        return -ENOMEM;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    s->frame_index = 0;
    return 0;
}

static int extract_temporal_vk(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                               VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                               VmafPicture *dist_pic_90, unsigned index,
                               VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    SpeedTemporalVulkanState *s = fex->priv;

    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);

    picture_copy(s->h_ref[cyclic], s->float_stride, ref_pic, -128, ref_pic->bpc, 0);
    picture_copy(s->h_dis[cyclic], s->float_stride, dist_pic, -128, dist_pic->bpc, 0);

    if (index == 0) {
        s->frame_index++;
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }

    const int orig_w = (int)s->dim.original_width;
    const int orig_h = (int)s->dim.original_height;
    subtract_plane_st(s->h_ref[other], s->h_ref[cyclic], orig_w, orig_h, s->float_stride);
    if (s->speed_temporal_use_ref_diff)
        subtract_plane_st(s->h_dis[other], s->h_ref[cyclic], orig_w, orig_h, s->float_stride);
    else
        subtract_plane_st(s->h_dis[other], s->h_dis[cyclic], orig_w, orig_h, s->float_stride);

    /* Filter+downscale the temporal diff planes. */
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t tmp_size = 2u * s->dim.alloc_height * stride_px;
    float *tmp_filter = (float *)aligned_malloc(tmp_size * sizeof(float), 32);
    if (!tmp_filter)
        return -ENOMEM;
    speed_internal_filter_and_downscale(s->dim, &s->opt, s->h_ref[other], tmp_filter,
                                        s->float_stride);
    speed_internal_filter_and_downscale(s->dim, &s->opt, s->h_dis[other], tmp_filter,
                                        s->float_stride);
    aligned_free(tmp_filter);

    /* GPU passes 0,1,2 on ref diff: means, cov, indterm_ref. Readback. */
    int err = run_gpu_k012(s, s->h_ref[other]);
    if (err)
        return err;

    /* Save cov copy (h_R was filled by readback). */
    float saved_cov[25 * 25];
    memcpy(saved_cov, s->h_R, sizeof(saved_cov));

    /* CPU linalg on ref + GPU pass 4 solve_ref. */
    err = run_linalg_and_solve_st(s, s->h_indterm_ref, s->b_sol_ref, 4u);
    if (err)
        return err;

    /* GPU pass 3 on dis diff: indterm_dis. Readback. */
    err = run_gpu_k3(s, s->h_dis[other]);
    if (err)
        return err;

    /* Restore ref cov for dis channel linalg. */
    memcpy(s->h_R, saved_cov, sizeof(saved_cov));

    /* CPU linalg on dis + GPU pass 5 solve_dis. */
    err = run_linalg_and_solve_st(s, s->h_indterm_dis, s->b_sol_dis, 5u);
    if (err)
        return err;

    float score = 0.0f;
    err = run_score_st(s, &score);
    if (err)
        return err;

    s->frame_index++;
    const double mxv = s->speed_temporal_max_val;
    const double clipped = (double)score < mxv ? (double)score : mxv;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
}

static int close_temporal_vk(VmafFeatureExtractor *fex)
{
    SpeedTemporalVulkanState *s = fex->priv;
    if (!s->ctx)
        return 0;

    vmaf_vulkan_kernel_submit_pool_destroy(s->ctx, &s->sub_pool);
    vmaf_vulkan_kernel_pipeline_destroy(s->ctx, &s->pl);

#define FREE_BUF(b)                                                                                \
    do {                                                                                           \
        if ((b)) {                                                                                 \
            vmaf_vulkan_buffer_free(s->ctx, (b));                                                  \
            (b) = NULL;                                                                            \
        }                                                                                          \
    } while (0)
    FREE_BUF(s->b_plane);
    FREE_BUF(s->b_means);
    FREE_BUF(s->b_cov_mat);
    FREE_BUF(s->b_iref);
    FREE_BUF(s->b_idis);
    FREE_BUF(s->b_sol_ref);
    FREE_BUF(s->b_sol_dis);
    FREE_BUF(s->b_R);
    FREE_BUF(s->b_eig);
    FREE_BUF(s->b_ref_ent);
    FREE_BUF(s->b_ref_var);
    FREE_BUF(s->b_dis_ent);
    FREE_BUF(s->b_dis_var);
#undef FREE_BUF

    if (s->owns_ctx)
        vmaf_vulkan_context_destroy(s->ctx);
    s->ctx = NULL;

    aligned_free(s->h_ref[0]);
    aligned_free(s->h_ref[1]);
    aligned_free(s->h_dis[0]);
    aligned_free(s->h_dis[1]);
    aligned_free(s->h_eigenvalues);
    aligned_free(s->h_eig_scratch);
    aligned_free(s->h_Q);
    aligned_free(s->h_R);
    aligned_free(s->h_qr_scratch);
    aligned_free(s->h_indterm_ref);
    aligned_free(s->h_indterm_dis);
    aligned_free(s->h_qt_scratch);

    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_temporal[] = {
    "Speed_temporal_feature_speed_temporal_score",
    NULL,
};

/* ADR-0567: real Vulkan GPU kernels for speed_temporal.
 * TEMPORAL flag guarantees sequential frame submission (ping-pong diff
 * requires frame ordering). */
VmafFeatureExtractor vmaf_fex_speed_temporal_vulkan = {
    .name = "speed_temporal_vulkan",
    .init = init_temporal_vk,
    .extract = extract_temporal_vk,
    .close = close_temporal_vk,
    .options = options_temporal,
    .priv_size = sizeof(SpeedTemporalVulkanState),
    .provided_features = provided_features_temporal,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_VULKAN,
};
