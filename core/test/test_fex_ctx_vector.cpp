/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <atomic>
#include "fex_ctx_vector.h"
#include "fex_ctx_vector_internal.h"
#include "feature/feature_name.h"
#include "test.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#ifdef FEX_VECTOR_ALLOC_TEST
namespace
{
unsigned name_calls;
unsigned fail_name_call;
const void *fail_grow_pointer;
} // namespace

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ADR-0723; Research-2047: GNU link wrapping ABI.
extern "C" char *__real_vmaf_feature_name_from_options(const char *, const VmafOption *,
                                                       const void *);
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ADR-0723; Research-2047: GNU link wrapping ABI.
extern "C" void *__real_realloc(void *, size_t);

// cppcheck-suppress unusedFunction
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage) — ADR-0723; Research-2047: GNU linker calls this entry point.
extern "C" char *__wrap_vmaf_feature_name_from_options(const char *name, const VmafOption *opts,
                                                       const void *obj)
{
    if (fail_name_call && ++name_calls == fail_name_call)
        return nullptr;
    return __real_vmaf_feature_name_from_options(name, opts, obj);
}

// cppcheck-suppress unusedFunction
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage) — ADR-0723; Research-2047: GNU linker calls this entry point.
extern "C" void *__wrap_realloc(void *ptr, size_t bytes)
{
    if (fail_grow_pointer && ptr == fail_grow_pointer) {
        fail_grow_pointer = nullptr;
        return nullptr;
    }
    return __real_realloc(ptr, bytes);
}
#endif

namespace
{

struct ContextDeleter {
    void operator()(VmafFeatureExtractorContext *ctx) const noexcept
    {
        (void)vmaf_feature_extractor_context_destroy(ctx);
    }
};
using Context = std::unique_ptr<VmafFeatureExtractorContext, ContextDeleter>;

class Vector
{
  public:
    ~Vector()
    {
        feature_extractor_vector_destroy(&entries_);
    }
    RegisteredFeatureExtractors &get() noexcept
    {
        return entries_;
    }

