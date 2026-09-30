/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * ADR-1359: vmaf_feature_backend_twin() and vmaf_registered_feature_extractor().
 * White-box: mock extractors drive the option and geometry verdicts, and a
 * stand-in backend state makes compute_fex_flags() select the compiled
 * backend's real registry entries without a device.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"
#include "dict.h"
#include "feature/feature_extractor.h"
#include "libvmaf_priv.h"
#include "mu_table.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

typedef struct {
    int scale;
    double kernelscale;
} MockTwinState;

static const VmafOption mock_twin_options[] = {
    {.name = "scale",
     .type = VMAF_OPT_TYPE_INT,
     .offset = offsetof(MockTwinState, scale),
     .default_val.i = 0,
     .min = 0,
     .max = 10,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_kernelscale",
     .type = VMAF_OPT_TYPE_DOUBLE,
     .offset = offsetof(MockTwinState, kernelscale),
     .default_val.d = 1.0,
     .min = 0.1,
     .max = 4.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY},
    {0},
};

/* Same auto-scale rule as the GPU float_ssim twins (ADR-1324). */
static int mock_twin_check(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                           unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    const MockTwinState *s = fex->priv;
    const int scale = s->scale > 0 ? s->scale : (int)((float)(w < h ? w : h) / 256.0f + 0.5f);
    return scale < 2 ? 0 : -ENOTSUP;
}

static VmafFeatureExtractor mock_twin(bool with_check)
{
    return (VmafFeatureExtractor){
        .name = "mock_twin_sycl",
        .options = mock_twin_options,
        .priv_size = sizeof(MockTwinState),
        .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
        .context_check = with_check ? mock_twin_check : NULL,
    };
}

static VmafPictureConfiguration geometry(unsigned w, unsigned h)
{
    return (VmafPictureConfiguration){
        .pic_params = {.w = w, .h = h, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P},
    };
}

/* Verdict for one option on the mock twin; *key receives the reported key. */
static int verdict_for(const char *key, const char *value, bool with_check,
                       const VmafPictureConfiguration *pic_cfg, const char **reported)
{
    VmafDictionary *opts = NULL;
    if (key && vmaf_dictionary_set(&opts, key, value, 0))
        return -ENOMEM;
    const VmafFeatureExtractor twin = mock_twin(with_check);
    const char *found = "stale";
    const int err = vmaf_backend_twin_verdict_for_test(&twin, opts, pic_cfg, &found);
    /* The key points into opts; compare it before the dictionary goes. */
    *reported = found && key && !strcmp(found, key) ? key : found;
    (void)vmaf_dictionary_free(&opts);
    return err;
}

static char *test_verdict_honours_options(void)
{
    const char *reported = NULL;
    mu_assert("no options must keep the twin",
              verdict_for(NULL, NULL, false, NULL, &reported) == 0 && reported == NULL);
    mu_assert("declared default must keep the twin",
              verdict_for("vif_kernelscale", "1.0", false, NULL, &reported) == 0);
    mu_assert("default-only option at a valid non-default must fall back",
              verdict_for("vif_kernelscale", "4.0", false, NULL, &reported) == -ENOTSUP &&
                  reported && !strcmp(reported, "vif_kernelscale"));
    mu_assert("option the twin lacks must fall back and be named",
              verdict_for("enable_lcs", "true", false, NULL, &reported) == -ENOTSUP && reported &&
                  !strcmp(reported, "enable_lcs"));
    return NULL;
}

static char *test_verdict_checks_geometry(void)
{
    const char *reported = NULL;
    const VmafPictureConfiguration qhd = geometry(960, 540);
    const VmafPictureConfiguration edge = geometry(383, 383);
    mu_assert("auto-scale above 1 must fall back without naming an option",
              verdict_for("scale", "0", true, &qhd, &reported) == -ENOTSUP && reported == NULL);
    mu_assert("383x383 still resolves scale 1",
              verdict_for("scale", "0", true, &edge, &reported) == 0);
    const VmafPictureConfiguration above_edge = geometry(384, 384);
    mu_assert("384x384 resolves scale 2",
              verdict_for("scale", "0", true, &above_edge, &reported) == -ENOTSUP);
    mu_assert("explicit scale=1 keeps the twin",
              verdict_for("scale", "1", true, &qhd, &reported) == 0);
    mu_assert("no geometry skips the check", verdict_for("scale", "0", true, NULL, &reported) == 0);
    mu_assert("unparsable value surfaces the parser error",
              verdict_for("scale", "not-a-number", true, &qhd, &reported) == -EINVAL);
    return NULL;
}

