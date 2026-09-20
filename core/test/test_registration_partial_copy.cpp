/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#include "dict.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "test.h"

#include <atomic>
#include <cerrno>
#include <memory>

namespace
{
std::atomic<bool> fail_next_copy{false};
std::atomic<bool> partial_copy_observed{false};

struct PartialCopyContextDeleter {
    void operator()(VmafContext *ctx) const noexcept
    {
        (void)vmaf_close(ctx);
    }
};

struct PartialCopyModelDeleter {
    void operator()(VmafModel *model) const noexcept
    {
        vmaf_model_destroy(model);
    }
};

int motion_options(VmafFeatureDictionary **options)
{
    int err = vmaf_feature_dictionary_set(options, "motion_force_zero", "true");
    if (!err)
        err = vmaf_feature_dictionary_set(options, "motion_blend_offset", "10");
    if (err)
        (void)vmaf_feature_dictionary_free(options);
    return err;
}
} // namespace

namespace
{
mu_message_t test_explicit_partial_copy()
{
    VmafContext *ctx = nullptr;
    const VmafConfiguration config{};
    mu_assert("context initialization failed", vmaf_init(&ctx, config) == 0);
    const std::unique_ptr<VmafContext, PartialCopyContextDeleter> context(ctx);
    VmafFeatureDictionary *options = nullptr;
    mu_assert("options creation failed", motion_options(&options) == 0);
    partial_copy_observed.store(false);
    fail_next_copy.store(true);
    // This call consumes the supplied dictionary even when its copy fails.
    mu_assert("copy failure must propagate", vmaf_use_feature(ctx, "motion", options) == -ENOMEM);
    mu_assert("the failed copy was not partial", partial_copy_observed.load());
    options = nullptr;
    mu_assert("retry options creation failed", motion_options(&options) == 0);
    mu_assert("registration retry failed", vmaf_use_feature(ctx, "motion", options) == 0);
    return nullptr;
}
} // namespace

namespace
{
mu_message_t test_model_partial_copy()
{
    VmafContext *ctx = nullptr;
    const VmafConfiguration config{};
    mu_assert("context initialization failed", vmaf_init(&ctx, config) == 0);
    const std::unique_ptr<VmafContext, PartialCopyContextDeleter> context(ctx);
    VmafModel *raw_model = nullptr;
    VmafModelConfig model_config{};
    mu_assert("model load failed",
              vmaf_model_load(&raw_model, &model_config, VMAF_NETFLIX_COMPAT_MODEL_VERSION) == 0);
    const std::unique_ptr<VmafModel, PartialCopyModelDeleter> model(raw_model);
    VmafFeatureDictionary *options = nullptr;
    mu_assert("options creation failed", motion_options(&options) == 0);
    mu_assert("model override failed",
              vmaf_model_feature_overload(model.get(), "motion", options) == 0);
    partial_copy_observed.store(false);
    fail_next_copy.store(true);
    mu_assert("copy failure must propagate",
              vmaf_use_features_from_model(ctx, model.get()) == -ENOMEM);
    mu_assert("the failed copy was not partial", partial_copy_observed.load());
    // The model retains its original dictionary and can be registered again.
    mu_assert("registration retry failed", vmaf_use_features_from_model(ctx, model.get()) == 0);
    return nullptr;
}
} // namespace

extern "C" int
real_vmaf_dictionary_copy(VmafDictionary **src,
                          VmafDictionary **dst) __asm__("__real_vmaf_dictionary_copy");

/* Visible on purpose, as in test_fex_ctx_vector.cpp: the library builds with
 * -fvisibility=hidden, and the linker must reach this wrapper. */
#define VMAF_WRAP_EXPORT __attribute__((visibility("default")))

// Research-2048: `-Wl,--wrap=vmaf_dictionary_copy` routes every library call to
// this wrapper, which fails only the requested copy; every other call runs the
// real implementation. The test links the static archive because --wrap rewrites
// references at link time. It used to interpose the symbol in libvmaf.so, which
// worked only while the shared library exported this internal function.

extern "C" VMAF_WRAP_EXPORT int
wrap_vmaf_dictionary_copy(VmafDictionary **src,
                          VmafDictionary **dst) __asm__("__wrap_vmaf_dictionary_copy");
extern "C" VMAF_WRAP_EXPORT int wrap_vmaf_dictionary_copy(VmafDictionary **src,
                                                          VmafDictionary **dst)
{
    if (fail_next_copy.exchange(false)) {
        if (!src || !*src || !dst || (*src)->cnt < 2)
            return -EINVAL;
        const VmafDictionaryEntry first = (*src)->entry[0];
        const int err = vmaf_dictionary_set(dst, first.key, first.val, 0);
        if (err)
            return err;
        partial_copy_observed.store((*dst)->cnt == 1 && (*src)->cnt >= 2);
        return -ENOMEM;
    }
    return real_vmaf_dictionary_copy(src, dst);
}

mu_message_t run_tests()
{
    mu_run_test(test_explicit_partial_copy);
    mu_run_test(test_model_partial_copy);
    return nullptr;
}
