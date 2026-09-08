/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "libvmaf/libvmaf.h"
#include "test.h"

#include <cstring>
#include <memory>

namespace
{

struct VmafDeleter {
    void operator()(VmafContext *ctx) const noexcept
    {
        (void)vmaf_close(ctx);
    }
};

int register_public_motion(VmafContext *vmaf, const char *value)
{
    VmafFeatureDictionary *options = nullptr;
    const int err = vmaf_feature_dictionary_set(&options, "motion_force_zero", value);
    if (err) {
        (void)vmaf_feature_dictionary_free(&options);
        return err;
    }
    // vmaf_use_feature consumes the dictionary, including on failure.
    return vmaf_use_feature(vmaf, "motion", options);
}

int read_blank_frame(VmafContext *vmaf)
{
    VmafPicture ref{};
    VmafPicture dis{};
    int err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8, 64, 64);
    if (err)
        return err;
    err = vmaf_picture_alloc(&dis, VMAF_PIX_FMT_YUV420P, 8, 64, 64);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    for (unsigned p = 0; p < 3; p++) {
        memset(ref.data[p], 0, static_cast<size_t>(ref.stride[p]) * ref.h[p]);
        memset(dis.data[p], 0, static_cast<size_t>(dis.stride[p]) * dis.h[p]);
    }
    return vmaf_read_pictures(vmaf, &ref, &dis, 0);
}

mu_message_t test_public_option_registration()
{
    VmafContext *ctx = nullptr;
    VmafConfiguration cfg{};
    cfg.log_level = VMAF_LOG_LEVEL_ERROR;
    mu_assert("vmaf_init failed", vmaf_init(&ctx, cfg) == 0);
    const std::unique_ptr<VmafContext, VmafDeleter> vmaf(ctx);
    const int first = register_public_motion(ctx, "false");
    const int second = first ? first : register_public_motion(ctx, "true");
    mu_assert("public registration failed", second == 0);
    mu_assert("frame extraction failed", read_blank_frame(ctx) == 0);
    mu_assert("flush failed", vmaf_read_pictures(ctx, nullptr, nullptr, 0) == 0);
    double score = -1;
    mu_assert("default score missing",
              vmaf_feature_score_at_index(ctx, "VMAF_integer_feature_motion2_score", &score, 0) ==
                  0);
    mu_assert("option-specific score missing",
              vmaf_feature_score_at_index(ctx, "integer_motion2_force_0", &score, 0) == 0);
    mu_assert("force-zero score must be zero", score == 0.0);
    return nullptr;
}

} // namespace

mu_message_t run_tests()
{
    mu_run_test(test_public_option_registration);
    return nullptr;
}
