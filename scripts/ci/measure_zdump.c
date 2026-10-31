/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Preload shim for scripts/ci/measure_hdr_pixels.py: appends every plane zimg
 * converts to $ZDUMP.<n>.yuv, n = 0 for the reference, 1 for the distorted
 * picture. Measurement only, never linked into the library or the CLI.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <zimg.h>

enum { PLANE_COUNT = 3, NAME_SIZE = 512 };

static zimg_image_format last_dst;
static unsigned process_calls;

typedef zimg_filter_graph *(*build_fn)(const zimg_image_format *, const zimg_image_format *,
                                       const zimg_graph_builder_params *);
typedef zimg_error_code_e (*process_fn)(const zimg_filter_graph *, const zimg_image_buffer_const *,
                                        const zimg_image_buffer *, void *,
                                        zimg_filter_graph_callback, void *,
                                        zimg_filter_graph_callback, void *);

zimg_filter_graph *zimg_filter_graph_build(const zimg_image_format *src,
                                           const zimg_image_format *dst,
                                           const zimg_graph_builder_params *params)
{
    build_fn real = (build_fn)dlsym(RTLD_NEXT, "zimg_filter_graph_build");
    if (real == NULL)
        return NULL;
    last_dst = *dst;
    return real(src, dst, params);
}

static int write_plane(FILE *file, const zimg_image_buffer *dst, int plane)
{
    const size_t bytes = last_dst.depth > 8 ? 2 : 1;
    const unsigned width = plane ? last_dst.width >> last_dst.subsample_w : last_dst.width;
    const unsigned height = plane ? last_dst.height >> last_dst.subsample_h : last_dst.height;
    for (unsigned y = 0; y < height; y++) {
        /* SAFETY: y is below the plane height zimg just wrote; the stride is zimg's own. */
        const char *row =
            (const char *)dst->plane[plane].data + (ptrdiff_t)y * dst->plane[plane].stride;
        if (fwrite(row, bytes, width, file) != width)
            return -1;
    }
    return 0;
}

static int dump_picture(const zimg_image_buffer *dst)
{
    const char *prefix = getenv("ZDUMP");
    char name[NAME_SIZE];
    if (prefix == NULL)
        return -1;
    const int len = snprintf(name, sizeof(name), "%s.%u.yuv", prefix, process_calls % 2U);
    if (len < 0 || (size_t)len >= sizeof(name))
        return -1;
    process_calls++;
    FILE *file = fopen(name, "ab");
    if (file == NULL)
        return -1;
    int rc = 0;
    for (int plane = 0; plane < PLANE_COUNT && rc == 0; plane++)
        rc = write_plane(file, dst, plane);
    return fclose(file) == 0 ? rc : -1;
}

zimg_error_code_e zimg_filter_graph_process(const zimg_filter_graph *graph,
                                            const zimg_image_buffer_const *src,
                                            const zimg_image_buffer *dst, void *tmp,
                                            zimg_filter_graph_callback unpack, void *unpack_user,
                                            zimg_filter_graph_callback pack, void *pack_user)
{
    process_fn real = (process_fn)dlsym(RTLD_NEXT, "zimg_filter_graph_process");
    if (real == NULL)
        return ZIMG_ERROR_UNKNOWN;
    const zimg_error_code_e rc = real(graph, src, dst, tmp, unpack, unpack_user, pack, pack_user);
    if (rc != ZIMG_ERROR_SUCCESS)
        return rc;
    return dump_picture(dst) == 0 ? rc : ZIMG_ERROR_UNKNOWN;
}
