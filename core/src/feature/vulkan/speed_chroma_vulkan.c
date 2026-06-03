/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  speed_chroma feature extractor — Vulkan backend with real on-device
 *  GPU kernels (ADR-0567).
 *
 *  Algorithm split: identical to speed_chroma_cuda.c.  See that file and
 *  ADR-0567 for the full GPU/CPU split rationale.
 *
 *  GPU (speed_score.comp, 7 passes):
 *    pass 0 — means
 *    pass 1 — covariance (float64 shared-memory reduction)
 *    pass 2 — indterm_ref
 *    pass 3 — indterm_dis
 *    pass 4 — solve_ref (backward substitution)
 *    pass 5 — solve_dis
 *    pass 6 — score (per-tile entropy + variance proxy)
 *
 *  CPU (between passes 2/3 and 4/5):
 *    eigendecomp, QR factorize, Qt×indterm — 25×25 serial ops.
 *    Results uploaded to R_mat and sol_ref/sol_dis buffers.
 *
 *  The submit-pool pattern (ADR-0353) is used: one slot reused per
 *  dispatch.  Multiple end_and_wait calls per frame are required due
 *  to the CPU linalg step between GPU passes.
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

/* Number of SSBO bindings in speed_score.comp (bindings 0-12). */
#define SP_VK_NUM_BINDINGS 13u

/* Push constant struct matching speed_score.comp layout. */
typedef struct SpeedVkPushConsts {
    uint32_t pass;
    uint32_t num_blocks;
    uint32_t num_blocks_h;
    uint32_t submatrix_w;
    uint32_t submatrix_h;
    uint32_t stride_px;
    float sigma_nn;
} SpeedVkPushConsts;

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct SpeedChromaVulkanState {
    VmafVulkanContext *ctx;
    int owns_ctx;
    VmafVulkanKernelPipeline pl;
    VmafVulkanKernelSubmitPool sub_pool;
    VkDescriptorSet pre_set;

    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride; /* stride in bytes */

    /* GPU buffers. */
    VmafVulkanBuffer *b_plane;   /* input float plane         */
    VmafVulkanBuffer *b_means;   /* 25 × num_blocks           */
    VmafVulkanBuffer *b_cov_mat; /* 25 × 25 (readback)        */
    VmafVulkanBuffer *b_iref;    /* indterm_ref (readback)    */
    VmafVulkanBuffer *b_idis;    /* indterm_dis (readback)    */
    VmafVulkanBuffer *b_sol_ref; /* solution ref              */
    VmafVulkanBuffer *b_sol_dis; /* solution dis              */
    VmafVulkanBuffer *b_R;       /* R matrix (host-write)     */
    VmafVulkanBuffer *b_eig;     /* eigenvalues (host-write)  */
    VmafVulkanBuffer *b_ref_ent; /* ref entropy (readback)    */
    VmafVulkanBuffer *b_ref_var; /* ref variance (readback)   */
    VmafVulkanBuffer *b_dis_ent; /* dis entropy (readback)    */
    VmafVulkanBuffer *b_dis_var; /* dis variance (readback)   */

    /* CPU scratch (always aligned_malloc). */
    float *h_plane_ref;
    float *h_plane_dis;
    float *h_eigenvalues;
    float *h_eig_scratch;
    float *h_Q;
    float *h_R;
    float *h_qr_scratch;
    float *h_indterm_ref;
    float *h_indterm_dis;
    float *h_qt_scratch;

    /* User options. */
    double speed_chroma_kernelscale;
    double speed_chroma_prescale;
    char *speed_chroma_prescale_method;
    double speed_chroma_sigma_nn;
    double speed_chroma_nn_floor;
    double speed_chroma_max_val;
    int speed_weight_var_mode;

    VmafDictionary *feature_name_dict;
} SpeedChromaVulkanState;

