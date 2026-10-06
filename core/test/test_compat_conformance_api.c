/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Conformance scenarios of the libvmaf compat library (RC4 WP6) that need no
 * frames: contexts, dictionaries, models and collections, pictures and their
 * v2 form, conversion, perceptual weighting, tiny AI, the HIP / Metal
 * functions of a build without them, and the MCP server. Each call's result
 * and outputs go to the trace; test_compat_conformance.c compares the trace
 * of the old libvmaf with the compat library's.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compat_conformance_trace.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define PATH_TEXT 1024
#define MODEL_FEATURE_PROBE 32u  /* past every built-in model's feature count */
#define BUILTIN_VERSIONS_MAX 64u /* HISS-02 bound of the built-in model walk */
#define BACKEND_HANDLE_PROBE 8

static const char *model_file(char *buf, const char *name)
{
    (void)snprintf(buf, PATH_TEXT, "%s/%s", VMAFX_TEST_MODEL_DIR, name);
    return buf;
}

static VmafConfiguration quiet_config(void)
{
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    return cfg;
}

/* ---- Contexts ---------------------------------------------------------------- */

void scenario_lifecycle(const VmafCompatApi *api, Trace *t)
{
    VmafContext *vmaf = NULL;
    trace(t, "init(NULL) %d", api->init(NULL, quiet_config()));
    const int err = api->init(&vmaf, quiet_config());
    trace(t, "init %d context %d", err, vmaf != NULL);
    trace(t, "version %s", trace_str(api->version()));
    trace(t, "close(NULL) %d", api->close(NULL));
    trace(t, "close %d", api->close(vmaf));
}

/* ---- Dictionaries ------------------------------------------------------------ */

void scenario_dictionary(const VmafCompatApi *api, Trace *t)
{
    VmafFeatureDictionary *dict = NULL;
    trace(t, "set number %d", api->feature_dictionary_set(&dict, "enable_chroma", "1"));
    trace(t, "set text %d", api->feature_dictionary_set(&dict, "name", "value"));
    trace(t, "set again %d", api->feature_dictionary_set(&dict, "enable_chroma", "0"));
    trace(t, "set NULL dict %d", api->feature_dictionary_set(NULL, "a", "b"));
    const int freed = api->feature_dictionary_free(&dict);
    trace(t, "free %d emptied %d", freed, dict == NULL);
    trace(t, "free empty %d", api->feature_dictionary_free(&dict));
    trace(t, "free NULL %d", api->feature_dictionary_free(NULL));
}

/* ---- Models ------------------------------------------------------------------ */

static void trace_features(const VmafCompatApi *api, Trace *t, const VmafModel *model)
{
    const unsigned n = api->model_feature_count(model);
    trace(t, "feature count %u", n);
    for (unsigned i = 0; i <= n && i < MODEL_FEATURE_PROBE; i++) {
        trace(t, "feature %u %s", i, trace_str(api->model_feature_name(model, i)));
    }
}

static void overload(const VmafCompatApi *api, Trace *t, VmafModel *model)
{
    VmafFeatureDictionary *opts = NULL;
    (void)api->feature_dictionary_set(&opts, "adm_enhn_gain_limit", "1.2");
    trace(t, "overload adm %d", api->model_feature_overload(model, "adm", opts));
    /* An extractor the model reads no feature of (the vmaf command line passes
     * cambi options to every model). */
    VmafFeatureDictionary *unused = NULL;
    (void)api->feature_dictionary_set(&unused, "enc_width", "576");
    trace(t, "overload cambi %d", api->model_feature_overload(model, "cambi", unused));
    trace(t, "overload NULL opts %d", api->model_feature_overload(model, "adm", NULL));
    trace(t, "overload NULL model %d", api->model_feature_overload(NULL, "adm", NULL));
}

static void builtin_versions(const VmafCompatApi *api, Trace *t)
{
    const char *version = NULL;
    const void *cursor = api->model_version_next(NULL, &version);
    for (unsigned i = 0; cursor && i < BUILTIN_VERSIONS_MAX; i++) {
        trace(t, "builtin %s", trace_str(version));
        cursor = api->model_version_next(cursor, &version);
    }
    trace(t, "after the last %s", trace_str(version));
    trace(t, "default %s", trace_str(api->default_model_version()));
}

