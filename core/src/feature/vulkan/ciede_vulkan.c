/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  ciede2000 (ΔE) feature kernel on the Vulkan backend (T7-23 /
 *  ADR-0182, GPU long-tail batch 1c part 1). Vulkan twin of the
 *  CPU ciede2000 extractor in libvmaf/src/feature/ciede.c.
 *
 *  Single dispatch per frame. Each workgroup computes the partial
 *  sum of per-pixel ΔE values and writes one float to the partials
 *  buffer; the host sums those partials in `double` and divides
 *  by w*h to recover the average ΔE that ciede.c emits as the
 *  `ciede2000` metric.
 *
 *  Precision contract: places=2 vs CPU scalar reference (the
 *  per-pixel math involves transcendentals — pow/sqrt/sin/atan2 —
 *  which the GPU evaluates at lower-than-libm precision). See
 *  ADR-0187. Unlike the integer extractors (psnr / motion / moment),
 *  bit-exactness is not the contract here.
 *
 *  Chroma handling: matches CPU. YUV400P is rejected at init.
 *  YUV444P is consumed directly. YUV422P / YUV420P are upscaled
 *  to luma resolution on the host before upload — same logic as
 *  ciede.c::scale_chroma_planes.
 *
 *  Submit-pool migration (T-GPU-OPT-VK-1 / ADR-0353 / PR-B):
 *  Replaced per-frame vkAllocateCommandBuffers + vkCreateFence +
 *  vkAllocateDescriptorSets with:
 *    - vmaf_vulkan_kernel_submit_pool_create / _acquire / _end_and_wait
 *    - vmaf_vulkan_kernel_descriptor_sets_alloc (one set at init)
 *  All 7 SSBO bindings are init-time-stable; no per-frame
 *  vkUpdateDescriptorSets needed. (T-GPU-OPT-VK-4 / ADR-0256.)
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"

#include "../../vulkan/vulkan_common.h"
#include "../../vulkan/picture_vulkan.h"
#include "../../vulkan/vulkan_internal.h"
#include "../../vulkan/kernel_template.h"

#include "ciede_spv.h" /* generated SPIR-V byte array */

#define CIEDE_WG_X 16
#define CIEDE_WG_Y 8
#define CIEDE_NUM_BINDINGS 7 /* 6 input planes + 1 partial-sums output */

typedef struct {
    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    enum VmafPixelFormat pix_fmt;

    uint32_t wg_count_x;
    uint32_t wg_count_y;
    uint32_t wg_count;

    /* Vulkan context handle. Borrow on imported state, lazy-create otherwise. */
    VmafVulkanContext *ctx;
    int owns_ctx;

    /* Pipeline objects (`vulkan/kernel_template.h` bundle, ADR-0246). */
    VmafVulkanKernelPipeline pl;

    /* Per-frame submit pool (T-GPU-OPT-VK-1 / ADR-0353).
     * Single slot: the single dispatch uses one command buffer. */
    VmafVulkanKernelSubmitPool sub_pool;

    /* Pre-allocated descriptor set reused across frames.
     * All 7 SSBO bindings are init-time-stable; written once at
     * init() — no per-frame vkUpdateDescriptorSets needed.
     * (T-GPU-OPT-VK-4 / ADR-0353.) */
    VkDescriptorSet pre_set;

    /* Per-plane input buffers (host-mapped). Six planes — Y/U/V
     * × ref/dis — each at full luma resolution. */
    VmafVulkanBuffer *ref_y_in;
    VmafVulkanBuffer *ref_u_in;
    VmafVulkanBuffer *ref_v_in;
    VmafVulkanBuffer *dis_y_in;
    VmafVulkanBuffer *dis_u_in;
    VmafVulkanBuffer *dis_v_in;

    /* Per-WG float partial-sums output. */
    VmafVulkanBuffer *partials;

    VmafDictionary *feature_name_dict;
} CiedeVulkanState;

static const VmafOption options[] = {{0}};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bpc;
    uint32_t num_workgroups_x;
} CiedePushConsts;