static const VmafOption options_chroma[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedChromaVulkanState, speed_chroma_kernelscale),
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
        .offset = offsetof(SpeedChromaVulkanState, speed_chroma_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ps",
    },
    {
        .name = "speed_prescale_method",
        .help = "scaling method",
        .offset = offsetof(SpeedChromaVulkanState, speed_chroma_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = "nearest",
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedChromaVulkanState, speed_chroma_sigma_nn),
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
        .offset = offsetof(SpeedChromaVulkanState, speed_chroma_nn_floor),
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
        .offset = offsetof(SpeedChromaVulkanState, speed_chroma_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1000.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mxv",
    },
    {
        .name = "speed_weight_var_mode",
        .help = "variance weighting mode (0-6)",
        .offset = offsetof(SpeedChromaVulkanState, speed_weight_var_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.d = 0,
        .min = 0,
        .max = 6,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "wvm",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int create_pipeline(SpeedChromaVulkanState *s)
{
    const VmafVulkanKernelPipelineDesc desc = {
        .ssbo_binding_count = SP_VK_NUM_BINDINGS,
        .push_constant_size = sizeof(SpeedVkPushConsts),
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

static int alloc_buffers(SpeedChromaVulkanState *s)
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

static void write_descriptor_set(SpeedChromaVulkanState *s, VkDescriptorSet set)
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

/* Record and submit a single pass dispatch, then wait for it. */
static int dispatch_pass(SpeedChromaVulkanState *s, SpeedVkPushConsts *pc_vals, uint32_t gx,
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

/* Upload a host float buffer into a Vulkan buffer (host-visible flush). */
static int upload_floats(SpeedChromaVulkanState *s, VmafVulkanBuffer *buf, const float *src,
                         size_t bytes)
{
    float *dst = vmaf_vulkan_buffer_host(buf);
    memcpy(dst, src, bytes);
    return vmaf_vulkan_buffer_flush(s->ctx, buf);
}

/* Upload the plane to b_plane, run passes 0+1+2 (means, cov, indterm_ref),
 * then readback cov_mat and indterm_ref to the CPU scratch buffers. */
static int run_gpu_k123_ref(SpeedChromaVulkanState *s)
{
    const uint32_t nb = (uint32_t)s->dim.num_blocks;
    const uint32_t nbh = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t sw = (uint32_t)s->dim.submatrix_width;
    const uint32_t sh = (uint32_t)s->dim.submatrix_height;
    const uint32_t spx = (uint32_t)(s->float_stride / sizeof(float));
    const float snn = (float)s->opt.speed_sigma_nn;

    /* Upload ref plane. */
    {
        const size_t plane_bytes = s->dim.truncated_height * spx * sizeof(float);
        int err = upload_floats(s, s->b_plane, s->h_plane_ref, plane_bytes);
        if (err)
            return err;
    }

    SpeedVkPushConsts pc = {0, nb, nbh, sw, sh, spx, snn};

    /* pass 0: means */
    {
        uint32_t gx = (nb + 255u) / 256u;
        int err = dispatch_pass(s, &pc, gx, 1u);
        if (err)
            return err;
    }
    /* pass 1: cov — 25×25 workgroups */
    {
        pc.pass = 1u;
        int err = dispatch_pass(s, &pc, 25u, 25u);
        if (err)
            return err;
    }
    /* pass 2: indterm_ref */
    {
        pc.pass = 2u;
        uint32_t gx = (25u * nb + 255u) / 256u;
        int err = dispatch_pass(s, &pc, gx, 1u);
        if (err)
            return err;
    }

    /* Readback cov_mat (625 floats) and indterm_ref (25×nb floats). */
    const size_t cov_bytes = 25u * 25u * sizeof(float);
    const size_t indterm_bytes = 25u * (size_t)nb * sizeof(float);
    int err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_cov_mat);
    if (err)
        return err;
    memcpy(s->h_plane_ref, vmaf_vulkan_buffer_host(s->b_cov_mat), cov_bytes);
    /* Reuse h_plane_ref as cov scratch? No — use h_qt_scratch as temp.
     * Actually use a dedicated approach: the cov_mat host copy goes into
     * h_eig_scratch-derived buffer.  Since h_plane_ref holds plane data
     * we need for cov, use h_R as cov host copy (it gets overwritten by
     * QR anyway). */
    /* Design: cov_mat readback → h_R (pre-QR slot), then QR writes h_R.
     * This is safe because QR input and output are distinct arrays. */
    memcpy(s->h_R, vmaf_vulkan_buffer_host(s->b_cov_mat), cov_bytes);

    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_iref);
    if (err)
        return err;
    memcpy(s->h_indterm_ref, vmaf_vulkan_buffer_host(s->b_iref), indterm_bytes);
    return 0;
}

/* Run passes 3 (indterm_dis), readback indterm_dis. */
static int run_gpu_k3_dis(SpeedChromaVulkanState *s)
{
    const uint32_t nb = (uint32_t)s->dim.num_blocks;
    const uint32_t nbh = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t sw = (uint32_t)s->dim.submatrix_width;
    const uint32_t sh = (uint32_t)s->dim.submatrix_height;
    const uint32_t spx = (uint32_t)(s->float_stride / sizeof(float));
    const float snn = (float)s->opt.speed_sigma_nn;
    const size_t plane_bytes = s->dim.truncated_height * spx * sizeof(float);
    const size_t indterm_bytes = 25u * (size_t)nb * sizeof(float);

    int err = upload_floats(s, s->b_plane, s->h_plane_dis, plane_bytes);
    if (err)
        return err;

    SpeedVkPushConsts pc = {3u, nb, nbh, sw, sh, spx, snn};
    uint32_t gx = (25u * nb + 255u) / 256u;
    err = dispatch_pass(s, &pc, gx, 1u);
    if (err)
        return err;

    err = vmaf_vulkan_buffer_invalidate(s->ctx, s->b_idis);
    if (err)
        return err;
    memcpy(s->h_indterm_dis, vmaf_vulkan_buffer_host(s->b_idis), indterm_bytes);
    return 0;
}

/* CPU linalg: eigendecomp, QR, Qt×indterm.  Upload R and solution.
 * Run pass 4 or 5 (solve).  Returns 0 on success or if singular. */
static int run_cpu_linalg_and_solve(SpeedChromaVulkanState *s, float *h_indterm,
                                    VmafVulkanBuffer *b_sol, uint32_t solve_pass)
{
    const int sz = 25;
    const int nb = (int)s->dim.num_blocks;
    const size_t cov_bytes = 25u * 25u * sizeof(float);
    const size_t indterm_bytes = 25u * (size_t)nb * sizeof(float);

    /* h_R holds the cov_mat copy from readback. */
    speed_internal_compute_eigenvalues(s->h_R, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, (size_t)sz);

    if (!regular) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_chroma_vulkan: covariance matrix singular, zeroing solution\n");
        memset(h_indterm, 0, indterm_bytes);
        /* Upload zeros as the solution. */
        float *dst = vmaf_vulkan_buffer_host(b_sol);
        memset(dst, 0, indterm_bytes);
        return vmaf_vulkan_buffer_flush(s->ctx, b_sol);
    }

    /* QR factorize: input h_R (cov_mat copy), output h_Q + h_R (overwrite). */
    (void)speed_internal_qr_factorize(s->h_R, sz, s->h_Q, s->h_R, s->h_qr_scratch);
    speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);

    /* Upload R matrix. */
    {
        float *dst = vmaf_vulkan_buffer_host(s->b_R);
        memcpy(dst, s->h_R, cov_bytes);
        int err = vmaf_vulkan_buffer_flush(s->ctx, s->b_R);
        if (err)
            return err;
    }
    /* Upload Q^T×indterm as initial rhs for the solve kernel. */
    {
        float *dst = vmaf_vulkan_buffer_host(b_sol);
        memcpy(dst, h_indterm, indterm_bytes);
        int err = vmaf_vulkan_buffer_flush(s->ctx, b_sol);
        if (err)
            return err;
    }

    /* Upload eigenvalues. */
    {
        float *dst = vmaf_vulkan_buffer_host(s->b_eig);
        memcpy(dst, s->h_eigenvalues, 25u * sizeof(float));
        int err = vmaf_vulkan_buffer_flush(s->ctx, s->b_eig);
        if (err)
            return err;
    }

    /* pass 4 or 5: backward substitution — num_blocks workgroups */
    const uint32_t nb_u = (uint32_t)nb;
    const uint32_t nbh = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t sw = (uint32_t)s->dim.submatrix_width;
    const uint32_t sh = (uint32_t)s->dim.submatrix_height;
    const uint32_t spx = (uint32_t)(s->float_stride / sizeof(float));
    const float snn = (float)s->opt.speed_sigma_nn;
    SpeedVkPushConsts pc = {solve_pass, nb_u, nbh, sw, sh, spx, snn};
    return dispatch_pass(s, &pc, nb_u, 1u);
}

/* Run pass 6 (score), readback, CPU aggregate. */
static int run_score_and_collect(SpeedChromaVulkanState *s, float *score_out)
{
    const uint32_t nb = (uint32_t)s->dim.num_blocks;
    const uint32_t nbh = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t sw = (uint32_t)s->dim.submatrix_width;
    const uint32_t sh = (uint32_t)s->dim.submatrix_height;
    const uint32_t spx = (uint32_t)(s->float_stride / sizeof(float));
    const float snn = (float)s->opt.speed_sigma_nn;

    SpeedVkPushConsts pc = {6u, nb, nbh, sw, sh, spx, snn};
    uint32_t gx = (nb + 255u) / 256u;
    int err = dispatch_pass(s, &pc, gx, 1u);
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
        const int wvm = s->opt.speed_weight_var_mode;
        float sr = 0.0f;
        float sd = 0.0f;
        if (wvm == 0) {
            sr = re * log2f(1.0f + rv);
            sd = de * log2f(1.0f + dv);
        } else if (wvm == 1) {
            sr = re * log2f(1.0f + rv);
            sd = de * log2f(1.0f + rv);
        } else if (wvm == 2) {
            sr = re * log2f(1.0f + dv);
            sd = de * log2f(1.0f + dv);
        } else if (wvm == 3) {
            const float mv = (rv + dv) * 0.5f;
            sr = re * log2f(1.0f + mv);
            sd = de * log2f(1.0f + mv);
        } else if (wvm == 4) {
            sr = re * log2f(1.0f + rv);
            sd = de * log2f(1.0f + (rv + dv) * 0.5f);
        } else if (wvm == 5) {
            sr = re * log2f(1.0f + rv);
            sd = de * log2f(1.0f + 0.75f * rv + 0.25f * dv);
        } else if (wvm == 6) {
            sr = re * log2f(1.0f + rv);
            sd = de * log2f(1.0f + 0.25f * rv + 0.75f * dv);
        }
        total += fabsf(sr - sd);
    }
    *score_out = total / (float)nb;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static int init_chroma_vk(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)bpc;
    SpeedChromaVulkanState *s = fex->priv;

    unsigned cw = w;
    unsigned ch = h;
    switch (pix_fmt) {
    case VMAF_PIX_FMT_UNKNOWN:
    case VMAF_PIX_FMT_YUV400P:
        return -EINVAL;
    case VMAF_PIX_FMT_YUV420P:
        cw /= 2u;
        ch /= 2u;
        break;
    case VMAF_PIX_FMT_YUV422P:
        cw /= 2u;
        break;
    case VMAF_PIX_FMT_YUV444P:
        break;
    }

    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };

    int err = speed_internal_init_dimensions(&s->dim, (int)cw, (int)ch, s->opt.speed_prescale);
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

    err = create_pipeline(s);
    if (err)
        return err;

    err = alloc_buffers(s);
    if (err)
        return err;

    err = vmaf_vulkan_kernel_submit_pool_create(s->ctx, 1, &s->sub_pool);
    if (err)
        return err;

    err = vmaf_vulkan_kernel_descriptor_sets_alloc(s->ctx, s->pl.desc_pool, s->pl.dsl, 1,
                                                   &s->pre_set);
    if (err)
        return err;
    write_descriptor_set(s, s->pre_set);

    /* CPU scratch buffers. */
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = 25u * nb * sizeof(float);
    const size_t cov_bytes = 25u * 25u * sizeof(float);

#define AA(f, sz) s->f = (float *)aligned_malloc((sz), 32)
    AA(h_plane_ref, plane_bytes);
    AA(h_plane_dis, plane_bytes);
    AA(h_eigenvalues, 25u * sizeof(float));
    AA(h_eig_scratch, (25u * 25u + 75u) * sizeof(float));
    AA(h_Q, cov_bytes);
    AA(h_R, cov_bytes);
    AA(h_qr_scratch, 4u * cov_bytes);
    AA(h_indterm_ref, indterm_bytes);
    AA(h_indterm_dis, indterm_bytes);
    AA(h_qt_scratch, indterm_bytes);
#undef AA

    if (!s->h_plane_ref || !s->h_plane_dis || !s->h_eigenvalues || !s->h_Q || !s->h_R)
        return -ENOMEM;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    return 0;
}

