/**
 *
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* Device-free regression gate for BUG-048 section E.
 *
 * The feature extractors below allocate USM during init.  libvmaf does not
 * call close after a failed init, so every failure branch must release the
 * resources already acquired by that init invocation.  GNU link wrapping
 * replaces the SYCL allocator, dictionary builder, shared-frame setup, and
 * graph registry with deterministic host-only fakes.  The test therefore
 * exercises the production init/close functions without a SYCL device.
 */

#include "dict.h"
#include "feature/feature_extractor.h"
#include "feature/feature_name.h"
#include "log.h"
#include "sycl/common.h"
#include "test.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{

constexpr unsigned LEDGER_CAPACITY = 256U;

struct Allocation {
    void *pointer;
    bool freed;
};

Allocation allocations[LEDGER_CAPACITY]{};
unsigned allocations_used;
unsigned allocation_calls;
unsigned fail_allocation_at;
bool fail_dictionary;
bool dictionary_live;
bool fail_graph_registration;
unsigned graph_registration_calls;
unsigned graph_unregistration_calls;
char fake_state_storage;
char fake_dictionary_storage;

void reset_faults()
{
    std::memset(allocations, 0, sizeof(allocations));
    allocations_used = 0U;
    allocation_calls = 0U;
    fail_allocation_at = 0U;
    fail_dictionary = false;
    dictionary_live = false;
    fail_graph_registration = false;
    graph_registration_calls = 0U;
    graph_unregistration_calls = 0U;
}

} // namespace

namespace
{

void *allocate_tracked()
{
    allocation_calls++;
    if (allocation_calls == fail_allocation_at)
        return nullptr;
    if (allocations_used >= LEDGER_CAPACITY)
        return nullptr;
    void *pointer = std::malloc(1U);
    if (!pointer)
        return nullptr;
    allocations[allocations_used++] = {.pointer = pointer, .freed = false};
    return pointer;
}

void release_tracked(void *pointer)
{
    for (unsigned i = 0U; i < allocations_used; i++) {
        if (allocations[i].pointer == pointer) {
            allocations[i].freed = true;
            std::free(pointer);
            return;
        }
    }
}

unsigned outstanding_allocations()
{
    unsigned outstanding = 0U;
    for (unsigned i = 0U; i < allocations_used; i++) {
        if (!allocations[i].freed)
            outstanding++;
    }
    return outstanding;
}

void discard_outstanding_allocations()
{
    for (unsigned i = 0U; i < allocations_used; i++) {
        if (!allocations[i].freed) {
            std::free(allocations[i].pointer);
            allocations[i].freed = true;
        }
    }
}

} // namespace

/* These names are fixed by GNU ld's --wrap ABI. */
#define VMAF_WRAP_EXPORT __attribute__((visibility("default")))

// cppcheck-suppress unusedFunction
// ADR-0141 load-bearing linker ABI; see the BUG-048 section-E research digest.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
extern "C" VMAF_WRAP_EXPORT void *__wrap_vmaf_sycl_malloc_device(VmafSyclState *, size_t)
{
    return allocate_tracked();
}

// cppcheck-suppress unusedFunction
extern "C" VMAF_WRAP_EXPORT void *__wrap_vmaf_sycl_malloc_host(VmafSyclState *, size_t)
{
    return allocate_tracked();
}

// cppcheck-suppress unusedFunction
extern "C" VMAF_WRAP_EXPORT void __wrap_vmaf_sycl_free(VmafSyclState *, void *pointer)
{
    if (pointer)
        release_tracked(pointer);
}

// cppcheck-suppress unusedFunction
extern "C" VMAF_WRAP_EXPORT VmafDictionary *
__wrap_vmaf_feature_name_dict_from_provided_features(const char **, const VmafOption *,
                                                     const void *)
{
    if (fail_dictionary)
        return nullptr;
    dictionary_live = true;
    return reinterpret_cast<VmafDictionary *>(&fake_dictionary_storage);
}

// cppcheck-suppress unusedFunction
extern "C" VMAF_WRAP_EXPORT int __wrap_vmaf_dictionary_free(VmafDictionary **dictionary)
{
    if (!dictionary || *dictionary != reinterpret_cast<VmafDictionary *>(&fake_dictionary_storage))
        return -EINVAL;
    dictionary_live = false;
    *dictionary = nullptr;
    return 0;
}

// cppcheck-suppress unusedFunction
extern "C" VMAF_WRAP_EXPORT int __wrap_vmaf_sycl_shared_frame_init(VmafSyclState *, unsigned,
                                                                   unsigned, unsigned)
{
    return 0;
}

// cppcheck-suppress unusedFunction
extern "C" VMAF_WRAP_EXPORT int
__wrap_vmaf_sycl_graph_register(VmafSyclState *, VmafSyclGraphEnqueueFn, VmafSyclGraphPreFn,
                                VmafSyclGraphPostFn, VmafSyclGraphConfigFn, void *, const char *)
{
    graph_registration_calls++;
    return fail_graph_registration ? -EIO : 0;
}