static int create_pipeline(CiedeVulkanState *s)
{
    struct {
        int32_t width;
        int32_t height;
        int32_t bpc;
        int32_t subgroup_size;
    } spec_data = {(int32_t)s->width, (int32_t)s->height, (int32_t)s->bpc, 32};

    VkSpecializationMapEntry spec_entries[4] = {
        {.constantID = 0,
         .offset = offsetof(__typeof__(spec_data), width),
         .size = sizeof(int32_t)},
        {.constantID = 1,
         .offset = offsetof(__typeof__(spec_data), height),
         .size = sizeof(int32_t)},
        {.constantID = 2, .offset = offsetof(__typeof__(spec_data), bpc), .size = sizeof(int32_t)},
        {.constantID = 3,
         .offset = offsetof(__typeof__(spec_data), subgroup_size),
         .size = sizeof(int32_t)},
    };
    VkSpecializationInfo spec_info = {
        .mapEntryCount = 4,
        .pMapEntries = spec_entries,
        .dataSize = sizeof(spec_data),
        .pData = &spec_data,
    };

    /* `vulkan/kernel_template.h` (ADR-0246) owns the descriptor-set
     * layout (CIEDE_NUM_BINDINGS = 7 SSBO bindings: 6 input planes +
     * 1 partial-sums output), pipeline layout, shader module, compute
     * pipeline, and descriptor pool sizing. */
    const VmafVulkanKernelPipelineDesc desc = {
        .ssbo_binding_count = (uint32_t)CIEDE_NUM_BINDINGS,
        .push_constant_size = (uint32_t)sizeof(CiedePushConsts),
        .spv_bytes = ciede_spv,
        .spv_size = ciede_spv_size,
        .pipeline_create_info =
            {
                .stage =
                    {
                        .pName = "main",
                        .pSpecializationInfo = &spec_info,
                    },
            },
        .max_descriptor_sets = 4U,
    };
    return vmaf_vulkan_kernel_pipeline_create(s->ctx, &desc, &s->pl);
}

static int alloc_buffers(CiedeVulkanState *s)
{
    size_t bytes_per_pixel = (s->bpc <= 8) ? 1 : 2;
    size_t plane_bytes = (size_t)s->width * s->height * bytes_per_pixel;

    int err = 0;
    err |= vmaf_vulkan_buffer_alloc(s->ctx, &s->ref_y_in, plane_bytes);
    err |= vmaf_vulkan_buffer_alloc(s->ctx, &s->ref_u_in, plane_bytes);
    err |= vmaf_vulkan_buffer_alloc(s->ctx, &s->ref_v_in, plane_bytes);
    err |= vmaf_vulkan_buffer_alloc(s->ctx, &s->dis_y_in, plane_bytes);
    err |= vmaf_vulkan_buffer_alloc(s->ctx, &s->dis_u_in, plane_bytes);
    err |= vmaf_vulkan_buffer_alloc(s->ctx, &s->dis_v_in, plane_bytes);
    if (err)
        return -ENOMEM;

    /* Per-WG float partial sum. */
    size_t partials_bytes = (size_t)s->wg_count * sizeof(float);
    err = vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->partials, partials_bytes);
    return err;
}

static int write_descriptor_set(CiedeVulkanState *s, VkDescriptorSet set)
{
    VkDescriptorBufferInfo dbi[CIEDE_NUM_BINDINGS] = {
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->ref_y_in),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->ref_u_in),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->ref_v_in),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->dis_y_in),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->dis_u_in),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->dis_v_in),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->partials),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[CIEDE_NUM_BINDINGS];
    for (int i = 0; i < CIEDE_NUM_BINDINGS; i++) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = (uint32_t)i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &dbi[i],
        };
    }
    vkUpdateDescriptorSets(s->ctx->device, CIEDE_NUM_BINDINGS, writes, 0, NULL);
    return 0;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    if (pix_fmt == VMAF_PIX_FMT_YUV400P)
        return -EINVAL;

    CiedeVulkanState *s = fex->priv;
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->pix_fmt = pix_fmt;
    s->wg_count_x = (w + CIEDE_WG_X - 1) / CIEDE_WG_X;
    s->wg_count_y = (h + CIEDE_WG_Y - 1) / CIEDE_WG_Y;
    s->wg_count = s->wg_count_x * s->wg_count_y;

    s->ctx = vmaf_vulkan_state_get_context(fex->vulkan_state);
    if (s->ctx) {
        s->owns_ctx = 0;
    } else {
        int err = vmaf_vulkan_context_new(&s->ctx, /*device_index=*/-1);
        if (err) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "ciede_vulkan: cannot create Vulkan context (%d)\n",
                     err);
            return err;
        }
        s->owns_ctx = 1;
    }

    int err = create_pipeline(s);
    if (err)
        return err;
    err = alloc_buffers(s);
    if (err)
        return err;

    /* Pre-allocate submit pool and the single descriptor set.
     * All 7 SSBO bindings are init-time-stable; write once here
     * and reuse every frame — no per-frame vkUpdateDescriptorSets.
     * (T-GPU-OPT-VK-1 + T-GPU-OPT-VK-4 / ADR-0353.) */
    err = vmaf_vulkan_kernel_submit_pool_create(s->ctx, /*slot_count=*/1, &s->sub_pool);
    if (err)
        return err;
    err = vmaf_vulkan_kernel_descriptor_sets_alloc(s->ctx, s->pl.desc_pool, s->pl.dsl,
                                                   /*count=*/1, &s->pre_set);
    if (err)
        return err;
    (void)write_descriptor_set(s, s->pre_set);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    return 0;
}

