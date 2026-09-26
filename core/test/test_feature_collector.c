/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include "test.h"
#include "feature_collector_internal.h"
#include "libvmaf.c"
#include <limits.h>
#include <time.h>

static unsigned close_retry_calls;

static int close_retry_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                            unsigned w, unsigned h)
{
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    (void)w;
    (void)h;
    return 0;
}

static int close_retry_once(VmafFeatureExtractor *fex)
{
    (void)fex;
    close_retry_calls++;
    return close_retry_calls == 1 ? -EIO : 0;
}

static char *test_vmaf_close_retains_context_for_retry(void)
{
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, (VmafConfiguration){0});
    mu_assert("vmaf_init", err == 0 && vmaf != NULL);
    VmafFeatureExtractor synth = {
        .name = "synth_public_close_retry",
        .init = close_retry_init,
        .close = close_retry_once,
    };
    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, &synth, NULL);
    mu_assert("context_create", err == 0 && ctx != NULL);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8, 64, 64);
    mu_assert("context_init", err == 0);
    err = feature_extractor_vector_append(&vmaf->registered_feature_extractors, ctx, 0);
    mu_assert("vector append", err == 0);

    close_retry_calls = 0;
    err = vmaf_close(vmaf);
    mu_assert("first vmaf_close returns the transient close error", err == -EIO);
    mu_assert("failed close retains the public context", vmaf->feature_collector != NULL);
    err = vmaf_close(vmaf);
    mu_assert("vmaf_close retry succeeds", err == 0);
    mu_assert("close callback was retried exactly once", close_retry_calls == 2);
    return NULL;
}

static int install_worker_close_retry(void *data, void **thread_data)
{
    BatchThreadData *td = NULL;
    int err = batch_thread_data_ensure(thread_data, 1, &td);
    if (err)
        return err;
    const VmafFeatureExtractor *fex = data;
    err = vmaf_feature_extractor_context_create(&td->fex_ctx[0], fex, NULL);
    if (err)
        return err;
    return vmaf_feature_extractor_context_init(td->fex_ctx[0], VMAF_PIX_FMT_YUV420P, 8, 64, 64);
}

static char *test_vmaf_close_retains_worker_private_context(void)
{
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, (VmafConfiguration){.n_threads = 1});
    mu_assert("threaded vmaf_init", err == 0 && vmaf != NULL);
    VmafFeatureExtractor synth = {
        .name = "synth_worker_close_retry",
        .init = close_retry_init,
        .close = close_retry_once,
    };
    err = vmaf_thread_pool_enqueue(vmaf->thread_pool, install_worker_close_retry, &synth,
                                   sizeof(synth));
    mu_assert("worker fixture enqueue", err == 0);
    err = vmaf_thread_pool_wait(vmaf->thread_pool);
    mu_assert("worker fixture install", err == 0);

    close_retry_calls = 0;
    err = vmaf_close(vmaf);
    mu_assert("worker close failure reaches the public caller", err == -EIO);
    mu_assert("failed worker prepare retains the pool", vmaf->thread_pool != NULL);
    err = vmaf_close(vmaf);
    mu_assert("public close retry commits worker ownership", err == 0);
    mu_assert("worker close callback was retried", close_retry_calls == 2);
    return NULL;
}

#ifdef HAVE_CUDA
static unsigned backend_close_calls;

static int backend_picture_alloc(VmafPicture *pic, void *cookie)
{
    (void)cookie;
    memset(pic, 0, sizeof(*pic));
    pic->data[0] = malloc(1);
    return pic->data[0] ? 0 : -ENOMEM;
}

static int backend_picture_close_positive_once(VmafPicture *pic, void *cookie)
{
    (void)cookie;
    backend_close_calls++;
    if (backend_close_calls == 1)
        return EIO; /* pthread-style positive errno must not look successful. */
    free(pic->data[0]);
    pic->data[0] = NULL;
    return 0;
}

