/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Exhaustive branch coverage for libvmaf/src/opt.c. The TU has four
 *  static type-specific helpers (bool / int / double / string) and a
 *  single public dispatch `vmaf_option_set`. Each helper has the same
 *  shape — NULL `val` returns the default, parse-fail returns -EINVAL,
 *  range-fail returns -EINVAL, ERANGE on strtol/strtod returns -EINVAL.
 *  We hit every branch in every helper plus the dispatch's NULL guards
 *  and the unknown-type default.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>

#include "test.h"

#include "opt.h"

struct cfg {
    bool b;
    int i;
    double d;
    char *s;
};

static char *test_dispatch_null_obj(void)
{
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_BOOL,
        .offset = offsetof(struct cfg, b),
        .default_val.b = false,
    };
    mu_assert("NULL obj must return -EINVAL", vmaf_option_set(&opt, NULL, "true") == -EINVAL);
    return NULL;
}

static char *test_dispatch_null_opt(void)
{
    struct cfg c = {0};
    mu_assert("NULL opt must return -EINVAL", vmaf_option_set(NULL, &c, "true") == -EINVAL);
    return NULL;
}

static char *test_dispatch_unknown_type(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = (enum VmafOptionType)9999,
        .offset = offsetof(struct cfg, i),
        .default_val.i = 0,
    };
    mu_assert("unknown type must return -EINVAL", vmaf_option_set(&opt, &c, "1") == -EINVAL);
    return NULL;
}

static char *test_bool_default_when_null_val(void)
{
    struct cfg c = {.b = false};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_BOOL,
        .offset = offsetof(struct cfg, b),
        .default_val.b = true,
    };
    mu_assert("rc=0 when val NULL", vmaf_option_set(&opt, &c, NULL) == 0);
    mu_assert("default applied when val NULL", c.b == true);
    return NULL;
}

static char *test_bool_true_string(void)
{
    struct cfg c = {.b = false};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_BOOL,
        .offset = offsetof(struct cfg, b),
        .default_val.b = false,
    };
    mu_assert("rc=0 for 'true'", vmaf_option_set(&opt, &c, "true") == 0);
    mu_assert("value=true", c.b == true);
    return NULL;
}

static char *test_bool_false_string(void)
{
    struct cfg c = {.b = true};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_BOOL,
        .offset = offsetof(struct cfg, b),
        .default_val.b = true,
    };
    mu_assert("rc=0 for 'false'", vmaf_option_set(&opt, &c, "false") == 0);
    mu_assert("value=false", c.b == false);
    return NULL;
}

static char *test_bool_invalid_string(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_BOOL,
        .offset = offsetof(struct cfg, b),
        .default_val.b = false,
    };
    mu_assert("invalid bool returns -EINVAL", vmaf_option_set(&opt, &c, "yes") == -EINVAL);
    return NULL;
}

static char *test_int_default_when_null_val(void)
{
    struct cfg c = {.i = -1};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_INT,
        .offset = offsetof(struct cfg, i),
        .default_val.i = 42,
        .min = -100,
        .max = 100,
    };
    mu_assert("rc=0 when val NULL", vmaf_option_set(&opt, &c, NULL) == 0);
    mu_assert("int default applied", c.i == 42);
    return NULL;
}

static char *test_int_valid_in_range(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_INT,
        .offset = offsetof(struct cfg, i),
        .default_val.i = 0,
        .min = 0,
        .max = 100,
    };
    mu_assert("rc=0 for '50'", vmaf_option_set(&opt, &c, "50") == 0);
    mu_assert("parsed value", c.i == 50);
    return NULL;
}

static char *test_int_below_min(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_INT,
        .offset = offsetof(struct cfg, i),
        .default_val.i = 0,
        .min = 10,
        .max = 100,
    };
    mu_assert("below min returns -EINVAL", vmaf_option_set(&opt, &c, "5") == -EINVAL);
    return NULL;
}

static char *test_int_above_max(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_INT,
        .offset = offsetof(struct cfg, i),
        .default_val.i = 0,
        .min = 0,
        .max = 10,
    };
    mu_assert("above max returns -EINVAL", vmaf_option_set(&opt, &c, "11") == -EINVAL);
    return NULL;
}

static char *test_int_unparseable(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_INT,
        .offset = offsetof(struct cfg, i),
        .default_val.i = 0,
        .min = 0,
        .max = 100,
    };
    mu_assert("non-numeric returns -EINVAL", vmaf_option_set(&opt, &c, "abc") == -EINVAL);
    return NULL;
}

static char *test_int_trailing_garbage(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_INT,
        .offset = offsetof(struct cfg, i),
        .default_val.i = 0,
        .min = 0,
        .max = 100,
    };
    mu_assert("trailing garbage returns -EINVAL", vmaf_option_set(&opt, &c, "12abc") == -EINVAL);
    return NULL;
}

static char *test_int_overflow(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_INT,
        .offset = offsetof(struct cfg, i),
        .default_val.i = 0,
        .min = 0,
        .max = 0x7fffffff,
    };
    /* strtol on "999999999999999999999" sets errno=ERANGE. */
    mu_assert("ERANGE returns -EINVAL",
              vmaf_option_set(&opt, &c, "999999999999999999999") == -EINVAL);
    return NULL;
}

static char *test_double_default_when_null_val(void)
{
    struct cfg c = {.d = 0.0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 1.5,
        .min = 0.0,
        .max = 10.0,
    };
    mu_assert("rc=0 when val NULL", vmaf_option_set(&opt, &c, NULL) == 0);
    mu_assert("double default applied", c.d == 1.5);
    return NULL;
}