/* Mirror of ciede.c::scale_chroma_planes — upscales chroma to
 * luma resolution. Writes directly into the GPU-mapped staging
 * buffers (HOST_VISIBLE) at width-strided layout. The plane
 * index `p` matches VmafPicture's: 0=Y, 1=U, 2=V. The luma plane
 * is copied straight (1:1); chroma planes are nearest-neighbour
 * upscaled from their subsampled grid. */
static void upscale_plane_8(unsigned p, const VmafPicture *pic, void *dst, unsigned out_w,
                            unsigned out_h, enum VmafPixelFormat pix_fmt)
{
    const int ss_hor = (p > 0u) && (pix_fmt != VMAF_PIX_FMT_YUV444P);
    const int ss_ver = (p > 0u) && (pix_fmt == VMAF_PIX_FMT_YUV420P);
    const uint8_t *in_buf = pic->data[p];
    uint8_t *out_buf = dst;
    for (unsigned i = 0; i < out_h; i++) {
        for (unsigned j = 0; j < out_w; j++) {
            unsigned in_x = ss_hor ? (j >> 1) : j;
            out_buf[j] = in_buf[in_x];
        }
        unsigned in_row_step = ss_ver ? (i & 1u) : 1u;
        in_buf += in_row_step * pic->stride[p];
        out_buf += out_w;
    }
}

static void upscale_plane_16(unsigned p, const VmafPicture *pic, void *dst, unsigned out_w,
                             unsigned out_h, enum VmafPixelFormat pix_fmt)
{
    const int ss_hor = (p > 0u) && (pix_fmt != VMAF_PIX_FMT_YUV444P);
    const int ss_ver = (p > 0u) && (pix_fmt == VMAF_PIX_FMT_YUV420P);
    const uint16_t *in_buf = pic->data[p];
    uint16_t *out_buf = dst;
    const ptrdiff_t in_stride16 = pic->stride[p] / 2;
    for (unsigned i = 0; i < out_h; i++) {
        for (unsigned j = 0; j < out_w; j++) {
            unsigned in_x = ss_hor ? (j >> 1) : j;
            out_buf[j] = in_buf[in_x];
        }
        unsigned in_row_step = ss_ver ? (i & 1u) : 1u;
        in_buf += in_row_step * in_stride16;
        out_buf += out_w;
    }
}

