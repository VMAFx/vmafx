/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  DISTS-Sq feature extractor — structural + missing-model tests.
 *
 *  Full inference is exercised by the DNN smoke gate when
 *  model/tiny/dists_sq.onnx is available; this file verifies the
 *  registration / option-table / init-rejection contract.
 */

#include "tiny_ai_test_template.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#include "dnn/tiny_extractor_template.h"

/* The registration tests set the model-path environment variable they
 * exercise; the binary is single-threaded (ADR-0141). */
/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS("dists_sq", "dists_sq", "VMAF_DISTS_SQ_MODEL_PATH", dists_sq)

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xffu);
    dst[1] = (uint8_t)(v >> 8u);
}

static char *test_dists_high_bitdepth_rgb_normalisation(void)
{
    uint8_t y8[4] = {64u, 96u, 128u, 160u};
    uint8_t u8[1] = {128u};
    uint8_t v8[1] = {128u};
    uint8_t y10[8];
    uint8_t u10[2];
    uint8_t v10[2];
    for (unsigned i = 0u; i < 4u; ++i) {
        put_le16(y10 + (size_t)i * 2u, (uint16_t)y8[i] << 2u);
    }
    put_le16(u10, (uint16_t)u8[0] << 2u);
    put_le16(v10, (uint16_t)v8[0] << 2u);

    VmafPicture pic8 = {
        .pix_fmt = VMAF_PIX_FMT_YUV420P,
        .bpc = 8u,
        .w = {2u, 1u, 1u},
        .h = {2u, 1u, 1u},
        .stride = {2, 1, 1},
        .data = {y8, u8, v8},
    };
    VmafPicture pic10 = {
        .pix_fmt = VMAF_PIX_FMT_YUV420P,
        .bpc = 10u,
        .w = {2u, 1u, 1u},
        .h = {2u, 1u, 1u},
        .stride = {4, 2, 2},
        .data = {y10, u10, v10},
    };
    uint8_t r8[4] = {0};
    uint8_t g8[4] = {0};
    uint8_t b8[4] = {0};
    uint8_t r10[4] = {0};
    uint8_t g10[4] = {0};
    uint8_t b10[4] = {0};

    int rc = vmaf_tiny_ai_yuv_to_rgb8_planes(&pic8, r8, g8, b8);
    mu_assert("8-bit conversion must pass", rc == 0);
    rc = vmaf_tiny_ai_yuv_to_rgb8_planes(&pic10, r10, g10, b10);
    mu_assert("10-bit conversion must pass", rc == 0);
    mu_assert("10-bit R plane must normalise to matching 8-bit RGB",
              memcmp(r8, r10, sizeof(r8)) == 0);
    mu_assert("10-bit G plane must normalise to matching 8-bit RGB",
              memcmp(g8, g10, sizeof(g8)) == 0);
    mu_assert("10-bit B plane must normalise to matching 8-bit RGB",
              memcmp(b8, b10, sizeof(b8)) == 0);
    return NULL;
}

/* vmaf_tiny_ai_resolve_model_path(): the option wins, an environment value is
 * copied into the caller's buffer (on Windows the environment is read into a
 * per-thread buffer the loader's own read reuses), and a value longer than the
 * buffer is refused rather than truncated. */
#define RESOLVE_ENV "VMAF_DISTS_SQ_MODEL_PATH"

static char *check_resolve_option_wins(void)
{
    char buf[VMAF_TINY_AI_ENV_PATH_MAX];
    mu_assert("setenv failed", vmaf_tiny_ai_test_setenv(RESOLVE_ENV, "/env/model.onnx") == 0);
    const char *opt = "/opt/model.onnx";
    const char *path =
        vmaf_tiny_ai_resolve_model_path("dists_sq", opt, RESOLVE_ENV, buf, sizeof(buf));
    (void)vmaf_tiny_ai_test_unsetenv(RESOLVE_ENV);
    mu_assert("option value not returned", path == opt);
    return NULL;
}

static char *check_resolve_copies_env(void)
{
    char buf[VMAF_TINY_AI_ENV_PATH_MAX] = {0};
    mu_assert("setenv failed", vmaf_tiny_ai_test_setenv(RESOLVE_ENV, "/env/model.onnx") == 0);
    const char *path =
        vmaf_tiny_ai_resolve_model_path("dists_sq", NULL, RESOLVE_ENV, buf, sizeof(buf));
    /* A later change of the environment leaves the resolved path as it was. */
    mu_assert("setenv failed", vmaf_tiny_ai_test_setenv(RESOLVE_ENV, "/other/x.onnx") == 0);
    (void)vmaf_tiny_ai_test_unsetenv(RESOLVE_ENV);
    mu_assert("environment value not copied into the caller's buffer", path == buf);
    mu_assert("copied value changed with the environment", strcmp(buf, "/env/model.onnx") == 0);
    return NULL;
}

static char *check_resolve_refuses_long_env(void)
{
    static char value[VMAF_TINY_AI_ENV_PATH_MAX + 1u];
    for (size_t i = 0; i + 1u < sizeof(value); i++)
        value[i] = 'a';
    value[sizeof(value) - 1u] = '\0';
    char buf[VMAF_TINY_AI_ENV_PATH_MAX];
    mu_assert("setenv failed", vmaf_tiny_ai_test_setenv(RESOLVE_ENV, value) == 0);
    const char *path =
        vmaf_tiny_ai_resolve_model_path("dists_sq", NULL, RESOLVE_ENV, buf, sizeof(buf));
    value[sizeof(value) - 2u] = '\0'; /* VMAF_TINY_AI_ENV_PATH_MAX - 1 bytes: fits exactly */
    mu_assert("setenv failed", vmaf_tiny_ai_test_setenv(RESOLVE_ENV, value) == 0);
    const char *fits =
        vmaf_tiny_ai_resolve_model_path("dists_sq", NULL, RESOLVE_ENV, buf, sizeof(buf));
    (void)vmaf_tiny_ai_test_unsetenv(RESOLVE_ENV);
    mu_assert("over-long environment value accepted", path == NULL);
    mu_assert("value of buffer size - 1 refused", fits == buf && strlen(buf) == sizeof(buf) - 1u);
    return NULL;
}

static char *test_resolve_model_path(void)
{
    char *err = check_resolve_option_wins();
    if (!err)
        err = check_resolve_copies_env();
    if (!err)
        err = check_resolve_refuses_long_env();
    return err;
}

char *run_tests(void)
{
    VMAF_TINY_AI_RUN_REGISTRATION_TESTS(dists_sq);
    mu_run_test(test_dists_high_bitdepth_rgb_normalisation);
    mu_run_test(test_resolve_model_path);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