void scenario_models(const VmafCompatApi *api, Trace *t)
{
    char path[PATH_TEXT];
    VmafModelConfig cfg = {.name = "conformance", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    VmafModel *model = NULL;
    trace(t, "load NULL version %d", api->model_load(&model, &cfg, NULL));
    trace(t, "load unknown %d", api->model_load(&model, &cfg, "no_such_model"));
    trace(t, "load %d", api->model_load(&model, &cfg, "vmaf_v0.6.1"));
    trace_features(api, t, model);
    overload(api, t, model);
    api->model_destroy(model);
    model = NULL;
    trace(t, "load path %d",
          api->model_load_from_path(&model, &cfg, model_file(path, "vmaf_v0.6.1neg.json")));
    trace_features(api, t, model);
    api->model_destroy(model);
    model = NULL;
    trace(t, "load missing path %d",
          api->model_load_from_path(&model, &cfg, model_file(path, "missing.json")));
    trace(t, "count NULL %u", api->model_feature_count(NULL));
    trace(t, "name NULL %s", trace_str(api->model_feature_name(NULL, 0)));
    api->model_destroy(NULL);
    builtin_versions(api, t);
}

static void trace_collection(const VmafCompatApi *api, Trace *t, VmafModel *model,
                             VmafModelCollection *collection)
{
    trace(t, "collection %d lead %d", collection != NULL, model != NULL);
    if (!collection || !model) {
        return;
    }
    trace_features(api, t, model);
    VmafFeatureDictionary *opts = NULL;
    (void)api->feature_dictionary_set(&opts, "vif_enhn_gain_limit", "1.1");
    trace(t, "collection overload %d",
          api->model_collection_feature_overload(model, &collection, "vif", opts));
    VmafFeatureDictionary *unused = NULL;
    (void)api->feature_dictionary_set(&unused, "enc_bitdepth", "8");
    trace(t, "collection overload cambi %d",
          api->model_collection_feature_overload(model, &collection, "cambi", unused));
    trace(t, "collection overload NULL %d",
          api->model_collection_feature_overload(model, NULL, "vif", NULL));
    api->model_destroy(model);
    api->model_collection_destroy(collection);
}

void scenario_collections(const VmafCompatApi *api, Trace *t)
{
    char path[PATH_TEXT];
    VmafModelConfig cfg = {.name = "set", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    VmafModel *model = NULL;
    VmafModelCollection *collection = NULL;
    trace(t, "collection unknown %d",
          api->model_collection_load(&model, &collection, &cfg, "no_such_set"));
    trace(t, "collection NULL version %d",
          api->model_collection_load(&model, &collection, &cfg, NULL));
    trace(t, "collection load %d",
          api->model_collection_load(&model, &collection, &cfg, "vmaf_b_v0.6.3"));
    trace_collection(api, t, model, collection);
    model = NULL;
    collection = NULL;
    trace(t, "collection path %d",
          api->model_collection_load_from_path(&model, &collection, &cfg,
                                               model_file(path, "vmaf_b_v0.6.3.json")));
    trace_collection(api, t, model, collection);
    api->model_collection_destroy(NULL);
}

/* ---- Pictures ---------------------------------------------------------------- */

static void trace_picture(Trace *t, const char *what, const VmafPicture *pic)
{
    trace(t, "%s fmt %d bpc %u w %u/%u/%u h %u/%u/%u stride %td/%td/%td data %d%d%d ref %d", what,
          (int)pic->pix_fmt, pic->bpc, pic->w[0], pic->w[1], pic->w[2], pic->h[0], pic->h[1],
          pic->h[2], pic->stride[0], pic->stride[1], pic->stride[2], pic->data[0] != NULL,
          pic->data[1] != NULL, pic->data[2] != NULL, pic->ref != NULL);
}

static void picture_allocs(const VmafCompatApi *api, Trace *t)
{
    VmafPicture pic;
    memset(&pic, 0, sizeof(pic));
    trace(t, "alloc NULL %d", api->picture_alloc(NULL, VMAF_PIX_FMT_YUV420P, 8, 64, 48));
    trace(t, "alloc no format %d", api->picture_alloc(&pic, VMAF_PIX_FMT_UNKNOWN, 8, 64, 48));
    trace(t, "alloc 7 bit %d", api->picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 7, 64, 48));
    trace(t, "alloc 0 wide %d", api->picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 0, 48));
    static const enum VmafPixelFormat formats[] = {VMAF_PIX_FMT_YUV420P, VMAF_PIX_FMT_YUV422P,
                                                   VMAF_PIX_FMT_YUV444P, VMAF_PIX_FMT_YUV400P};
    for (unsigned i = 0; i < 4u; i++) {
        trace(t, "alloc %d", api->picture_alloc(&pic, formats[i], 8u + 2u * i, 65, 37));
        trace_picture(t, "picture", &pic);
        trace(t, "unref %d", api->picture_unref(&pic));
        trace_picture(t, "after unref", &pic);
    }
    trace(t, "unref again %d", api->picture_unref(&pic));
    trace(t, "unref NULL %d", api->picture_unref(NULL));
}