static char *test_double_valid_in_range(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 0.0,
        .min = 0.0,
        .max = 10.0,
    };
    mu_assert("rc=0 for '3.14'", vmaf_option_set(&opt, &c, "3.14") == 0);
    mu_assert("parsed value approx 3.14", fabs(c.d - 3.14) < 1e-9);
    return NULL;
}

static char *test_double_below_min(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 0.0,
        .min = 1.0,
        .max = 10.0,
    };
    mu_assert("below min returns -EINVAL", vmaf_option_set(&opt, &c, "0.5") == -EINVAL);
    return NULL;
}

static char *test_double_above_max(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 0.0,
        .min = 0.0,
        .max = 1.0,
    };
    mu_assert("above max returns -EINVAL", vmaf_option_set(&opt, &c, "1.5") == -EINVAL);
    return NULL;
}

static char *test_double_unparseable(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 0.0,
        .min = -1e9,
        .max = 1e9,
    };
    mu_assert("non-numeric returns -EINVAL", vmaf_option_set(&opt, &c, "xyz") == -EINVAL);
    return NULL;
}

static char *test_double_trailing_garbage(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 0.0,
        .min = -1e9,
        .max = 1e9,
    };
    mu_assert("trailing garbage returns -EINVAL", vmaf_option_set(&opt, &c, "1.5x") == -EINVAL);
    return NULL;
}

static char *test_double_overflow(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 0.0,
        .min = -HUGE_VAL,
        .max = HUGE_VAL,
    };
    /* strtod on "1e500" sets errno=ERANGE. */
    mu_assert("ERANGE returns -EINVAL", vmaf_option_set(&opt, &c, "1e500") == -EINVAL);
    return NULL;
}

/* T-ROUND8-OPT-NAN-BYPASS: strtod("nan") returns NaN whose ordered
 * comparisons with any finite bound both return false, so the old code
 * silently accepted it.  The new code rejects it before the bounds check.
 * Verified: strtod("nan") ⟹ NaN, end='', so the parse-success path is
 * reached; only the explicit isnan() guard closes the hole. */
static char *test_double_nan_is_rejected(void)
{
    struct cfg c = {.d = 42.0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 42.0,
        .min = 0.0,
        .max = 1500.0,
    };
    mu_assert("NaN must return -EINVAL", vmaf_option_set(&opt, &c, "nan") == -EINVAL);
    /* The default must not have been overwritten. */
    mu_assert("dst must remain at default after NaN rejection", c.d == 42.0);
    mu_assert("NaN uppercase must return -EINVAL", vmaf_option_set(&opt, &c, "NaN") == -EINVAL);
    mu_assert("NAN uppercase must return -EINVAL", vmaf_option_set(&opt, &c, "NAN") == -EINVAL);
    return NULL;
}

/* Inf with a finite upper bound is already rejected by the `n > max` check
 * once max is finite; with an infinite max the caller explicitly opted in to
 * unbounded doubles, which is a legitimate use-case.  Verify both branches. */
static char *test_double_inf_rejected_when_max_finite(void)
{
    struct cfg c = {.d = 1.0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_DOUBLE,
        .offset = offsetof(struct cfg, d),
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 1500.0,
    };
    mu_assert("inf rejected when max is finite", vmaf_option_set(&opt, &c, "inf") == -EINVAL);
    mu_assert("-inf rejected when min is finite", vmaf_option_set(&opt, &c, "-inf") == -EINVAL);
    return NULL;
}

static char *test_string_default_when_null_val(void)
{
    struct cfg c = {.s = NULL};
    char *deflt = (char *)"hello";
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_STRING,
        .offset = offsetof(struct cfg, s),
        .default_val.s = deflt,
    };
    mu_assert("rc=0 when val NULL", vmaf_option_set(&opt, &c, NULL) == 0);
    mu_assert("string default applied", c.s == deflt);
    return NULL;
}

static char *test_string_assign(void)
{
    struct cfg c = {0};
    const VmafOption opt = {
        .name = "x",
        .type = VMAF_OPT_TYPE_STRING,
        .offset = offsetof(struct cfg, s),
        .default_val.s = NULL,
    };
    const char *val = "abc";
    mu_assert("rc=0 for non-NULL val", vmaf_option_set(&opt, &c, val) == 0);
    mu_assert("pointer captured", c.s == val);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_dispatch_null_obj);
    mu_run_test(test_dispatch_null_opt);
    mu_run_test(test_dispatch_unknown_type);
    mu_run_test(test_bool_default_when_null_val);
    mu_run_test(test_bool_true_string);
    mu_run_test(test_bool_false_string);
    mu_run_test(test_bool_invalid_string);
    mu_run_test(test_int_default_when_null_val);
    mu_run_test(test_int_valid_in_range);
    mu_run_test(test_int_below_min);
    mu_run_test(test_int_above_max);
    mu_run_test(test_int_unparseable);
    mu_run_test(test_int_trailing_garbage);
    mu_run_test(test_int_overflow);
    mu_run_test(test_double_default_when_null_val);
    mu_run_test(test_double_valid_in_range);
    mu_run_test(test_double_below_min);
    mu_run_test(test_double_above_max);
    mu_run_test(test_double_unparseable);
    mu_run_test(test_double_trailing_garbage);
    mu_run_test(test_double_overflow);
    mu_run_test(test_double_nan_is_rejected);
    mu_run_test(test_double_inf_rejected_when_max_finite);
    mu_run_test(test_string_default_when_null_val);
    mu_run_test(test_string_assign);
    return NULL;
}
