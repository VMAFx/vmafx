/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The C API leg of the scoring contract test (#2155, RC4 WP8): scores one raw
 * YUV 4:2:0 pair through the VMAFx API and prints the pooled mean model score
 * and the context's provenance record as one JSON object. The Go contract test
 * (cmd/vmafx-server/score_contract_test.go) runs the same request through this
 * program, the vmaf CLI and the scoring server and requires the same score bit
 * for bit and the same provenance.
 *
 * Usage: vmafx_score_contract REF DIST WIDTH HEIGHT BPC MODEL THREADS SUBSAMPLE
 * MODEL "-" is the library default (vmafx_model_default_version()).
 *
 * Output: {"vmaf": <%.17g>, "vmaf_hex": "<%a>", "frames": N,
 *          "provenance": {"abi_major": .., "abi_minor": .., "abi_patch": ..,
 *                         "n_extractors": .., "version": ".."}}
 * Exit status: 0 scored, 1 a VMAFx call failed (the error is on stderr),
 * 2 bad arguments or an unreadable input.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_fopen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum {
    ARG_REF = 1,
    ARG_DIST,
    ARG_W,
    ARG_H,
    ARG_BPC,
    ARG_MODEL,
    ARG_THREADS,
    ARG_SUBSAMPLE,
    N_ARGS
};

#define MAX_FRAMES 100000u /* bound of the frame loop (HISS-02) */

typedef struct Request {
    const char *ref, *dist, *model;
    uint32_t w, h, bpc, threads, subsample;
} Request;

static bool parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (!end || *end != '\0' || end == text || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_request(int argc, char **argv, Request *req)
{
    if (argc != N_ARGS) {
        return false;
    }
    req->ref = argv[ARG_REF];
    req->dist = argv[ARG_DIST];
    req->model =
        strcmp(argv[ARG_MODEL], "-") == 0 ? vmafx_model_default_version() : argv[ARG_MODEL];
    return parse_u32(argv[ARG_W], &req->w) && parse_u32(argv[ARG_H], &req->h) &&
           parse_u32(argv[ARG_BPC], &req->bpc) && parse_u32(argv[ARG_THREADS], &req->threads) &&
           parse_u32(argv[ARG_SUBSAMPLE], &req->subsample);
}

/* Bytes of one tightly packed 4:2:0 frame. */
static size_t frame_bytes(const Request *req)
{
    const size_t sample = req->bpc > 8 ? 2u : 1u;
    const size_t luma = (size_t)req->w * req->h;
    const size_t chroma = (size_t)((req->w + 1) / 2) * ((req->h + 1) / 2);
    return (luma + 2 * chroma) * sample;
}

/* A host frame holding a copy of the next frame of `file`; NULL at the end. */
static VmafxFrame *read_frame(FILE *file, const Request *req, uint8_t *buf)
{
    const size_t size = frame_bytes(req);
    if (fread(buf, 1, size, file) != size) {
        return NULL;
    }
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    desc.pix_fmt = VMAFX_PIXEL_FORMAT_YUV420P;
    desc.bpc = req->bpc;
    desc.w = req->w;
    desc.h = req->h;
    VmafxFrame *frame = NULL;
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    if (vmafx_frame_create_host(NULL, &desc, &frame, NULL) != VMAFX_OK ||
        vmafx_frame_planes(frame, &planes, NULL) != VMAFX_OK) {
        vmafx_frame_unref(frame);
        return NULL;
    }
    const size_t sample = req->bpc > 8 ? 2u : 1u;
    const uint8_t *src = buf;
    for (unsigned p = 0; p < planes.n_planes && p < 3; p++) {
        const unsigned pw = p ? (req->w + 1) / 2 : req->w;
        const unsigned ph = p ? (req->h + 1) / 2 : req->h;
        uint8_t *dst = planes.data[p];
        for (unsigned y = 0; y < ph; y++, src += pw * sample, dst += planes.stride[p]) {
            memcpy(dst, src, pw * sample);
        }
    }
    return frame;
}

static bool report_failure(const char *what, VmafxError *error)
{
    (void)fprintf(stderr, "vmafx_score_contract: %s: %s\n", what,
                  error ? vmafx_error_message(error) : "failed");
    vmafx_error_free(error);
    return false;
}

/* Submit every frame pair of the two files; `*n` counts them. */
static bool submit_all(VmafxContext *context, const Request *req, unsigned *n)
{
    FILE *ref = vmaf_test_fopen(req->ref, "rb");
    FILE *dist = vmaf_test_fopen(req->dist, "rb");
    uint8_t *ref_buf = malloc(frame_bytes(req));
    uint8_t *dist_buf = malloc(frame_bytes(req));
    bool ok = ref && dist && ref_buf && dist_buf;
    for (*n = 0; ok && *n < MAX_FRAMES; (*n)++) {
        VmafxFrame *r = read_frame(ref, req, ref_buf);
        VmafxFrame *d = r ? read_frame(dist, req, dist_buf) : NULL;
        if (!r || !d) {
            vmafx_frame_unref(r);
            break;
        }
        VmafxError *error = NULL;
        ok = vmafx_submit(context, r, d, *n, &error) == VMAFX_OK || report_failure("submit", error);
    }
    free(ref_buf);
    free(dist_buf);
    if (ref) {
        (void)fclose(ref);
    }
    if (dist) {
        (void)fclose(dist);
    }
    return ok && *n > 0;
}

static void print_result(const VmafxPooledScore *score, unsigned n, const VmafxProvenance *p)
{
    (void)printf("{\"vmaf\": %.17g, \"vmaf_hex\": \"%a\", \"frames\": %u, "
                 "\"provenance\": {\"abi_major\": %u, \"abi_minor\": %u, \"abi_patch\": %u, "
                 "\"n_extractors\": %u, \"version\": \"%s\"}}\n",
                 score->value, score->value, n, (unsigned)p->abi_major, (unsigned)p->abi_minor,
                 (unsigned)p->abi_patch, (unsigned)p->n_extractors, p->version ? p->version : "");
}

/* Score the request on a fresh context; false (reason on stderr) on failure. */
static bool score(const Request *req)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = req->threads;
    config.n_subsample = req->subsample;
    VmafxModelConfig model_config = VMAFX_MODEL_CONFIG_INIT;
    VmafxContext *context = NULL;
    VmafxModel *model = NULL;
    VmafxError *error = NULL;
    unsigned n = 0;
    VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
    VmafxProvenance provenance = VMAFX_PROVENANCE_INIT;
    bool ok = (vmafx_context_create(&config, &context, &error) == VMAFX_OK &&
               vmafx_model_load(&model_config, req->model, &model, &error) == VMAFX_OK &&
               vmafx_context_use_model(context, model, &error) == VMAFX_OK) ||
              report_failure("set up", error);
    ok = ok && submit_all(context, req, &n);
    ok = ok && ((vmafx_flush(context, &error) == VMAFX_OK &&
                 vmafx_score_pooled(context, model, VMAFX_POOL_MEAN, 0, n - 1, &pooled, &error) ==
                     VMAFX_OK &&
                 vmafx_context_provenance(context, &provenance, &error) == VMAFX_OK) ||
                report_failure("score", error));
    if (ok) {
        print_result(&pooled, n, &provenance);
    }
    vmafx_model_unref(model);
    if (context && vmafx_context_destroy(context, NULL) != VMAFX_OK) {
        ok = false;
    }
    return ok;
}

int main(int argc, char **argv)
{
    Request req;
    memset(&req, 0, sizeof(req));
    if (!parse_request(argc, argv, &req)) {
        (void)fprintf(stderr, "usage: %s REF DIST WIDTH HEIGHT BPC MODEL THREADS SUBSAMPLE\n",
                      argc > 0 ? argv[0] : "vmafx_score_contract");
        return 2;
    }
    return score(&req) ? 0 : 1;
}

/* NOLINTEND(modernize-use-nullptr) */