static char *test_vmaf_close_commit_retry_is_idempotent(void)
{
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, (VmafConfiguration){0});
    mu_assert("vmaf_init", err == 0 && vmaf != NULL);

    vmaf->dnn.in_buf = malloc(sizeof(*vmaf->dnn.in_buf));
    vmaf->dnn.in_elements = 1;
    vmaf->perceptual.summaries = malloc(sizeof(*vmaf->perceptual.summaries));
    vmaf->perceptual.capacity = 1;
    mu_assert("commit fixture allocations",
              vmaf->dnn.in_buf != NULL && vmaf->perceptual.summaries != NULL);

    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 1,
        .alloc_picture_callback = backend_picture_alloc,
        .free_picture_callback = backend_picture_close_positive_once,
    };
    err = vmaf_gpu_picture_pool_init(&vmaf->cuda.ring_buffer, cfg);
    mu_assert("backend ring fixture", err == 0 && vmaf->cuda.ring_buffer != NULL);

    backend_close_calls = 0;
    err = vmaf_close(vmaf);
    mu_assert("positive child error is normalized at the public boundary", err == -EIO);
    mu_assert("collector commit stays committed", vmaf->feature_collector == NULL);
    mu_assert("framesync commit stays committed", vmaf->framesync == NULL);
    mu_assert("DNN commit is zeroed for retry",
              vmaf->dnn.in_buf == NULL && vmaf->dnn.in_elements == 0);
    mu_assert("perceptual commit is zeroed for retry",
              vmaf->perceptual.summaries == NULL && vmaf->perceptual.capacity == 0);
    mu_assert("failed backend owner remains reachable", vmaf->cuda.ring_buffer != NULL);

    err = vmaf_close(vmaf);
    mu_assert("commit-phase retry succeeds", err == 0);
    mu_assert("failed backend child alone was retried", backend_close_calls == 2);
    return NULL;
}

static char *test_cuda_state_import_rejects_duplicate_owners(void)
{
    VmafContext *first = NULL;
    VmafContext *second = NULL;
    VmafCudaState *state = calloc(1, sizeof(*state));
    mu_assert("CUDA state fixture allocation", state != NULL);
    state->ctx = (CUcontext)state;

    int err = vmaf_init(&first, (VmafConfiguration){0});
    err |= vmaf_init(&second, (VmafConfiguration){0});
    mu_assert("context fixtures", err == 0 && first != NULL && second != NULL);
    err = vmaf_cuda_import_state(first, state);
    mu_assert("first CUDA import succeeds", err == 0 && state->imported);
    mu_assert("context copy is an independent release owner", !first->cuda.state.imported);
    err = vmaf_cuda_import_state(second, state);
    mu_assert("same CUDA state cannot be imported twice", err == -EBUSY);

    VmafCudaState *other = calloc(1, sizeof(*other));
    mu_assert("second CUDA state fixture allocation", other != NULL);
    err = vmaf_cuda_import_state(first, other);
    mu_assert("live context CUDA state cannot be overwritten", err == -EBUSY);

    memset(&first->cuda.state, 0, sizeof(first->cuda.state));
    mu_assert("first context cleanup", vmaf_close(first) == 0);
    mu_assert("second context cleanup", vmaf_close(second) == 0);
    mu_assert("imported wrapper cleanup", vmaf_cuda_state_free(state) == 0);
    mu_assert("never-imported fixture cleanup", vmaf_cuda_state_free(other) == 0);
    return NULL;
}
#endif

static char *test_model_mount_with_use_features()
{
    int err = 0;

    VmafConfiguration vmaf_cfg = {0};

    VmafContext *vmaf = NULL;
    vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("problem during vmaf_init", vmaf);

    VmafModelConfig model_cfg = {0};
    VmafModel *model;
    vmaf_model_load(&model, &model_cfg, "vmaf_v0.6.1");
    mu_assert("problem during vmaf_model_load", model);

    err = vmaf_use_features_from_model(vmaf, model);
    mu_assert("problem during vmaf_use_features_from_model", !err);

    mu_assert("problem during vmaf_model_mount", vmaf->feature_collector->models);

    vmaf_model_destroy(model);
    err = vmaf_close(vmaf);
    mu_assert("problem During vmaf_close", !err);

    return NULL;
}

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: this C translation unit
 * stays portable to MSVC /std:clatest, which does not provide C23 nullptr. */
static const VmafOption model_value_capability_options[] = {
    {.name = "vif_kernelscale",
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 1.0,
     .min = 0.1,
     .max = 4.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY},
    {0},
};

typedef struct {
    bool setup_ok;
    bool selected_mock;
    const char *selected_name;
} ModelCapabilitySelection;