static void trace_picture2(Trace *t, const char *what, const VmafPicture2 *pic)
{
    trace(t, "%s fmt %d bpc %u w %u h %u stride %td data %d ref %d backend %d handle %d", what,
          (int)pic->pix_fmt, pic->bpc, pic->w[0], pic->h[0], pic->stride[0], pic->data[0] != NULL,
          pic->ref != NULL, (int)pic->backend, pic->backend_handle != 0);
}

static void picture_v2(const VmafCompatApi *api, Trace *t)
{
    VmafPicture2 two;
    VmafPicture one;
    memset(&two, 0, sizeof(two));
    memset(&one, 0, sizeof(one));
    trace(t, "alloc2 NULL %d", api->picture2_alloc(NULL, VMAF_PIX_FMT_YUV420P, 8, 32, 32));
    trace(t, "alloc2 %d", api->picture2_alloc(&two, VMAF_PIX_FMT_YUV444P, 10, 33, 17));
    trace_picture2(t, "picture2", &two);
    trace(t, "v2 to v1 %d", api->picture_v2_to_v1(&two, &one));
    trace_picture(t, "v1 of v2", &one);
    trace(t, "unref2 %d", api->picture2_unref(&two));
    trace_picture2(t, "after unref2", &two);
    trace(t, "v1 to v2 %d", api->picture_v1_to_v2(&one, &two));
    trace_picture2(t, "v2 of v1", &two);
    trace(t, "unref v1 %d", api->picture_unref(&one));
    trace(t, "unref2 %d", api->picture2_unref(&two));
    trace(t, "unref2 again %d", api->picture2_unref(&two));
    trace(t, "unref2 NULL %d", api->picture2_unref(NULL));
    trace(t, "v1 to v2 NULL %d no ref %d", api->picture_v1_to_v2(NULL, &two),
          api->picture_v1_to_v2(&one, &two));
    trace(t, "v2 to v1 NULL %d no ref %d", api->picture_v2_to_v1(NULL, &one),
          api->picture_v2_to_v1(&two, &one));
    for (int b = -1; b < BACKEND_HANDLE_PROBE; b++) {
        trace(t, "handle name %d %s", b, trace_str(api->backend_handle_name((VmafBackendHandle)b)));
    }
}

void scenario_pictures(const VmafCompatApi *api, Trace *t)
{
    picture_allocs(api, t);
    picture_v2(api, t);
}

/* ---- Conversion (#2140) -------------------------------------------------------- */

void scenario_conversion(const VmafCompatApi *api, Trace *t)
{
    VmafPicture src;
    VmafPicture dst;
    memset(&dst, 0, sizeof(dst));
    (void)api->picture_alloc(&src, VMAF_PIX_FMT_YUV420P, 8, 64, 48);
    const VmafColor color = {VMAF_COLOR_RANGE_LIMITED, VMAF_COLOR_PRIMARIES_BT709,
                             VMAF_COLOR_TRC_BT709, VMAF_COLOR_MATRIX_BT709};
    const VmafPictureConvertTarget target = {VMAF_PIX_FMT_YUV444P,  10, 32, 24, color,
                                             VMAF_RESAMPLE_BILINEAR};
    VmafPictureConvertContext *ctx = NULL;
    trace(t, "convert init NULL %d",
          api->picture_convert_context_init_with_color(NULL, &src, &color, &target));
    const int err = api->picture_convert_context_init_with_color(&ctx, &src, &color, &target);
    trace(t, "convert init %d", err);
    trace(t, "convert %d", api->picture_convert(ctx, &dst, &src));
    trace_picture(t, "converted", &dst);
    if (dst.ref) {
        trace(t, "unref converted %d", api->picture_unref(&dst));
    }
    trace(t, "convert NULL %d", api->picture_convert(NULL, NULL, NULL));
    trace(t, "close %d", api->picture_convert_context_close(err ? NULL : ctx));
    trace(t, "close NULL %d", api->picture_convert_context_close(NULL));
    (void)api->picture_unref(&src);
}