  private:
    RegisteredFeatureExtractors entries_{};
};

Context make_context(const VmafFeatureExtractor *fex, const char *key, const char *value)
{
    VmafDictionary *options = nullptr;
    if (key && vmaf_dictionary_set(&options, key, value, 0) != 0) {
        (void)vmaf_dictionary_free(&options);
        return {};
    }
    VmafFeatureExtractorContext *ctx = nullptr;
    if (vmaf_feature_extractor_context_create(&ctx, fex, options) != 0) {
        (void)vmaf_dictionary_free(&options);
        return {};
    }
    return Context(ctx);
}

int append_context(RegisteredFeatureExtractors *entries, Context ctx)
{
    if (!ctx)
        return -ENOMEM;
    auto *const raw = ctx.release();
    const int err = feature_extractor_vector_append(entries, raw, 0);
    if (err)
        ctx.reset(raw);
    return err;
}

mu_message_t check_motion_pair(const char *first, const char *second, unsigned count,
                               bool twin = false, bool legacy = false,
                               const char *second_key = "motion_force_zero")
{
    const VmafFeatureExtractor *motion = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", motion);
    const VmafFeatureExtractor a = *motion;
    VmafFeatureExtractor b = *motion;
    if (twin)
        b.name = "motion_mock_gpu";
    if (legacy)
        b.provided_features = nullptr;
    Vector vector;
    mu_assert("vector init failed", feature_extractor_vector_init(&vector.get()) == 0);
    auto first_ctx = make_context(&a, first ? "motion_force_zero" : nullptr, first);
    auto *const first_ptr = first_ctx.get();
    mu_assert("first append failed", append_context(&vector.get(), std::move(first_ctx)) == 0);
    mu_assert("second append failed",
              append_context(&vector.get(), make_context(&b, second_key, second)) == 0);
    mu_assert("option identity produced the wrong count", vector.get().cnt == count);
    mu_assert("first registration was not preserved", vector.get().fex_ctx[0] == first_ptr);
    return nullptr;
}

mu_message_t test_distinct_motion_options()
{
    return check_motion_pair("false", "true", 2);
}
mu_message_t test_explicit_default()
{
    return check_motion_pair(nullptr, "false", 1);
}
mu_message_t test_option_alias()
{
    return check_motion_pair("true", "true", 1, false, false, "force_0");
}
mu_message_t test_identical_custom_options()
{
    return check_motion_pair("true", "true", 1);
}
mu_message_t test_backend_twin()
{
    return check_motion_pair(nullptr, "false", 1, true);
}
mu_message_t test_distinct_backend_twin()
{
    return check_motion_pair("false", "true", 2, true);
}
mu_message_t test_legacy_default()
{
    return check_motion_pair(nullptr, "false", 1, false, true);
}
mu_message_t test_legacy_distinct()
{
    return check_motion_pair("false", "true", 2, false, true);
}
mu_message_t test_legacy_different_name()
{
    return check_motion_pair(nullptr, "false", 2, true, true);
}

mu_message_t test_capacity_arithmetic()
{
    mu_assert("zero capacity must not grow", vmaf_next_fex_capacity(0) == 0);
    mu_assert("initial capacity must double", vmaf_next_fex_capacity(8) == 16);
    mu_assert("unsigned overflow must fail", vmaf_next_fex_capacity(UINT_MAX / 2u + 1u) == 0);
    constexpr size_t byte_limit = (SIZE_MAX / sizeof(VmafFeatureExtractorContext *)) / 2u;
    constexpr unsigned limit = byte_limit < UINT_MAX / 2u ? byte_limit : UINT_MAX / 2u;
    mu_assert("last representable doubling must succeed",
              vmaf_next_fex_capacity(limit) == limit * 2u);
    mu_assert("first unrepresentable doubling must fail", vmaf_next_fex_capacity(limit + 1u) == 0);
    return nullptr;
}

int append_motion_variant(RegisteredFeatureExtractors *entries, unsigned value)
{
    char option[16];
    if (snprintf(option, sizeof(option), "%u", value) < 0)
        return -EINVAL;
    const auto *motion = vmaf_get_feature_extractor_by_name("motion");
    if (!motion)
        return -EINVAL;
    return append_context(entries, make_context(motion, "motion_blend_offset", option));
}

bool unused_slots_clear(const RegisteredFeatureExtractors *entries)
{
    for (unsigned i = entries->cnt; i < entries->capacity; i++) {
        if (entries->fex_ctx[i])
            return false;
    }
    return true;
}

bool vector_unchanged(const RegisteredFeatureExtractors *entries, const void *storage,
                      unsigned count, unsigned capacity)
{
    return entries->cnt == count && entries->capacity == capacity &&
           static_cast<const void *>(entries->fex_ctx) == storage;
}

mu_message_t test_native_growth()
{
    Vector vector;
    mu_assert("vector init failed", feature_extractor_vector_init(&vector.get()) == 0);
    mu_assert("first append failed", append_motion_variant(&vector.get(), 0) == 0);
    const auto *const first = vector.get().fex_ctx[0];
    for (unsigned i = 1; i < 17; i++)
        mu_assert("distinct append failed", append_motion_variant(&vector.get(), i) == 0);
    mu_assert("seventeen option variants must survive", vector.get().cnt == 17);
    mu_assert("growth must retain doubling", vector.get().capacity == 32);
    mu_assert("growth must preserve order", vector.get().fex_ctx[0] == first);
    mu_assert("unused slots must be null", unused_slots_clear(&vector.get()));
    return nullptr;
}

#ifdef FEX_VECTOR_ALLOC_TEST
mu_message_t check_retry(RegisteredFeatureExtractors *entries, Context incoming, unsigned count,
                         unsigned capacity)
{
    mu_assert("incoming ownership must permit retry",
              append_context(entries, std::move(incoming)) == 0);
    mu_assert("retry must append with expected capacity",
              entries->cnt == count && entries->capacity == capacity);
    return nullptr;
}

mu_message_t check_name_allocation_failure(unsigned failure)
{
    const auto *motion = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", motion);
    Vector vector;
    mu_assert("vector init failed", feature_extractor_vector_init(&vector.get()) == 0);
    mu_assert("first append failed",
              append_context(&vector.get(), make_context(motion, nullptr, nullptr)) == 0);
    auto incoming = make_context(motion, "motion_force_zero", "true");
    mu_assert("incoming context creation failed", incoming);
    const void *const storage = static_cast<const void *>(vector.get().fex_ctx);
    name_calls = 0;
    fail_name_call = failure;
    const int err = feature_extractor_vector_append(&vector.get(), incoming.get(), 0);
    fail_name_call = 0;
    mu_assert("name allocation failure must return ENOMEM", err == -ENOMEM);
    mu_assert("name failure must preserve vector", vector_unchanged(&vector.get(), storage, 1, 8));
    return check_retry(&vector.get(), std::move(incoming), 2, 8);
}

mu_message_t test_first_name_allocation_failure()
{
    return check_name_allocation_failure(1);
}
mu_message_t test_second_name_allocation_failure()
{
    return check_name_allocation_failure(2);
}

mu_message_t test_growth_allocation_failure()
{
    Vector vector;
    mu_assert("vector init failed", feature_extractor_vector_init(&vector.get()) == 0);
    for (unsigned i = 0; i < 8; i++)
        mu_assert("initial append failed", append_motion_variant(&vector.get(), i) == 0);
    const auto *motion = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", motion);
    auto incoming = make_context(motion, "motion_blend_offset", "8");
    mu_assert("incoming context creation failed", incoming);
    const void *const storage = static_cast<const void *>(vector.get().fex_ctx);
    fail_grow_pointer = storage;
    const int err = feature_extractor_vector_append(&vector.get(), incoming.get(), 0);
    const bool intercepted = fail_grow_pointer == nullptr;
    fail_grow_pointer = nullptr;
    mu_assert("growth allocation must be intercepted", intercepted);
    mu_assert("growth failure must return ENOMEM", err == -ENOMEM);
    mu_assert("growth failure must preserve vector",
              vector_unchanged(&vector.get(), storage, 8, 8));
    return check_retry(&vector.get(), std::move(incoming), 9, 16);
}
#endif

mu_message_t run_identity_tests()
{
    mu_run_test(test_distinct_motion_options);
    mu_run_test(test_explicit_default);
    mu_run_test(test_option_alias);
    mu_run_test(test_identical_custom_options);
    mu_run_test(test_backend_twin);
    mu_run_test(test_distinct_backend_twin);
    return nullptr;
}

mu_message_t run_legacy_and_growth_tests()
{
    mu_run_test(test_legacy_default);
    mu_run_test(test_legacy_distinct);
    mu_run_test(test_legacy_different_name);
    mu_run_test(test_capacity_arithmetic);
    mu_run_test(test_native_growth);
    return nullptr;
}

} // namespace

mu_message_t run_tests()
{
    mu_message_t err = run_identity_tests();
    if (err)
        return err;
    err = run_legacy_and_growth_tests();
    if (err)
        return err;
#ifdef FEX_VECTOR_ALLOC_TEST
    mu_run_test(test_first_name_allocation_failure);
    mu_run_test(test_second_name_allocation_failure);
    mu_run_test(test_growth_allocation_failure);
#endif
    return nullptr;
}