static int upload_pic(CiedeVulkanState *s, VmafVulkanBuffer *y_buf, VmafVulkanBuffer *u_buf,
                      VmafVulkanBuffer *v_buf, VmafPicture *pic)
{
    void *y_dst = vmaf_vulkan_buffer_host(y_buf);
    void *u_dst = vmaf_vulkan_buffer_host(u_buf);
    void *v_dst = vmaf_vulkan_buffer_host(v_buf);
    if (s->bpc <= 8) {
        upscale_plane_8(0, pic, y_dst, s->width, s->height, s->pix_fmt);
        upscale_plane_8(1, pic, u_dst, s->width, s->height, s->pix_fmt);
        upscale_plane_8(2, pic, v_dst, s->width, s->height, s->pix_fmt);
    } else {
        upscale_plane_16(0, pic, y_dst, s->width, s->height, s->pix_fmt);
        upscale_plane_16(1, pic, u_dst, s->width, s->height, s->pix_fmt);
        upscale_plane_16(2, pic, v_dst, s->width, s->height, s->pix_fmt);
    }
    int err = 0;
    err |= vmaf_vulkan_buffer_flush(s->ctx, y_buf);
    err |= vmaf_vulkan_buffer_flush(s->ctx, u_buf);
    err |= vmaf_vulkan_buffer_flush(s->ctx, v_buf);
    return err;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    CiedeVulkanState *s = fex->priv;
    int err = 0;

    err = upload_pic(s, s->ref_y_in, s->ref_u_in, s->ref_v_in, ref_pic);
    if (err)
        return err;
    err = upload_pic(s, s->dis_y_in, s->dis_u_in, s->dis_v_in, dist_pic);
    if (err)
        return err;

    /* Zero the partials buffer before each dispatch. */
    memset(vmaf_vulkan_buffer_host(s->partials), 0, (size_t)s->wg_count * sizeof(float));
    err = vmaf_vulkan_buffer_flush(s->ctx, s->partials);
    if (err)
        return err;

    /* All 7 SSBO bindings are init-time-stable; no per-frame
     * vkUpdateDescriptorSets needed (T-GPU-OPT-VK-4 / ADR-0353). */

    /* Acquire a pre-allocated command buffer + fence from the submit
     * pool. Eliminates per-frame vkAllocateCommandBuffers +
     * vkCreateFence + vkAllocateDescriptorSets.
     * (T-GPU-OPT-VK-1 / ADR-0353.) */
    VmafVulkanKernelSubmit submit = {0};
    err = vmaf_vulkan_kernel_submit_acquire(s->ctx, &s->sub_pool, /*pool_slot=*/0, &submit);
    if (err)
        return err;
    VkCommandBuffer cmd = submit.cmd;

    CiedePushConsts pc = {
        .width = s->width,
        .height = s->height,
        .bpc = s->bpc,
        .num_workgroups_x = s->wg_count_x,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pl.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pl.pipeline_layout, 0, 1,
                            &s->pre_set, 0, NULL);
    vkCmdPushConstants(cmd, s->pl.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, s->wg_count_x, s->wg_count_y, 1);

    /* End recording, submit and wait synchronously. */
    err = vmaf_vulkan_kernel_submit_end_and_wait(s->ctx, &submit);
    if (err)
        goto cleanup;

    /* Host-side reduction in `double` to retain places=2 precision
     * across the per-WG float partials. See ADR-0187. The CPU
     * reference (libvmaf/src/feature/ciede.c) emits a logarithmic
     * score `45 - 20*log10(mean_dE)` rather than the raw mean —
     * mirror that here so the `ciede2000` metric matches across
     * backends. */
    int err_inv = vmaf_vulkan_buffer_invalidate(s->ctx, s->partials);
    if (err_inv)
        return err_inv;
    {
        const float *partials = vmaf_vulkan_buffer_host(s->partials);
        double total = 0.0;
        for (unsigned i = 0; i < s->wg_count; i++)
            total += (double)partials[i];
        const double n_pixels = (double)s->width * (double)s->height;
        const double mean_de = total / n_pixels;
        const double score = 45.0 - 20.0 * log10(mean_de);

        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "ciede2000", score, index);
    }

cleanup:
    /* Pool-owned submit: submit_free clears local handles only; pool
     * retains the fence + cmd buffer for reuse next frame. */
    vmaf_vulkan_kernel_submit_free(s->ctx, &submit);
    return err;
}

static int close_fex(VmafFeatureExtractor *fex)
{
    CiedeVulkanState *s = fex->priv;
    if (!s->ctx)
        return 0;

    /* Drain the submit pool before the pipeline (ADR-0353).
     * `vulkan/kernel_template.h` collapses the vkDeviceWaitIdle +
     * 5×vkDestroy* sweep into one call. Descriptor sets pre-allocated
     * via descriptor_sets_alloc are freed implicitly with the pool —
     * do NOT call vkFreeDescriptorSets on pre_set. */
    vmaf_vulkan_kernel_submit_pool_destroy(s->ctx, &s->sub_pool);
    vmaf_vulkan_kernel_pipeline_destroy(s->ctx, &s->pl);

    if (s->ref_y_in)
        vmaf_vulkan_buffer_free(s->ctx, s->ref_y_in);
    if (s->ref_u_in)
        vmaf_vulkan_buffer_free(s->ctx, s->ref_u_in);
    if (s->ref_v_in)
        vmaf_vulkan_buffer_free(s->ctx, s->ref_v_in);
    if (s->dis_y_in)
        vmaf_vulkan_buffer_free(s->ctx, s->dis_y_in);
    if (s->dis_u_in)
        vmaf_vulkan_buffer_free(s->ctx, s->dis_u_in);
    if (s->dis_v_in)
        vmaf_vulkan_buffer_free(s->ctx, s->dis_v_in);
    if (s->partials)
        vmaf_vulkan_buffer_free(s->ctx, s->partials);

    if (s->owns_ctx)
        vmaf_vulkan_context_destroy(s->ctx);
    s->ctx = NULL;

    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);

    return 0;
}

static const char *provided_features[] = {"ciede2000", NULL};

VmafFeatureExtractor vmaf_fex_ciede_vulkan = {
    .name = "ciede_vulkan",
    .init = init,
    .extract = extract,
    .close = close_fex,
    .options = options,
    .priv_size = sizeof(CiedeVulkanState),
    .flags = VMAF_FEATURE_EXTRACTOR_VULKAN,
    .provided_features = provided_features,
    /* 1 dispatch/frame, per-pixel transcendentals + reduction.
     * AUTO + 1080p area threshold matches motion's profile
     * (ADR-0181 / ADR-0182). */
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
