/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  ADR-2056: the debug ratio of float_adm is filed under the unsuffixed key
 *  `adm`, which the Netflix golden tests read under every option set. Two
 *  instances with debug=true therefore write one key twice; the second is
 *  refused when it is registered, with a message naming the key, instead of
 *  failing the run at the first frame ("cannot be overwritten").
 *
 *  Positive: one debug instance, and any number without debug, register.
 *  Negative: a second debug instance (other options or the same) is refused
 *  with -EINVAL. Boundary: an identical second instance without debug is
 *  skipped as before, and the refusal leaves the first instance usable.
 */

#include <errno.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit, ADR-1138. */

static VmafFeatureDictionary *opts(const char *debug, const char *egl)
{
    VmafFeatureDictionary *d = NULL;
    if (debug && vmaf_feature_dictionary_set(&d, "debug", debug))
        return NULL;
    if (egl && vmaf_feature_dictionary_set(&d, "adm_enhn_gain_limit", egl))
        return NULL;
    return d;
}

static int use(VmafContext *vmaf, const char *debug, const char *egl)
{
    return vmaf_use_feature(vmaf, "float_adm", opts(debug, egl));
}

static char *with_context(char *(*body)(VmafContext *))
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    char *msg = body(vmaf);
    mu_assert("vmaf_close failed", vmaf_close(vmaf) == 0);
    return msg;
}

static char *one_debug_instance_registers(VmafContext *vmaf)
{
    mu_assert("a first debug instance must register", use(vmaf, "true", NULL) == 0);
    mu_assert("instances without debug register beside it",
              use(vmaf, NULL, "1.2") == 0 && use(vmaf, "false", "1.3") == 0);
    return NULL;
}

static char *second_debug_instance_is_refused(VmafContext *vmaf)
{
    mu_assert("a first debug instance must register", use(vmaf, "true", NULL) == 0);
    mu_assert("a second debug instance with other options must be refused",
              use(vmaf, "true", "1.2") == -EINVAL);
    mu_assert("a third one is refused too", use(vmaf, "true", "1.3") == -EINVAL);
    return NULL;
}

static char *debug_after_a_non_debug_instance_registers(VmafContext *vmaf)
{
    mu_assert("an instance without debug registers", use(vmaf, NULL, "1.2") == 0);
    mu_assert("a debug instance after it registers", use(vmaf, "true", "1.3") == 0);
    mu_assert("a second debug instance is refused", use(vmaf, "true", "1.4") == -EINVAL);
    return NULL;
}

static char *identical_second_instance_is_skipped_not_refused(VmafContext *vmaf)
{
    mu_assert("a first debug instance must register", use(vmaf, "true", NULL) == 0);
    /* Same options: its outputs are already covered, so it is skipped silently. */
    mu_assert("an identical second instance is skipped, not refused", use(vmaf, "true", NULL) == 0);
    return NULL;
}

static char *test_one_debug_instance_registers(void)
{
    return with_context(one_debug_instance_registers);
}
static char *test_second_debug_instance_is_refused(void)
{
    return with_context(second_debug_instance_is_refused);
}
static char *test_debug_after_a_non_debug_instance(void)
{
    return with_context(debug_after_a_non_debug_instance_registers);
}
static char *test_identical_second_instance(void)
{
    return with_context(identical_second_instance_is_skipped_not_refused);
}

char *run_tests(void)
{
    mu_run_test(test_one_debug_instance_registers);
    mu_run_test(test_second_debug_instance_is_refused);
    mu_run_test(test_debug_after_a_non_debug_instance);
    mu_run_test(test_identical_second_instance);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