static ModelCapabilitySelection select_for_model_option(const char *value)
{
    VmafFeatureExtractor mock_gpu = {
        .name = "mock_float_vif_gpu",
        .options = model_value_capability_options,
        .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    };
    VmafModelFeature feature = {.name = "VMAF_feature_vif_scale0_score"};
    ModelCapabilitySelection result = {0};
    if (vmaf_dictionary_set(&feature.opts_dict, "vif_kernelscale", value, 0))
        return result;

    result.setup_ok = true;
    VmafFeatureExtractor *selected = fex_honouring_model_options(&mock_gpu, &feature);
    result.selected_mock = selected == &mock_gpu;
    result.selected_name = selected ? selected->name : NULL;
    (void)vmaf_dictionary_free(&feature.opts_dict);
    return result;
}

static char *test_model_option_minimum_falls_back(void)
{
    const ModelCapabilitySelection result = select_for_model_option("0.1");
    mu_assert("valid minimum must select CPU float_vif",
              result.setup_ok && !result.selected_mock && result.selected_name &&
                  !strcmp(result.selected_name, "float_vif"));
    return NULL;
}

static char *test_model_option_maximum_falls_back(void)
{
    const ModelCapabilitySelection result = select_for_model_option("4.0");
    mu_assert("valid maximum must select CPU float_vif",
              result.setup_ok && !result.selected_mock && result.selected_name &&
                  !strcmp(result.selected_name, "float_vif"));
    return NULL;
}

static char *test_model_option_default_preserves_gpu(void)
{
    const ModelCapabilitySelection result = select_for_model_option("1.0");
    mu_assert("declared default must preserve selected GPU twin",
              result.setup_ok && result.selected_mock && result.selected_name &&
                  !strcmp(result.selected_name, "mock_float_vif_gpu"));
    return NULL;
}

static char *test_model_option_invalid_stays_parser_owned(void)
{
    const ModelCapabilitySelection result = select_for_model_option("invalid");
    mu_assert("malformed value must remain on selected GPU for parser rejection",
              result.setup_ok && result.selected_mock && result.selected_name &&
                  !strcmp(result.selected_name, "mock_float_vif_gpu"));
    return NULL;
}

typedef struct {
    int scale;
} MockContextScaleState;

static int mock_context_scale_check(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                                    unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    const MockContextScaleState *s = fex->priv;
    const int scale = s->scale > 0 ? s->scale : (int)((float)(w < h ? w : h) / 256.0f + 0.5f);
    return (scale < 2) ? 0 : -ENOTSUP;
}

static const VmafOption mock_context_scale_options[] = {
    {.name = "scale",
     .type = VMAF_OPT_TYPE_INT,
     .default_val.i = 0,
     .min = 0,
     .max = 10,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0},
};

static bool context_fallback_case_matches(unsigned w, unsigned h, const char *scale,
                                          bool allow_fallback, bool expect_cpu)
{
    VmafContext *vmaf = NULL;
    if (vmaf_init(&vmaf, (VmafConfiguration){0}))
        return false;

    VmafDictionary *options = NULL;
    int err = vmaf_dictionary_set(&options, "scale", scale, 0);
    VmafFeatureExtractor mock_gpu = {
        .name = "mock_float_ssim_gpu",
        .options = mock_context_scale_options,
        .priv_size = sizeof(MockContextScaleState),
        .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
        .context_check = mock_context_scale_check,
        .context_fallback_name = "float_ssim",
    };
    VmafFeatureExtractorContext *ctx = NULL;
    if (!err)
        err = vmaf_feature_extractor_context_create(&ctx, &mock_gpu, options);
    if (err) {
        (void)vmaf_dictionary_free(&options);
        (void)vmaf_close(vmaf);
        return false;
    }

    ctx->allow_context_fallback = allow_fallback;
    err = feature_extractor_vector_append(&vmaf->registered_feature_extractors, ctx, 0);
    if (err) {
        (void)vmaf_feature_extractor_context_destroy(ctx);
        (void)vmaf_close(vmaf);
        return false;
    }
    vmaf->pic_params.w = w;
    vmaf->pic_params.h = h;
    vmaf->pic_params.bpc = 8;
    vmaf->pic_params.pix_fmt = VMAF_PIX_FMT_YUV420P;
    err = resolve_context_fallbacks(vmaf);

    ctx = vmaf->registered_feature_extractors.fex_ctx[0];
    const bool used_cpu = !strcmp(ctx->fex->name, "float_ssim");
    const VmafDictionaryEntry *scale_entry = vmaf_dictionary_get(&ctx->opts_dict, "scale", 0);
    const bool option_preserved = scale_entry && !strcmp(scale_entry->val, scale);
    err |= vmaf_close(vmaf);
    return !err && used_cpu == expect_cpu && option_preserved;
}