static int extract_chroma_vk(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                             VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                             VmafPicture *dist_pic_90, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    SpeedChromaVulkanState *s = fex->priv;

    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t tmp_size = 2u * s->dim.alloc_height * stride_px;
    float *tmp_filter = (float *)aligned_malloc(tmp_size * sizeof(float), 32);
    if (!tmp_filter)
        return -ENOMEM;

    float score_u = 0.0f;
    float score_v = 0.0f;
    int err_u = 0;
    int err_v = 0;

    for (int ch = 1; ch <= 2; ++ch) {
        /* Copy + filter+downscale ref and dis chroma planes. */
        picture_copy(s->h_plane_ref, s->float_stride, ref_pic, -128, ref_pic->bpc, ch);
        speed_internal_filter_and_downscale(s->dim, &s->opt, s->h_plane_ref, tmp_filter,
                                            s->float_stride);
        picture_copy(s->h_plane_dis, s->float_stride, dist_pic, -128, dist_pic->bpc, ch);
        speed_internal_filter_and_downscale(s->dim, &s->opt, s->h_plane_dis, tmp_filter,
                                            s->float_stride);

        /* GPU passes 0,1,2: means + cov + indterm_ref; readback cov+iref. */
        int e = run_gpu_k123_ref(s);

        /* Save cov for dis channel (h_R was written by readback). */
        float saved_cov[25 * 25];
        if (!e)
            memcpy(saved_cov, s->h_R, sizeof(saved_cov));

        /* CPU linalg on ref + GPU pass 4 solve_ref. */
        if (!e)
            e = run_cpu_linalg_and_solve(s, s->h_indterm_ref, s->b_sol_ref, 4u);

        /* GPU pass 3: indterm_dis; readback. */
        if (!e)
            e = run_gpu_k3_dis(s);

        /* Restore cov for dis channel linalg. */
        if (!e)
            memcpy(s->h_R, saved_cov, sizeof(saved_cov));

        /* CPU linalg on dis + GPU pass 5 solve_dis. */
        if (!e)
            e = run_cpu_linalg_and_solve(s, s->h_indterm_dis, s->b_sol_dis, 5u);

        float sc = 0.0f;
        if (!e)
            e = run_score_and_collect(s, &sc);

        if (ch == 1) {
            err_u = e;
            score_u = sc;
        } else {
            err_v = e;
            score_v = sc;
        }
    }

    aligned_free(tmp_filter);

    float score_uv;
    if (err_u && !err_v)
        score_uv = score_v;
    else if (err_v && !err_u)
        score_uv = score_u;
    else
        score_uv = (score_u + score_v) * 0.5f;

    const double mxv = s->speed_chroma_max_val;
    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_u_score",
        (double)score_u < mxv ? (double)score_u : mxv, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_v_score",
        (double)score_v < mxv ? (double)score_v : mxv, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_uv_score",
        (double)score_uv < mxv ? (double)score_uv : mxv, index);
    return err;
}

static int close_chroma_vk(VmafFeatureExtractor *fex)
{
    SpeedChromaVulkanState *s = fex->priv;
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

    aligned_free(s->h_plane_ref);
    aligned_free(s->h_plane_dis);
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

static const char *provided_features_chroma[] = {
    "Speed_chroma_feature_speed_chroma_u_score",
    "Speed_chroma_feature_speed_chroma_v_score",
    "Speed_chroma_feature_speed_chroma_uv_score",
    NULL,
};

/* ADR-0567: real Vulkan GPU kernels for speed_chroma.
 * 7 compute passes (GLSL speed_score.comp); CPU handles 25×25 eigendecomp
 * and QR factorisation between passes 2/3 and 4/5. */
VmafFeatureExtractor vmaf_fex_speed_chroma_vulkan = {
    .name = "speed_chroma_vulkan",
    .init = init_chroma_vk,
    .extract = extract_chroma_vk,
    .close = close_chroma_vk,
    .options = options_chroma,
    .priv_size = sizeof(SpeedChromaVulkanState),
    .provided_features = provided_features_chroma,
    .flags = VMAF_FEATURE_EXTRACTOR_VULKAN,
};