/* ---- Perceptual weighting (ADR-1118) ---------------------------------------------- */

void scenario_perceptual(const VmafCompatApi *api, Trace *t)
{
    VmafContext *vmaf = NULL;
    (void)api->init(&vmaf, quiet_config());
    static const uint8_t garbage[] = {0x50, 0x4c, 0x52, 0x53, 0, 0, 0, 0};
    trace(t, "enable %d", api->set_perceptual_weight_enabled(vmaf, 1));
    trace(t, "enable NULL %d", api->set_perceptual_weight_enabled(NULL, 1));
    trace(t, "strength %d", api->set_perceptual_weight_strength(vmaf, 0.75));
    trace(t, "strength negative %d", api->set_perceptual_weight_strength(vmaf, -1.0));
    trace(t, "strength NULL %d", api->set_perceptual_weight_strength(NULL, 0.5));
    trace(t, "sidedata garbage %d",
          api->set_perceptual_sidedata(vmaf, garbage, sizeof(garbage), 0));
    trace(t, "sidedata NULL blob %d", api->set_perceptual_sidedata(vmaf, NULL, 0, 0));
    trace(t, "sidedata NULL %d", api->set_perceptual_sidedata(NULL, garbage, 8, 0));
    trace(t, "disable %d", api->set_perceptual_weight_enabled(vmaf, 0));
    (void)api->close(vmaf);
}

/* ---- Tiny AI ------------------------------------------------------------------------ */

static void tiny_sessions(const VmafCompatApi *api, Trace *t)
{
    VmafDnnSession *sess = NULL;
    const VmafDnnConfig cfg = {VMAF_DNN_DEVICE_CPU, 0, 1, false};
    uint8_t plane8[16] = {0};
    uint16_t plane16[16] = {0};
    trace(t, "open missing %d", api->dnn_session_open(&sess, "/nonexistent/model.onnx", &cfg));
    trace(t, "open NULL %d", api->dnn_session_open(NULL, NULL, NULL));
    trace(t, "run8 NULL %d", api->dnn_session_run_luma8(NULL, plane8, 4, 4, 4, plane8, 4));
    trace(t, "run16 NULL %d", api->dnn_session_run_plane16(NULL, plane16, 8, 4, 4, 10, plane16, 8));
    trace(t, "run NULL %d", api->dnn_session_run(NULL, NULL, 0, NULL, 0));
    trace(t, "ep NULL %s", trace_str(api->dnn_session_attached_ep(NULL)));
    api->dnn_session_close(NULL);
    trace(t, "verify NULL %d", api->dnn_verify_signature(NULL, NULL));
    trace(t, "verify missing %d", api->dnn_verify_signature("/nonexistent/model.onnx", NULL));
}

void scenario_tiny_ai(const VmafCompatApi *api, Trace *t)
{
    VmafContext *vmaf = NULL;
    (void)api->init(&vmaf, quiet_config());
    trace(t, "dnn available %d", api->dnn_available());
    trace(t, "use missing %d", api->use_tiny_model(vmaf, "/nonexistent/model.onnx", NULL));
    trace(t, "use NULL %d", api->use_tiny_model(NULL, NULL, NULL));
    trace(t, "codec %d", api->dnn_set_codec_context(vmaf, "libx264", "medium", 23));
    trace(t, "codec NULL %d", api->dnn_set_codec_context(NULL, NULL, NULL, 0));
    trace(t, "codec aware %d NULL %d", api->dnn_is_codec_aware(vmaf),
          api->dnn_is_codec_aware(NULL));
    trace(t, "resize %d", api->dnn_set_resize_mode(vmaf, VMAF_DNN_RESIZE_BILINEAR));
    trace(t, "resize bad %d", api->dnn_set_resize_mode(vmaf, (VmafDnnResizeMode)99));
    trace(t, "resize NULL %d", api->dnn_set_resize_mode(NULL, VMAF_DNN_RESIZE_DISABLED));
    tiny_sessions(api, t);
    (void)api->close(vmaf);
}

