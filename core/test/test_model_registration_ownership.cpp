/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "test.h"

#include <cerrno>
#include <memory>

namespace
{

struct ContextDeleter {
    void operator()(VmafContext *ctx) const noexcept
    {
        (void)vmaf_close(ctx);
    }
};

struct ModelDeleter {
    void operator()(VmafModel *model) const noexcept
    {
        vmaf_model_destroy(model);
    }
};

int overload_motion(VmafModel *model, const char *key, const char *value)
{
    VmafFeatureDictionary *options = nullptr;
    const int err = vmaf_feature_dictionary_set(&options, key, value);
    if (err) {
        (void)vmaf_feature_dictionary_free(&options);
        return err;
    }
    // A valid model/feature call consumes options, including on failure.
    return vmaf_model_feature_overload(model, "motion", options);
}

mu_message_t check_rejected_options(const char *key, const char *value)
{
    VmafContext *ctx = nullptr;
    VmafConfiguration config{};
    config.log_level = VMAF_LOG_LEVEL_NONE;
    mu_assert("context initialization failed", vmaf_init(&ctx, config) == 0);
    const std::unique_ptr<VmafContext, ContextDeleter> context(ctx);

    VmafModel *raw_model = nullptr;
    VmafModelConfig model_config{};
    mu_assert("model load failed",
              vmaf_model_load(&raw_model, &model_config, VMAF_NETFLIX_COMPAT_MODEL_VERSION) == 0);
    const std::unique_ptr<VmafModel, ModelDeleter> model(raw_model);
    mu_assert("model option override failed", overload_motion(model.get(), key, value) == 0);

    // Registration owns only a copy: repeated rejection must not consume the
    // model's dictionary. LeakSanitizer observes the failed copies at exit.
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        mu_assert("invalid model options must be rejected",
                  vmaf_use_features_from_model(context.get(), model.get()) == -EINVAL);
    }
    return nullptr;
}

mu_message_t test_invalid_option_value()
{
    return check_rejected_options("motion_force_zero", "invalid");
}

mu_message_t test_unknown_option()
{
    return check_rejected_options("motion_unknown_option", "true");
}

mu_message_t test_valid_options_transfer()
{
    VmafContext *ctx = nullptr;
    const VmafConfiguration config{};
    mu_assert("context initialization failed", vmaf_init(&ctx, config) == 0);
    const std::unique_ptr<VmafContext, ContextDeleter> context(ctx);
    VmafModel *raw_model = nullptr;
    VmafModelConfig model_config{};
    mu_assert("model load failed",
              vmaf_model_load(&raw_model, &model_config, VMAF_NETFLIX_COMPAT_MODEL_VERSION) == 0);
    const std::unique_ptr<VmafModel, ModelDeleter> model(raw_model);
    mu_assert("model option override failed",
              overload_motion(model.get(), "motion_force_zero", "true") == 0);
    mu_assert("valid model registration failed",
              vmaf_use_features_from_model(context.get(), model.get()) == 0);
    return nullptr;
}

} // namespace

mu_message_t run_tests()
{
    mu_run_test(test_invalid_option_value);
    mu_run_test(test_unknown_option);
    mu_run_test(test_valid_options_transfer);
    return nullptr;
}