static char *test_context_fallback_threshold_and_direct_contract(void)
{
    mu_assert("320x240 auto-scale must stay on the selected GPU twin",
              context_fallback_case_matches(320, 240, "0", true, false));
    mu_assert("383x383 auto-scale must stay on the selected GPU twin",
              context_fallback_case_matches(383, 383, "0", true, false));
    mu_assert("384x384 auto-scale must fall back to CPU float_ssim",
              context_fallback_case_matches(384, 384, "0", true, true));
    mu_assert("960x540 auto-scale must fall back to CPU float_ssim",
              context_fallback_case_matches(960, 540, "0", true, true));
    mu_assert("explicit scale=1 must stay on the selected GPU twin",
              context_fallback_case_matches(960, 540, "1", true, false));
    mu_assert("direct extractor selection must retain its backend contract",
              context_fallback_case_matches(960, 540, "0", false, false));
    return NULL;
}
/* NOLINTEND(modernize-use-nullptr) */

static int load_three_test_models(VmafModel *models[3], const char *const names[3])
{
    for (unsigned k = 0; k < 3; k++) {
        VmafModelConfig cfg = {
            .name = names[k],
            .flags = VMAF_MODEL_FLAGS_DEFAULT,
        };
        int err = vmaf_model_load(&models[k], &cfg, "vmaf_v0.6.1");
        if (err || !models[k])
            return -1;
    }
    return 0;
}

static void destroy_three_test_models(VmafModel *models[3])
{
    for (unsigned k = 0; k < 3; k++)
        vmaf_model_destroy(models[k]);
}