/* ---- HIP / Metal in a build without them ----------------------------------------------- */

#if !VMAFX_ENGINE_EXPORTS_HIP
static void hip_absent(const VmafCompatApi *api, Trace *t)
{
    VmafHipState *state = (VmafHipState *)(uintptr_t)1;
    const VmafHipConfiguration cfg = {-1, 0};
    trace(t, "hip available %d", api->hip_available());
    const int err = api->hip_state_init(&state, cfg);
    trace(t, "hip init %d cleared %d", err, state == NULL);
    trace(t, "hip init NULL %d", api->hip_state_init(NULL, cfg));
    trace(t, "hip import %d", api->hip_import_state(NULL, NULL));
    state = (VmafHipState *)(uintptr_t)1;
    api->hip_state_free(&state);
    trace(t, "hip free cleared %d", state == NULL);
    api->hip_state_free(NULL);
    trace(t, "hip list %d", api->hip_list_devices());
}
#endif

#if !VMAFX_ENGINE_EXPORTS_METAL
static void metal_absent(const VmafCompatApi *api, Trace *t)
{
    VmafMetalState *state = (VmafMetalState *)(uintptr_t)1;
    VmafMetalConfiguration cfg;
    VmafMetalExternalHandles handles;
    memset(&cfg, 0, sizeof(cfg));
    memset(&handles, 0, sizeof(handles));
    trace(t, "metal available %d", api->metal_available());
    const int err = api->metal_state_init(&state, cfg);
    trace(t, "metal init %d cleared %d", err, state == NULL);
    trace(t, "metal import %d", api->metal_import_state(NULL, NULL));
    state = (VmafMetalState *)(uintptr_t)1;
    api->metal_state_free(&state);
    trace(t, "metal free cleared %d", state == NULL);
    trace(t, "metal list %d", api->metal_list_devices());
    trace(t, "metal external %d", api->metal_state_init_external(&state, handles));
    trace(t, "metal picture %d", api->metal_picture_import(NULL, 0, 0, 16, 16, 8, 1, 0));
    trace(t, "metal wait %d", api->metal_wait_compute(NULL));
    trace(t, "metal read %d", api->metal_read_imported_pictures(NULL, 0));
}
#endif

void scenario_absent_backends(const VmafCompatApi *api, Trace *t)
{
#if !VMAFX_ENGINE_EXPORTS_HIP
    hip_absent(api, t);
#endif
#if !VMAFX_ENGINE_EXPORTS_METAL
    metal_absent(api, t);
#endif
    (void)api;
    (void)t;
}

/* ---- The embedded MCP server (builds with -Denable_mcp=true) ---------------------------- */

void scenario_mcp(const VmafCompatApi *api, Trace *t)
{
#if VMAFX_BUILD_MCP
    VmafContext *vmaf = NULL;
    (void)api->init(&vmaf, quiet_config());
    trace(t, "mcp available %d", api->mcp_available());
    for (int transport = 0; transport < 4; transport++) {
        trace(t, "transport %d %d", transport,
              api->mcp_transport_available((VmafMcpTransport)transport));
    }
    VmafMcpServer *server = NULL;
    trace(t, "mcp init NULL %d", api->mcp_init(NULL, vmaf, NULL));
    trace(t, "mcp init %d", api->mcp_init(&server, vmaf, NULL));
    const VmafMcpUdsConfig uds = {NULL};
    const VmafMcpStdioConfig stdio = {-1, -1};
    VmafMcpSseConfig sse = {.port = 0, .path = ""};
    trace(t, "uds NULL path %d", api->mcp_start_uds(server, &uds));
    trace(t, "stdio bad fds %d", api->mcp_start_stdio(server, &stdio));
    trace(t, "sse empty path %d", api->mcp_start_sse(server, &sse));
    trace(t, "stop %d stop NULL %d", api->mcp_stop(server), api->mcp_stop(NULL));
    api->mcp_close(&server);
    trace(t, "closed %d", server == NULL);
    api->mcp_close(NULL);
    (void)api->close(vmaf);
#else
    (void)api;
    (void)t;
#endif
}

/* NOLINTEND(modernize-use-nullptr) */