static char *test_twin_lookup_guards(void)
{
    const char *twin = "stale";
    const char *option = "stale";
    mu_assert("NULL context",
              vmaf_feature_backend_twin(NULL, "ciede", NULL, NULL, &twin, &option) == -EINVAL &&
                  option == NULL);

    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", !vmaf_init(&vmaf, (VmafConfiguration){0}));
    mu_assert("NULL name",
              vmaf_feature_backend_twin(vmaf, NULL, NULL, NULL, &twin, NULL) == -EINVAL);
    mu_assert("NULL result",
              vmaf_feature_backend_twin(vmaf, "ciede", NULL, NULL, NULL, NULL) == -EINVAL);
    mu_assert("vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

static char *test_twin_lookup_without_device(void)
{
    const char *twin = "stale";
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", !vmaf_init(&vmaf, (VmafConfiguration){0}));
    mu_assert("unknown extractor", vmaf_feature_backend_twin(vmaf, "no_such_extractor", NULL, NULL,
                                                             &twin, NULL) == -EINVAL &&
                                       twin == NULL);
    mu_assert("a CPU-only context has no device backend",
              vmaf_feature_backend_twin(vmaf, "ciede", NULL, NULL, &twin, NULL) == -ENODEV &&
                  twin == NULL);
    mu_assert("vmaf_close", !vmaf_close(vmaf));

    mu_assert("NULL extractor has no twin",
              vmaf_get_feature_extractor_twin(NULL, VMAF_FEATURE_EXTRACTOR_SYCL) == NULL);
    mu_assert("no flags means no twin",
              vmaf_get_feature_extractor_twin(vmaf_get_feature_extractor_by_name("ciede"), 0) ==
                  NULL);
    return NULL;
}

static bool twin_provides(const char *twin, unsigned flag, const char *feature)
{
    const VmafFeatureExtractor *fex = twin ? vmaf_get_feature_extractor_by_name(twin) : NULL;
    if (!fex || !(fex->flags & flag) || !fex->provided_features)
        return false;
    for (unsigned i = 0; i < 64U && fex->provided_features[i]; i++) {
        if (!strcmp(fex->provided_features[i], feature))
            return true;
    }
    return false;
}

static char *check_registry_twins(VmafContext *vmaf, unsigned flag)
{
    const char *twin = NULL;
    const char *option = NULL;
    mu_assert("ciede must map to the backend's ciede2000 twin",
              vmaf_feature_backend_twin(vmaf, "ciede", NULL, NULL, &twin, &option) == 0 &&
                  twin_provides(twin, flag, "ciede2000") && option == NULL);
    const char *again = NULL;
    mu_assert("a twin name is not a CPU extractor",
              vmaf_feature_backend_twin(vmaf, twin, NULL, NULL, &again, NULL) == -EINVAL);
    mu_assert("brisque has no device twin",
              vmaf_feature_backend_twin(vmaf, "brisque", NULL, NULL, &twin, NULL) == -ENOENT &&
                  twin == NULL);

    const VmafPictureConfiguration small = geometry(320, 240);
    const VmafPictureConfiguration qhd = geometry(960, 540);
    mu_assert("float_ssim twin runs 320x240",
              vmaf_feature_backend_twin(vmaf, "float_ssim", NULL, &small, &twin, NULL) == 0);
#if defined(HAVE_SYCL)
    /* ADR-1370: the SYCL twin decimates on the device. */
    const int qhd_verdict = 0;
#else
    const int qhd_verdict = -ENOTSUP;
#endif
    mu_assert("float_ssim twin auto-scales 960x540 only where it decimates",
              vmaf_feature_backend_twin(vmaf, "float_ssim", NULL, &qhd, &twin, &option) ==
                      qhd_verdict &&
                  twin != NULL && option == NULL);
    /* Every twin refuses a plane decimated below the 11x11 Gaussian. */
    VmafDictionary *scale10 = NULL;
    mu_assert("scale option", !vmaf_dictionary_set(&scale10, "scale", "10", 0));
    const VmafPictureConfiguration tiny = geometry(100, 100);
    const int tiny_verdict = vmaf_feature_backend_twin(
        vmaf, "float_ssim", (const VmafFeatureDictionary *)scale10, &tiny, &twin, &option);
    (void)vmaf_dictionary_free(&scale10);
    mu_assert("float_ssim twin cannot run 100x100 at scale=10",
              tiny_verdict == -ENOTSUP && twin != NULL && option == NULL);
    return NULL;
}

static char *test_twin_lookup_on_imported_backend(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", !vmaf_init(&vmaf, (VmafConfiguration){0}));
    unsigned char token = 0;
    const unsigned flag = vmaf_context_fake_backend_for_test(vmaf, &token);
    char *msg = flag ? check_registry_twins(vmaf, flag) : NULL;
#if defined(HAVE_SYCL) || defined(HAVE_CUDA)
    if (!msg && flag) {
        vmaf_context_set_gpumask_for_test(vmaf, 1);
        const char *twin = NULL;
        if (vmaf_feature_backend_twin(vmaf, "ciede", NULL, NULL, &twin, NULL) != -ENODEV)
            msg = "a non-zero gpumask must disable the twin lookup";
        vmaf_context_set_gpumask_for_test(vmaf, 0);
    }
#endif
    (void)vmaf_context_fake_backend_for_test(vmaf, NULL);
    mu_assert("vmaf_close", !vmaf_close(vmaf));
    return msg;
}

static VmafFeatureExtractor mock_named(const char *name, uint64_t flags)
{
    return (VmafFeatureExtractor){.name = name, .flags = flags};
}

static bool reports(VmafContext *vmaf, unsigned index, const char *want_name,
                    enum VmafBackend want_backend)
{
    const char *name = NULL;
    enum VmafBackend backend = VMAF_BACKEND_VULKAN;
    if (vmaf_registered_feature_extractor(vmaf, index, &name, &backend))
        return false;
    return name && !strcmp(name, want_name) && backend == want_backend;
}

/* psnr (CPU), then a SYCL and a Metal mock, in that order. */
static bool register_three(VmafContext *vmaf)
{
    const VmafFeatureExtractor sycl = mock_named("mock_sycl", VMAF_FEATURE_EXTRACTOR_SYCL);
    const VmafFeatureExtractor metal = mock_named("mock_metal", VMAF_FEATURE_EXTRACTOR_METAL);
    return !vmaf_use_feature(vmaf, "psnr", NULL) &&
           !vmaf_context_append_registered_feature_extractor_for_test(vmaf, &sycl, false) &&
           !vmaf_context_append_registered_feature_extractor_for_test(vmaf, &metal, false);
}

static char *test_registered_extractors_report_backends(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", !vmaf_init(&vmaf, (VmafConfiguration){0}));
    mu_assert("register", register_three(vmaf));
    mu_assert("CPU extractor", reports(vmaf, 0, "psnr", VMAF_BACKEND_UNKNOWN));
    mu_assert("SYCL extractor", reports(vmaf, 1, "mock_sycl", VMAF_BACKEND_SYCL));
    mu_assert("Metal extractor", reports(vmaf, 2, "mock_metal", VMAF_BACKEND_METAL));
    const char *name = NULL;
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    mu_assert("past the end",
              vmaf_registered_feature_extractor(vmaf, 3, &name, &backend) == -ENOENT);
    mu_assert("vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

static char *test_registered_extractors_empty(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", !vmaf_init(&vmaf, (VmafConfiguration){0}));
    const char *name = NULL;
    enum VmafBackend backend = VMAF_BACKEND_CUDA;
    mu_assert("empty context",
              vmaf_registered_feature_extractor(vmaf, 0, &name, &backend) == -ENOENT);
    mu_assert("vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

static char *test_registered_extractor_guards(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", !vmaf_init(&vmaf, (VmafConfiguration){0}));
    mu_assert("register psnr", !vmaf_use_feature(vmaf, "psnr", NULL));
    const char *name = NULL;
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    mu_assert("NULL context",
              vmaf_registered_feature_extractor(NULL, 0, &name, &backend) == -EINVAL);
    mu_assert("NULL name", vmaf_registered_feature_extractor(vmaf, 0, NULL, &backend) == -EINVAL);
    mu_assert("NULL backend", vmaf_registered_feature_extractor(vmaf, 0, &name, NULL) == -EINVAL);
    mu_assert("vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

/* The report is taken after the first picture: a model-selected twin replaced
 * by its CPU extractor (ADR-1324) must be reported as the CPU extractor. */
static char *test_report_follows_context_fallback(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", !vmaf_init(&vmaf, (VmafConfiguration){0}));
    VmafFeatureExtractor twin = mock_twin(true);
    twin.context_fallback_name = "float_ssim";
    mu_assert("append",
              !vmaf_context_append_registered_feature_extractor_for_test(vmaf, &twin, true));
    const char *name = NULL;
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    mu_assert("twin reported before the first picture",
              !vmaf_registered_feature_extractor(vmaf, 0, &name, &backend) &&
                  backend == VMAF_BACKEND_SYCL);

    const VmafPictureConfiguration cfg = geometry(960, 540);
    mu_assert("fallback", !vmaf_context_resolve_context_fallbacks_for_test(vmaf, &cfg));
    mu_assert("CPU extractor reported after the fallback",
              !vmaf_registered_feature_extractor(vmaf, 0, &name, &backend) &&
                  !strcmp(name, "float_ssim") && backend == VMAF_BACKEND_UNKNOWN);
    mu_assert("vmaf_close", !vmaf_close(vmaf));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_verdict_honours_options),
        MU_TEST(test_verdict_checks_geometry),
        MU_TEST(test_twin_lookup_guards),
        MU_TEST(test_twin_lookup_without_device),
        MU_TEST(test_twin_lookup_on_imported_backend),
        MU_TEST(test_registered_extractors_report_backends),
        MU_TEST(test_registered_extractor_guards),
        MU_TEST(test_registered_extractors_empty),
        MU_TEST(test_report_follows_context_fallback),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