static char *test_model_mount()
{
    VmafFeatureCollector *feature_collector;
    int err = vmaf_feature_collector_init(&feature_collector);
    mu_assert("problem during vmaf_feature_collector_init", !err);

    const char *const model_names[3] = {"vmaf_0", "vmaf_1", "vmaf_2"};
    VmafModel *models[3];
    err = load_three_test_models(models, model_names);
    mu_assert("problem during vmaf_model_load", !err);

    for (unsigned k = 0; k < 3; k++) {
        err = vmaf_feature_collector_mount_model(feature_collector, models[k]);
        mu_assert("problem during vmaf_model_mount", !err);
    }

    VmafPredictModel *it = feature_collector->models;
    for (unsigned i = 0; it; i++, it = it->next) {
        mu_assert("model name does not match mount order",
                  !strcmp(it->model->name, model_names[i]));
    }

    destroy_three_test_models(models);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_model_unmount()
{
    VmafFeatureCollector *feature_collector;
    int err = vmaf_feature_collector_init(&feature_collector);
    mu_assert("problem during vmaf_feature_collector_init", !err);

    const char *const model_names[3] = {"vmaf_0", "vmaf_1", "vmaf_2"};
    VmafModel *models[3];
    err = load_three_test_models(models, model_names);
    mu_assert("problem during vmaf_model_load", !err);

    for (unsigned k = 0; k < 3; k++) {
        err = vmaf_feature_collector_mount_model(feature_collector, models[k]);
        mu_assert("problem during vmaf_model_mount", !err);
    }
    for (unsigned k = 0; k < 3; k++) {
        err = vmaf_feature_collector_unmount_model(feature_collector, models[k]);
        mu_assert("problem during vmaf_model_unmount", !err);
    }

    mu_assert("feature_collector->models should be NULL", !feature_collector->models);

    destroy_three_test_models(models);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_aggregate_vector_init_append_and_destroy()
{
    int err = 0;

    AggregateVector aggregate_vector;
    err = aggregate_vector_init(&aggregate_vector);
    mu_assert("problem during aggregate_vector_init", !err);
    mu_assert("aggregate_vector is not initialized properly",
              (aggregate_vector.cnt == 0) && (aggregate_vector.capacity == 8));

    err = aggregate_vector_append(&aggregate_vector, "A", 1);
    mu_assert("problem during aggregate_vector_append", !err);
    mu_assert(
        "name and value were incorrectly set",
        (!strcmp("A", aggregate_vector.metric[0].name) && aggregate_vector.metric[0].value == 1));

    err |= aggregate_vector_append(&aggregate_vector, "B", 2);
    err |= aggregate_vector_append(&aggregate_vector, "C", 3);
    err |= aggregate_vector_append(&aggregate_vector, "D", 4);
    err |= aggregate_vector_append(&aggregate_vector, "E", 5);
    err |= aggregate_vector_append(&aggregate_vector, "F", 6);
    err |= aggregate_vector_append(&aggregate_vector, "G", 7);
    err |= aggregate_vector_append(&aggregate_vector, "H", 8);
    mu_assert("problem during aggregate_vector_append", !err);
    mu_assert("aggregate_vector is not sized properly",
              (aggregate_vector.cnt == 8) && (aggregate_vector.capacity == 8));

    err = aggregate_vector_append(&aggregate_vector, "I", 9);
    mu_assert("problem during aggregate_vector_append", !err);
    mu_assert("aggregate_vector has not realloc'd properly",
              (aggregate_vector.cnt == 9) && (aggregate_vector.capacity == 16));
    mu_assert(
        "name and value were incorrectly set",
        (!strcmp("I", aggregate_vector.metric[8].name) && aggregate_vector.metric[8].value == 9));

    aggregate_vector_destroy(&aggregate_vector);
    return NULL;
}

static char *test_feature_vector_init_append_and_destroy()
{
    int err;

    FeatureVector *feature_vector;
    err = feature_vector_init(&feature_vector, "psnr_y");
    mu_assert("problem during feature_vector_init", !err);

    unsigned initial_capacity = feature_vector->capacity;
    for (int j = initial_capacity - 1; j >= 0; j--) {
        err = feature_vector_append(feature_vector, j, 60.);
        mu_assert("problem during feature_vector_append", !err);
    }
    mu_assert("feature_vector->capacity should not have changed",
              feature_vector->capacity == initial_capacity);
    err = feature_vector_append(feature_vector, initial_capacity, 60.);
    mu_assert("problem during feature_vector_append", !err);
    mu_assert("feature_vector->capacity did not double its allocation",
              feature_vector->capacity == initial_capacity * 2);
    err = feature_vector_append(feature_vector, initial_capacity, 60.);
    mu_assert("feature_vector_append should not overwrite", err);

    feature_vector_destroy(feature_vector);
    return NULL;
}

/* Finding R2-5: feature_vector_append() must reject a pathological,
 * caller-controlled frame index instead of doubling `capacity` (unsigned)
 * until it wraps to 0.  Before the fix, an index near UINT_MAX wrapped the
 * doubling loop to a 0 capacity — under NDEBUG the assert is compiled out, so
 * realloc(p, 0) leaves capacity stuck at 0 and `index >= 0` spins forever (and
 * before that it tries multi-gigabyte allocations).  This test passes a huge
 * index and asserts a clean error return; without the guard it would hang or
 * OOM rather than return. */
static char *test_feature_vector_append_rejects_huge_index()
{
    int err;

    FeatureVector *feature_vector;
    err = feature_vector_init(&feature_vector, "psnr_y");
    mu_assert("problem during feature_vector_init", !err);

    const unsigned capacity_before = feature_vector->capacity;

    err = feature_vector_append(feature_vector, FEATURE_VECTOR_MAX_INDEX, 60.);
    mu_assert("feature_vector_append must reject index == FEATURE_VECTOR_MAX_INDEX", err);

    err = feature_vector_append(feature_vector, UINT_MAX, 60.);
    mu_assert("feature_vector_append must reject UINT_MAX index", err);

    /* The rejection happens before any realloc, so capacity is untouched. */
    mu_assert("rejected index must not grow capacity", feature_vector->capacity == capacity_before);

    /* A legitimate, modest index must still be accepted and grow the array —
     * the guard rejects only the pathological range, not valid frame indices. */
    err = feature_vector_append(feature_vector, 1000u, 60.);
    mu_assert("feature_vector_append must accept a legitimate index", !err);
    mu_assert("legitimate index must grow capacity past it", feature_vector->capacity > 1000u);

    feature_vector_destroy(feature_vector);
    return NULL;
}

static char *test_feature_collector_init_append_get_and_destroy()
{
    int err;

    VmafFeatureCollector *feature_collector;
    err = vmaf_feature_collector_init(&feature_collector);
    mu_assert("problem during vmaf_feature_collector_init", !err);
    unsigned initial_capacity = feature_collector->capacity;
    mu_assert("this test assumes an initial capacity of 8", initial_capacity == 8);
    err = vmaf_feature_collector_append(feature_collector, "feature0", 60., 1);
    err |= vmaf_feature_collector_append(feature_collector, "feature1", 60., 1);
    err |= vmaf_feature_collector_append(feature_collector, "feature2", 60., 1);
    err |= vmaf_feature_collector_append(feature_collector, "feature3", 60., 1);
    err |= vmaf_feature_collector_append(feature_collector, "feature4", 60., 1);
    err |= vmaf_feature_collector_append(feature_collector, "feature5", 60., 1);
    err |= vmaf_feature_collector_append(feature_collector, "feature6", 60., 1);
    err |= vmaf_feature_collector_append(feature_collector, "feature7", 60., 1);
    mu_assert("problem during vmaf_feature_collector_append", !err);
    mu_assert("feature_collector->capacity should not have changed",
              feature_collector->capacity == initial_capacity);
    err = vmaf_feature_collector_append(feature_collector, "feature8", 60., 1);
    mu_assert("problem during vmaf_feature_collector_append", !err);
    mu_assert("feature_collector->capacity did not double its allocation",
              feature_collector->capacity == initial_capacity * 2);

    double score;
    err = vmaf_feature_collector_get_score(feature_collector, "feature5", &score, 1);
    mu_assert("problem during vmaf_feature_collector_get_score", !err);
    mu_assert("vmaf_feature_collector_get_score did not get the expected score", score == 60.);
    err = vmaf_feature_collector_get_score(feature_collector, "feature5", &score, 2);
    mu_assert("vmaf_feature_collector_get_score did not fail with bad index", err);

    err = vmaf_feature_collector_set_aggregate(feature_collector, "aggregate0", 100.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate1", 101.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate2", 102.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate3", 103.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate4", 104.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate5", 105.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate6", 106.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate7", 107.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate8", 108.);
    err |= vmaf_feature_collector_set_aggregate(feature_collector, "aggregate9", 109.);
    mu_assert("problem during vmaf_feature_collector_set_aggregate", !err);

    err = vmaf_feature_collector_get_aggregate(feature_collector, "aggregate5", &score);
    mu_assert("problem during vmaf_feature_collector_get_aggregate", !err);
    mu_assert("unexpected aggreggate_score", score == 105.);
    err = vmaf_feature_collector_get_aggregate(feature_collector, "aggregate9", &score);
    mu_assert("problem during vmaf_feature_collector_get_aggregate", !err);
    mu_assert("unexpected aggreggate_score", score == 109.);

    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *run_model_option_capability_tests(void)
{
    mu_run_test(test_model_option_minimum_falls_back);
    mu_run_test(test_model_option_maximum_falls_back);
    mu_run_test(test_model_option_default_preserves_gpu);
    mu_run_test(test_model_option_invalid_stays_parser_owned);
    mu_run_test(test_context_fallback_threshold_and_direct_contract);
    return NULL; /* NOLINT(modernize-use-nullptr) -- ADR-1138: MSVC C23 portability. */
}

char *run_tests()
{
    mu_run_test(test_vmaf_close_retains_context_for_retry);
    mu_run_test(test_vmaf_close_retains_worker_private_context);
#ifdef HAVE_CUDA
    mu_run_test(test_vmaf_close_commit_retry_is_idempotent);
    mu_run_test(test_cuda_state_import_rejects_duplicate_owners);
#endif
    mu_run_test(test_feature_vector_init_append_and_destroy);
    mu_run_test(test_feature_vector_append_rejects_huge_index);
    mu_run_test(test_feature_collector_init_append_get_and_destroy);
    mu_run_test(test_aggregate_vector_init_append_and_destroy);
    mu_run_test(test_model_mount);
    mu_run_test(test_model_unmount);
    mu_run_test(test_model_mount_with_use_features);
    return run_model_option_capability_tests();
}
