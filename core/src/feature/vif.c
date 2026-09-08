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

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: preserve Netflix NULL; MSVC C nullptr support is unverified. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "mem.h"
#include "offset.h"
#include "vif.h"
#include "vif_options.h"
#include "vif_tools.h"

enum { VIF_BUF_CNT = 10, VIF_SCALES = 4 };

typedef struct {
    int stride;
    float *data;
    float *ref_scale;
    float *dis_scale;
    float *mu1;
    float *mu2;
    float *ref_sq_filt;
    float *dis_sq_filt;
    float *ref_dis_filt;
    float *num_array;
    float *den_array;
    float *tmpbuf;
} VifWorkspace;

typedef struct {
    const float *ref;
    const float *dis;
    int w;
    int h;
    int ref_stride;
    int dis_stride;
} VifFrame;

typedef struct {
    double gain_limit;
    double kernelscale;
    double sigma_nsq;
    const float (*filters)[128];
    const int *filter_widths;
} VifSettings;

typedef struct {
    float coefficients[128];
    int width;
} VifFilter;

/* stride is measured in float elements, as supplied by vifdiff(). */
void apply_frame_differencing(const float *current_frame, const float *previous_frame,
                              float *frame_difference, int width, int height, int stride)
{
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            frame_difference[i * stride + j] =
                current_frame[i * stride + j] - previous_frame[i * stride + j];
        }
    }
}

static int vif_plane_size(int w, int h, int *stride, size_t *size)
{
    if (w <= 0 || h <= 0 || (size_t)w > ALIGN_FLOOR(INT_MAX) / sizeof(float))
        return 1;
    *stride = ALIGN_CEIL((size_t)w * sizeof(float));
    if ((size_t)h > SIZE_MAX / (size_t)*stride)
        return 1;
    *size = (size_t)*stride * (size_t)h;
    return 0;
}

static int vif_workspace_alloc(VifWorkspace *work, int w, int h)
{
    size_t plane_size;
    if (vif_plane_size(w, h, &work->stride, &plane_size))
        return 1;
    if (SIZE_MAX / plane_size < VIF_BUF_CNT) {
        printf("error: SIZE_MAX / buf_sz_one < VIF_BUF_CNT, buf_sz_one = %zu.\n", plane_size);
        (void)fflush(stdout);
        return 1;
    }
    work->data = aligned_malloc(plane_size * VIF_BUF_CNT, MAX_ALIGN);
    if (!work->data) {
        printf("error: aligned_malloc failed for data_buf.\n");
        (void)fflush(stdout);
        return 1;
    }
    /* One aligned slab, in the original order. Every plane contains a whole
     * number of floats; typed offsets preserve alignment without char casts. */
    const size_t plane_floats = plane_size / sizeof(float);
    work->ref_scale = work->data;
    work->dis_scale = work->ref_scale + plane_floats;
    work->mu1 = work->dis_scale + plane_floats;
    work->mu2 = work->mu1 + plane_floats;
    work->ref_sq_filt = work->mu2 + plane_floats;
    work->dis_sq_filt = work->ref_sq_filt + plane_floats;
    work->ref_dis_filt = work->dis_sq_filt + plane_floats;
    work->num_array = work->ref_dis_filt + plane_floats;
    work->den_array = work->num_array + plane_floats;
    work->tmpbuf = work->den_array + plane_floats;
    return 0;
}

static void vif_prepare_filter(VifFilter *filter, unsigned scale, const VifSettings *settings)
{
    if (settings->filters && settings->filter_widths) {
        /* ADR-0500: preserve the caller's precomputed Gaussian coefficients. */
        filter->width = settings->filter_widths[scale];
        memcpy(filter->coefficients, settings->filters[scale],
               (size_t)filter->width * sizeof(filter->coefficients[0]));
    } else {
        filter->width = vif_get_filter_size(scale, settings->kernelscale);
        vif_get_filter(filter->coefficients, scale, settings->kernelscale);
    }
}

static int vif_filter_border(int filter_width)
{
#ifdef VIF_OPT_HANDLE_BORDERS
    (void)filter_width;
    return 0;
#else
    return filter_width / 2;
#endif
}