// cppcheck-suppress unusedFunction
extern "C" VMAF_WRAP_EXPORT int __wrap_vmaf_sycl_graph_unregister(VmafSyclState *, void *)
{
    graph_unregistration_calls++;
    return 0;
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

namespace
{

extern "C" VmafFeatureExtractor vmaf_fex_float_adm_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_float_motion_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_float_psnr_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_float_vif_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_ciede_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_float_moment_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_integer_motion_v2_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_float_ms_ssim_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_psnr_hvs_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_psnr_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_float_ssim_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_integer_ssim_sycl;
extern "C" VmafFeatureExtractor vmaf_fex_ssimulacra2_sycl;

struct InitCase {
    VmafFeatureExtractor *descriptor;
    bool has_dictionary;
    bool has_graph_registration;
};

const InitCase cases[] = {
    {.descriptor = &vmaf_fex_float_adm_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_float_motion_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_float_psnr_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_float_vif_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_ciede_sycl, .has_dictionary = true, .has_graph_registration = false},
    {.descriptor = &vmaf_fex_float_moment_sycl,
     .has_dictionary = true,
     .has_graph_registration = true},
    {.descriptor = &vmaf_fex_integer_motion_v2_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_float_ms_ssim_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_psnr_hvs_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_psnr_sycl, .has_dictionary = true, .has_graph_registration = true},
    {.descriptor = &vmaf_fex_float_ssim_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_integer_ssim_sycl,
     .has_dictionary = true,
     .has_graph_registration = false},
    {.descriptor = &vmaf_fex_ssimulacra2_sycl,
     .has_dictionary = false,
     .has_graph_registration = false},
};

} // namespace

namespace
{

void apply_option_defaults(void *priv, const VmafOption *options)
{
    for (unsigned i = 0U; options && options[i].name; i++) {
        void *destination = static_cast<void *>(static_cast<char *>(priv) + options[i].offset);
        switch (options[i].type) {
        case VMAF_OPT_TYPE_BOOL:
            *static_cast<bool *>(destination) = options[i].default_val.b;
            break;
        case VMAF_OPT_TYPE_INT:
            *static_cast<int *>(destination) = options[i].default_val.i;
            break;
        case VMAF_OPT_TYPE_DOUBLE:
            *static_cast<double *>(destination) = options[i].default_val.d;
            break;
        case VMAF_OPT_TYPE_STRING:
            break;
        }
    }
}

struct InitResult {
    int error;
    unsigned allocations;
    unsigned outstanding;
    bool leaked_dictionary;
    unsigned graph_registrations;
    unsigned graph_unregistrations;
};

} // namespace

namespace
{

InitResult invoke_init(const InitCase &test_case)
{
    VmafFeatureExtractor extractor = *test_case.descriptor;
    extractor.priv = std::calloc(1U, extractor.priv_size);
    if (!extractor.priv) {
        return {.error = -ENOMEM,
                .allocations = 0U,
                .outstanding = 0U,
                .leaked_dictionary = false,
                .graph_registrations = 0U,
                .graph_unregistrations = 0U};
    }
    apply_option_defaults(extractor.priv, extractor.options);
    extractor.sycl_state = reinterpret_cast<VmafSyclState *>(&fake_state_storage);
    const int error = extractor.init(&extractor, VMAF_PIX_FMT_YUV420P, 8U, 256U, 256U);
    const InitResult result = {
        .error = error,
        .allocations = allocations_used,
        .outstanding = outstanding_allocations(),
        .leaked_dictionary = dictionary_live,
        .graph_registrations = graph_registration_calls,
        .graph_unregistrations = graph_unregistration_calls,
    };
    discard_outstanding_allocations();
    std::free(extractor.priv);
    return result;
}

} // namespace

namespace
{

mu_message_t validate_result(const InitCase &test_case, const char *fault, const InitResult &result,
                             int expected_error)
{
    if (result.error != expected_error || result.allocations == 0U || result.outstanding != 0U ||
        result.leaked_dictionary) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "%s/%s: rc=%d expected=%d allocations=%u outstanding=%u dict_live=%d "
                 "graph=%u/%u\n",
                 test_case.descriptor->name, fault, result.error, expected_error,
                 result.allocations, result.outstanding, result.leaked_dictionary,
                 result.graph_registrations, result.graph_unregistrations);
        return "failed SYCL init must release every resource acquired by that init";
    }
    return nullptr;
}

mu_message_t run_allocation_fault(const InitCase &test_case)
{
    reset_faults();
    fail_allocation_at = 2U;
    return validate_result(test_case, "second-allocation", invoke_init(test_case), -ENOMEM);
}

mu_message_t run_dictionary_fault(const InitCase &test_case)
{
    reset_faults();
    fail_dictionary = true;
    return validate_result(test_case, "dictionary", invoke_init(test_case), -ENOMEM);
}

mu_message_t run_graph_fault(const InitCase &test_case)
{
    reset_faults();
    fail_graph_registration = true;
    const InitResult result = invoke_init(test_case);
    mu_assert("graph failure injection did not reach graph registration",
              result.graph_registrations == 1U);
    mu_assert("failed graph registration must be balanced by unregister",
              result.graph_unregistrations == 1U);
    return validate_result(test_case, "graph-registration", result, -EIO);
}

} // namespace

namespace
{

mu_message_t test_sycl_init_failures_unwind()
{
    for (const InitCase &test_case : cases) {
        mu_assert_msg(run_allocation_fault(test_case));
        if (test_case.has_dictionary)
            mu_assert_msg(run_dictionary_fault(test_case));
        if (test_case.has_graph_registration)
            mu_assert_msg(run_graph_fault(test_case));
    }
    return nullptr;
}

} // namespace

extern "C" mu_message_t run_tests()
{
    mu_run_test(test_sycl_init_failures_unwind);
    return nullptr;
}
