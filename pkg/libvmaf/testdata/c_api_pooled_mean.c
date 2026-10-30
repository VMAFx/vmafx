/*
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * c_api_pooled_mean.c: the libvmaf C API reference for the Go bit-exactness
 * tests of pkg/libvmaf (#2434).
 *
 * Scores a raw planar YUV pair with a model through the public libvmaf C API
 * only (vmaf_init, vmaf_model_load_from_path, vmaf_use_features_from_model,
 * vmaf_read_pictures, vmaf_score_pooled) and prints the pooled score of the
 * requested method as a C99 hexadecimal float ("%a"), which a Go test reads back
 * with strconv.ParseFloat and compares by IEEE-754 bits.
 *
 * usage: c_api_pooled_mean REF DIS MODEL WIDTH HEIGHT PIXFMT BPC POOL
 *   PIXFMT is the VmafPixelFormat value (1 = 420, 2 = 422, 3 = 444) and POOL
 *   the VmafPoolingMethod value (3 = mean).
 * exit status: 0 on success, 1 on any error (reported on stderr).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>

enum { PLANES = 3, MAX_FRAMES = 4096 };

/* Reads one frame into pic, row by row. Returns the bytes read; 0 on a clean
 * end-of-file before the first byte, -1 on a short frame. */
static long read_frame(FILE *file, VmafPicture *pic)
{
    long total = 0;
    const size_t sample = pic->bpc > 8 ? 2u : 1u;
    for (int plane = 0; plane < PLANES; plane++) {
        const size_t row_bytes = (size_t)pic->w[plane] * sample;
        unsigned char *row = pic->data[plane];
        for (unsigned y = 0; y < pic->h[plane]; y++) {
            const size_t got = fread(row, 1, row_bytes, file);
            total += (long)got;
            if (got != row_bytes) {
                return total == 0 ? 0 : -1;
            }
            row += pic->stride[plane];
        }
    }
    return total;
}

static int fail(const char *what, int rc)
{
    (void)fprintf(stderr, "c_api_pooled_mean: %s failed (%d)\n", what, rc);
    return 1;
}

typedef struct {
    unsigned width;
    unsigned height;
    unsigned bpc;
    enum VmafPixelFormat pix_fmt;
} Geometry;

/* Reads and queues one pair. Returns 0 when queued, 1 on a clean end of both
 * files, -1 on an error (already reported). */
static int feed_one(VmafContext *vmaf, FILE *fr, FILE *fd, const Geometry *g, unsigned index)
{
    VmafPicture ref;
    VmafPicture dis;
    if (vmaf_picture_alloc(&ref, g->pix_fmt, g->bpc, g->width, g->height) != 0) {
        (void)fail("vmaf_picture_alloc(ref)", 0);
        return -1;
    }
    if (vmaf_picture_alloc(&dis, g->pix_fmt, g->bpc, g->width, g->height) != 0) {
        (void)vmaf_picture_unref(&ref);
        (void)fail("vmaf_picture_alloc(dis)", 0);
        return -1;
    }
    const long nr = read_frame(fr, &ref);
    const long nd = read_frame(fd, &dis);
    if (nr <= 0 || nd <= 0 || nr != nd) {
        (void)vmaf_picture_unref(&ref);
        (void)vmaf_picture_unref(&dis);
        if (nr == 0 && nd == 0) {
            return 1;
        }
        (void)fail("short frame", 0);
        return -1;
    }
    const int rc = vmaf_read_pictures(vmaf, &ref, &dis, index);
    if (rc != 0) {
        (void)fail("vmaf_read_pictures", rc);
        return -1;
    }
    return 0;
}

/* Feeds every frame pair of the two files; stores the frame count in *frames. */
static int feed(VmafContext *vmaf, FILE *fr, FILE *fd, const Geometry *g, unsigned *frames)
{
    for (unsigned index = 0; index < MAX_FRAMES; index++) {
        const int rc = feed_one(vmaf, fr, fd, g, index);
        if (rc < 0) {
            return 1;
        }
        if (rc > 0) {
            *frames = index;
            return 0;
        }
    }
    return fail("more frames than the harness bound", MAX_FRAMES);
}

/* Loads the model, scores the pair and prints the pooled value. */
static int score_with_model(VmafContext *vmaf, VmafModel *model, FILE *fr, FILE *fd,
                            const Geometry *g, int pool)
{
    unsigned frames = 0;
    double value = 0.0;
    int rc = vmaf_use_features_from_model(vmaf, model);
    if (rc != 0) {
        return fail("vmaf_use_features_from_model", rc);
    }
    if (feed(vmaf, fr, fd, g, &frames) != 0) {
        return 1;
    }
    if (frames == 0) {
        return fail("no frames", 0);
    }
    rc = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (rc != 0) {
        return fail("vmaf_read_pictures(flush)", rc);
    }
    rc = vmaf_score_pooled(vmaf, model, (enum VmafPoolingMethod)pool, &value, 0, frames - 1);
    if (rc != 0) {
        return fail("vmaf_score_pooled", rc);
    }
    return printf("%a\n", value) < 0 ? fail("printf", 0) : 0;
}

static int score(const char *model_path, FILE *fr, FILE *fd, const Geometry *g, int pool)
{
    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_WARNING,
        .n_threads = 0,
        .n_subsample = 1,
        .cpumask = 0,
        .gpumask = 0,
    };
    VmafContext *vmaf = NULL;
    VmafModel *model = NULL;
    VmafModelConfig mcfg = {.name = "vmaf_c_api", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    int rc = vmaf_init(&vmaf, cfg);
    if (rc != 0) {
        return fail("vmaf_init", rc);
    }
    int status = 1;
    rc = vmaf_model_load_from_path(&model, &mcfg, model_path);
    if (rc != 0) {
        (void)fail("vmaf_model_load_from_path", rc);
    } else {
        status = score_with_model(vmaf, model, fr, fd, g, pool);
    }
    rc = vmaf_close(vmaf);
    if (rc != 0) {
        return fail("vmaf_close", rc);
    }
    if (model != NULL) {
        vmaf_model_destroy(model);
    }
    return status;
}

int main(int argc, char **argv)
{
    if (argc != 9) {
        (void)fprintf(stderr, "usage: %s REF DIS MODEL WIDTH HEIGHT PIXFMT BPC POOL\n", argv[0]);
        return 1;
    }
    FILE *fr = fopen(argv[1], "rb");
    if (fr == NULL) {
        return fail("fopen(ref)", errno);
    }
    FILE *fd = fopen(argv[2], "rb");
    if (fd == NULL) {
        (void)fclose(fr);
        return fail("fopen(dis)", errno);
    }
    const Geometry g = {
        .width = (unsigned)strtoul(argv[4], NULL, 10),
        .height = (unsigned)strtoul(argv[5], NULL, 10),
        .bpc = (unsigned)strtoul(argv[7], NULL, 10),
        .pix_fmt = (enum VmafPixelFormat)strtol(argv[6], NULL, 10),
    };
    const int status = score(argv[3], fr, fd, &g, (int)strtol(argv[8], NULL, 10));
    (void)fclose(fr);
    (void)fclose(fd);
    return status;
}