static void vif_downsample(VifFrame *frame, VifWorkspace *work, const VifFilter *filter)
{
    vif_filter1d_s(filter->coefficients, frame->ref, work->mu1, work->tmpbuf, frame->w, frame->h,
                   frame->ref_stride, work->stride, filter->width);
    vif_filter1d_s(filter->coefficients, frame->dis, work->mu2, work->tmpbuf, frame->w, frame->h,
                   frame->dis_stride, work->stride, filter->width);
    const int border = vif_filter_border(filter->width);
    const int valid_w = frame->w - border * 2;
    const int valid_h = frame->h - border * 2;
    const size_t offset = (size_t)border * (work->stride / sizeof(float)) + border;
    vif_dec2_s(work->mu1 + offset, work->ref_scale, valid_w, valid_h, work->stride, work->stride);
    vif_dec2_s(work->mu2 + offset, work->dis_scale, valid_w, valid_h, work->stride, work->stride);
    frame->w = valid_w / 2;
    frame->h = valid_h / 2;
    frame->ref = work->ref_scale;
    frame->dis = work->dis_scale;
    frame->ref_stride = work->stride;
    frame->dis_stride = work->stride;
}

static void vif_compute_scale(const VifFrame *frame, VifWorkspace *work, const VifFilter *filter,
                              const VifSettings *settings)
{
    vif_filter1d_s(filter->coefficients, frame->ref, work->mu1, work->tmpbuf, frame->w, frame->h,
                   frame->ref_stride, work->stride, filter->width);
    vif_filter1d_s(filter->coefficients, frame->dis, work->mu2, work->tmpbuf, frame->w, frame->h,
                   frame->dis_stride, work->stride, filter->width);
    vif_filter1d_sq_s(filter->coefficients, frame->ref, work->ref_sq_filt, work->tmpbuf, frame->w,
                      frame->h, frame->ref_stride, work->stride, filter->width);
    vif_filter1d_sq_s(filter->coefficients, frame->dis, work->dis_sq_filt, work->tmpbuf, frame->w,
                      frame->h, frame->dis_stride, work->stride, filter->width);
    vif_filter1d_xy_s(filter->coefficients, frame->ref, frame->dis, work->ref_dis_filt,
                      work->tmpbuf, frame->w, frame->h, frame->ref_stride, frame->dis_stride,
                      work->stride, filter->width);
    vif_statistic_s(work->mu1, work->mu2, work->ref_sq_filt, work->dis_sq_filt, work->ref_dis_filt,
                    work->num_array, work->den_array, frame->w, frame->h, work->stride,
                    work->stride, work->stride, work->stride, work->stride, settings->gain_limit,
                    settings->sigma_nsq);
}

#ifdef VIF_OPT_DEBUG_DUMP
static void vif_dump_plane(const char *name, unsigned scale, const float *data, int w, int h,
                           int stride)
{
    char path[256];
    const int length = snprintf(path, sizeof(path), "stage/%s[%u].bin", name, scale);
    if (length < 0 || (size_t)length >= sizeof(path))
        return;
    FILE *file = fopen(path, "wb");
    if (!file) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not open VIF debug dump: %s\n", path);
        return;
    }
    for (int row = 0; row < h; ++row) {
        if (fwrite(data + (size_t)row * (stride / sizeof(float)), sizeof(float), (size_t)w, file) !=
            (size_t)w) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not write VIF debug dump: %s\n", path);
            break;
        }
    }
    if (fclose(file))
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not close VIF debug dump: %s\n", path);
}

static void vif_dump_scale(const VifFrame *frame, const VifWorkspace *work, const VifFilter *filter,
                           unsigned scale)
{
    const int border = vif_filter_border(filter->width);
    const int w = frame->w - border * 2;
    const int h = frame->h - border * 2;
    const size_t offset = (size_t)border * (work->stride / sizeof(float)) + border;
    vif_dump_plane("ref", scale, frame->ref, frame->w, frame->h, frame->ref_stride);
    vif_dump_plane("dis", scale, frame->dis, frame->w, frame->h, frame->dis_stride);
    vif_dump_plane("mu1", scale, work->mu1 + offset, w, h, work->stride);
    vif_dump_plane("mu2", scale, work->mu2 + offset, w, h, work->stride);
    vif_dump_plane("ref_sq_filt", scale, work->ref_sq_filt + offset, w, h, work->stride);
    vif_dump_plane("dis_sq_filt", scale, work->dis_sq_filt + offset, w, h, work->stride);
    vif_dump_plane("ref_dis_filt", scale, work->ref_dis_filt + offset, w, h, work->stride);
    /* vif_statistic_s writes one reduced float per output, not image planes. */
    vif_dump_plane("num_array", scale, work->num_array, 1, 1, sizeof(float));
    vif_dump_plane("den_array", scale, work->den_array, 1, 1, sizeof(float));
}
#endif

