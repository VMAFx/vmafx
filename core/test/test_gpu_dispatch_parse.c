/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Tests for the shared VMAF_<BACKEND>_DISPATCH env-variable parser in
 *  core/src/gpu_dispatch_parse.h.
 *
 *  The parser is header-only (static inline) and used by every GPU backend
 *  to honour per-feature strategy overrides expressed via environment
 *  variables.  Historically the strategy-name match used strncmp() without
 *  a token-boundary check, which silently routed e.g.
 *  VMAF_CUDA_DISPATCH=feature:directx to the valid "direct" strategy.
 *  This file pins down the strict-match contract so the bug cannot
 *  regress: a prefix-but-not-whole-token match is treated as no match.
 */
#include "test.h"

#include "gpu_dispatch_parse.h"

static const char *const STRATEGY_NAMES[] = {"fastest", "direct", "balanced", NULL};

enum { STRAT_FASTEST = 0, STRAT_DIRECT = 1, STRAT_BALANCED = 2 };

static char *test_direct_matches_direct(void)
{
    int idx = -1;
    int rc = vmaf_gpu_dispatch_parse_env("feature:direct", "feature", STRATEGY_NAMES, &idx);
    mu_assert("'direct' should match 'direct'", rc == 1);
    mu_assert("'direct' should resolve to STRAT_DIRECT", idx == STRAT_DIRECT);
    return NULL;
}

static char *test_directx_does_not_match_direct(void)
{
    int idx = -1;
    int rc = vmaf_gpu_dispatch_parse_env("feature:directx", "feature", STRATEGY_NAMES, &idx);
    /* HIGH-SEVERITY bug guard: pre-fix, strncmp() would silently match the
     * "direct" strategy here because the first 6 bytes of "directx" are
     * "direct".  After the fix, the trailing 'x' fails the token-boundary
     * check, so the parser must report no match. */
    mu_assert("'directx' must NOT match 'direct' (no token boundary)", rc == 0);
    mu_assert("idx must remain unchanged on no-match", idx == -1);
    return NULL;
}

static char *test_direct_comma_separator_matches(void)
{
    int idx = -1;
    int rc = vmaf_gpu_dispatch_parse_env("feature:direct,other:balanced", "feature", STRATEGY_NAMES,
                                         &idx);
    mu_assert("'direct,…' should match 'direct'", rc == 1);
    mu_assert("'direct,…' should resolve to STRAT_DIRECT", idx == STRAT_DIRECT);
    return NULL;
}

static char *test_direct_newline_terminator_matches(void)
{
    int idx = -1;
    int rc = vmaf_gpu_dispatch_parse_env("feature:direct\n", "feature", STRATEGY_NAMES, &idx);
    mu_assert("'direct\\n' should match 'direct' (newline is a valid terminator)", rc == 1);
    mu_assert("'direct\\n' should resolve to STRAT_DIRECT", idx == STRAT_DIRECT);
    return NULL;
}

static char *test_fastest_matches_fastest(void)
{
    int idx = -1;
    int rc = vmaf_gpu_dispatch_parse_env("feature:fastest", "feature", STRATEGY_NAMES, &idx);
    mu_assert("'fastest' should match 'fastest'", rc == 1);
    mu_assert("'fastest' should resolve to STRAT_FASTEST", idx == STRAT_FASTEST);
    return NULL;
}

static char *test_balanced_prefix_does_not_match_balanced(void)
{
    /* Symmetric regression: longest-strategy-name prefix collision. */
    int idx = -1;
    int rc = vmaf_gpu_dispatch_parse_env("feature:balancedx", "feature", STRATEGY_NAMES, &idx);
    mu_assert("'balancedx' must NOT match 'balanced'", rc == 0);
    mu_assert("idx must remain unchanged on no-match", idx == -1);
    return NULL;
}

static char *test_second_token_directx_does_not_match(void)
{
    /* Mixed tokens: a leading valid token followed by a typo that historically
     * silently matched. */
    int idx = -1;
    int rc = vmaf_gpu_dispatch_parse_env("other:fastest,feature:directx", "feature", STRATEGY_NAMES,
                                         &idx);
    mu_assert("typo in second token must NOT match", rc == 0);
    mu_assert("idx must remain unchanged on no-match", idx == -1);
    return NULL;
}

static char *test_unrelated_feature_not_matched(void)
{
    int idx = -1;
    int rc = vmaf_gpu_dispatch_parse_env("other:direct", "feature", STRATEGY_NAMES, &idx);
    mu_assert("unrelated feature must not match", rc == 0);
    mu_assert("idx must remain unchanged on no-match", idx == -1);
    return NULL;
}

static char *test_null_inputs_safe(void)
{
    int idx = -1;
    mu_assert("NULL env_value returns 0",
              vmaf_gpu_dispatch_parse_env(NULL, "feature", STRATEGY_NAMES, &idx) == 0);
    mu_assert("NULL feature_name returns 0",
              vmaf_gpu_dispatch_parse_env("feature:direct", NULL, STRATEGY_NAMES, &idx) == 0);
    mu_assert("NULL strategy_names returns 0",
              vmaf_gpu_dispatch_parse_env("feature:direct", "feature", NULL, &idx) == 0);
    mu_assert("NULL out_strategy_idx returns 0",
              vmaf_gpu_dispatch_parse_env("feature:direct", "feature", STRATEGY_NAMES, NULL) == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_direct_matches_direct);
    mu_run_test(test_directx_does_not_match_direct);
    mu_run_test(test_direct_comma_separator_matches);
    mu_run_test(test_direct_newline_terminator_matches);
    mu_run_test(test_fastest_matches_fastest);
    mu_run_test(test_balanced_prefix_does_not_match_balanced);
    mu_run_test(test_second_token_directx_does_not_match);
    mu_run_test(test_unrelated_feature_not_matched);
    mu_run_test(test_null_inputs_safe);
    return NULL;
}