int compute_vif(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride,
                double *score, double *score_num, double *score_den, double *scores,
                double vif_enhn_gain_limit, double vif_kernelscale, int vif_skip_scale0,
                double vif_sigma_nsq, const float (*precomputed_filters)[128],
                const int *precomputed_filter_widths)
{
    if (!precomputed_filters && !vif_validate_kernelscale(vif_kernelscale)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "invalid vif_kernelscale: %f", vif_kernelscale);
        return 1;
    }
    VifWorkspace work = {0};
    if (vif_workspace_alloc(&work, w, h))
        return 1;
    VifFrame frame = {ref, dis, w, h, ref_stride, dis_stride};
    const VifSettings settings = {vif_enhn_gain_limit, vif_kernelscale, vif_sigma_nsq,
                                  precomputed_filters, precomputed_filter_widths};
    const unsigned scale_start = vif_skip_scale0 ? 1 : 0;
    for (unsigned scale = scale_start; scale < VIF_SCALES; ++scale) {
        VifFilter filter;
        vif_prepare_filter(&filter, scale, &settings);
        if (scale > 0)
            vif_downsample(&frame, &work, &filter);
        vif_compute_scale(&frame, &work, &filter, &settings);
#ifdef VIF_OPT_DEBUG_DUMP
        vif_dump_scale(&frame, &work, &filter, scale);
#endif
        const float num = *work.num_array;
        const float den = *work.den_array;
        scores[(size_t)2 * scale] = num;
        scores[(size_t)2 * scale + 1] = den;
#ifdef VIF_OPT_DEBUG_DUMP
        printf("num[%u]: %e\n", scale, num);
        printf("den[%u]: %e\n", scale, den);
#endif
    }
    *score_num = 0.0;
    *score_den = 0.0;
    for (unsigned scale = scale_start; scale < VIF_SCALES; ++scale) {
        *score_num += scores[(size_t)2 * scale];
        *score_den += scores[(size_t)2 * scale + 1];
    }
    *score = *score_den == 0.0 ? 1.0f : (*score_num) / (*score_den);
    aligned_free(work.data);
    return 0;
}

enum {
    VIFDIFF_REF,
    VIFDIFF_REF_DIFF,
    VIFDIFF_PREV_REF,
    VIFDIFF_DIS,
    VIFDIFF_DIS_DIFF,
    VIFDIFF_PREV_DIS,
    VIFDIFF_TEMP,
    VIFDIFF_BUF_CNT
};

typedef struct {
    int stride;
    size_t data_size;
    float *buffers[VIFDIFF_BUF_CNT];
    double score;
    double numerator;
    double denominator;
    double scores[VIF_SCALES * 2];
} VifDiffWorkspace;

static int vifdiff_alloc(VifDiffWorkspace *work, int w, int h)
{
    if (vif_plane_size(w, h, &work->stride, &work->data_size) || work->data_size > SIZE_MAX / 2)
        return 1;
    static const char *const names[] = {"ref_buf",      "ref_diff_buf", "prev_ref_buf", "dis_buf",
                                        "dis_diff_buf", "prev_dis_buf", "temp_buf"};
    for (unsigned i = 0; i < VIFDIFF_BUF_CNT; ++i) {
        const size_t size = work->data_size * (i == VIFDIFF_TEMP ? 2 : 1);
        work->buffers[i] = aligned_malloc(size, MAX_ALIGN);
        if (!work->buffers[i]) {
            printf("error: aligned_malloc failed for %s.\n", names[i]);
            (void)fflush(stdout);
            return 1;
        }
    }
    return 0;
}

static void vifdiff_prepare_frame(VifDiffWorkspace *work, int w, int h, int frame_index)
{
    offset_image_s(work->buffers[VIFDIFF_REF], OPT_RANGE_PIXEL_OFFSET, w, h, work->stride);
    offset_image_s(work->buffers[VIFDIFF_DIS], OPT_RANGE_PIXEL_OFFSET, w, h, work->stride);
    if (frame_index > 0) {
        apply_frame_differencing(work->buffers[VIFDIFF_REF], work->buffers[VIFDIFF_PREV_REF],
                                 work->buffers[VIFDIFF_REF_DIFF], w, h,
                                 work->stride / sizeof(float));
        apply_frame_differencing(work->buffers[VIFDIFF_DIS], work->buffers[VIFDIFF_PREV_DIS],
                                 work->buffers[VIFDIFF_DIS_DIFF], w, h,
                                 work->stride / sizeof(float));
    }
    memcpy(work->buffers[VIFDIFF_PREV_REF], work->buffers[VIFDIFF_REF], work->data_size);
    memcpy(work->buffers[VIFDIFF_PREV_DIS], work->buffers[VIFDIFF_DIS], work->data_size);
}

static int vifdiff_compute_frame(VifDiffWorkspace *work, int w, int h, int frame_index)
{
    if (frame_index == 0) {
        /* Preserve the first-frame placeholder: no preceding frame is available. */
        work->score = 0.0;
        work->numerator = 0.0;
        work->denominator = 0.0;
        for (size_t scale = 0; scale < VIF_SCALES; ++scale) {
            work->scores[2 * scale] = 0.0;
            work->scores[2 * scale + 1] = 0.0 + 1e-5;
        }
        return 0;
    }
    const int ret = compute_vif(work->buffers[VIFDIFF_REF_DIFF], work->buffers[VIFDIFF_DIS_DIFF], w,
                                h, work->stride, work->stride, &work->score, &work->numerator,
                                &work->denominator, work->scores, DEFAULT_VIF_ENHN_GAIN_LIMIT,
                                DEFAULT_VIF_KERNELSCALE, 0, 2.0, NULL, NULL);
    if (ret) {
        printf("error: compute_vifdiff failed.\n");
        (void)fflush(stdout);
    }
    return ret;
}

static void vifdiff_print_frame(const VifDiffWorkspace *work, int frame_index)
{
    printf("vifdiff: %d %f\n", frame_index, work->score);
    (void)fflush(stdout);
    printf("vifdiff_num: %d %f\n", frame_index, work->numerator);
    (void)fflush(stdout);
    printf("vifdiff_den: %d %f\n", frame_index, work->denominator);
    (void)fflush(stdout);
    for (size_t scale = 0; scale < VIF_SCALES; ++scale) {
        printf("vifdiff_num_scale%zu: %d %f\n", scale, frame_index, work->scores[2 * scale]);
        printf("vifdiff_den_scale%zu: %d %f\n", scale, frame_index, work->scores[2 * scale + 1]);
    }
}

int vifdiff(int (*read_frame)(float *ref_data, float *main_data, float *temp_data, int stride,
                              void *user_data),
            void *user_data, int w, int h, const char *fmt)
{
    (void)fmt;
    VifDiffWorkspace work = {0};
    int ret = vifdiff_alloc(&work, w, h);
    if (!ret) {
        for (int frame_index = 0;; ++frame_index) {
            ret = read_frame(work.buffers[VIFDIFF_REF], work.buffers[VIFDIFF_DIS],
                             work.buffers[VIFDIFF_TEMP], work.stride, user_data);
            if (ret == 1)
                break;
            if (ret == 2) {
                ret = 0;
                break;
            }
            vifdiff_prepare_frame(&work, w, h, frame_index);
            ret = vifdiff_compute_frame(&work, w, h, frame_index);
            if (ret)
                break;
            vifdiff_print_frame(&work, frame_index);
        }
    }
    for (unsigned i = 0; i < VIFDIFF_BUF_CNT; ++i)
        aligned_free(work.buffers[i]);
    return ret;
}

/* NOLINTEND(modernize-use-nullptr) */
