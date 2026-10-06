/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Conformance of the libvmaf compat library (ADR-1852 design section 2.11,
 * RC4 WP6): every scenario runs through the old libvmaf (the engine's own
 * bodies, vmaf_engine_<stem>) and through libvmaf.so.3 on the VMAFx API, and
 * the two traces must be equal: return values, outputs and every score as
 * %a. Every compat function this build has must be called through both
 * tables (compat_conformance_gen.h), so a compat function without a
 * conformance case fails here.
 *
 * Planted defects (meson tests registered should_fail): VMAF_COMPAT_PLANT=
 * score moves one score of the compat side by one ulp; =uncovered skips a
 * scenario, leaving functions without a case.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat_conformance_trace.h"
#include "gpu_dispatch_env.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define TRACE_LINE_MAX 4096

void trace(Trace *t, const char *fmt, ...)
{
    char line[TRACE_LINE_MAX];
    va_list args;
#if defined(__clang__) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    /* As in core/src/log.c: clang lowers the C23 va_start macro to
     * __builtin_c23_va_start, which its VAList analyzer does not model yet;
     * the traditional builtin initialises the list the same way. */
    __builtin_va_start(args, fmt);
#else
    va_start(args, fmt);
#endif
    const int n = vsnprintf(line, sizeof(line) - 1u, fmt, args);
    va_end(args);
    if (n < 0) {
        t->failed = 1;
        return;
    }
    const size_t add = strlen(line) + 1u;
    if (t->len + add + 1u > t->cap) {
        const size_t cap = (t->cap + add + 1u) * 2u;
        char *const grown = realloc(t->text, cap);
        if (!grown) {
            t->failed = 1;
            return;
        }
        t->text = grown;
        t->cap = cap;
    }
    memcpy(t->text + t->len, line, add - 1u);
    t->len += add;
    t->text[t->len - 1u] = '\n';
    t->text[t->len] = '\0';
}

const char *trace_str(const char *s)
{
    return s ? s : "(null)";
}

void trace_free(Trace *t)
{
    free(t->text);
    *t = (Trace){0};
}

/* ---- Planted defects -------------------------------------------------------- */

static int (*real_score_at_index)(VmafContext *, VmafModel *, double *, unsigned);

/* The compat side's vmaf_score_at_index(), one ulp off. */
static int planted_score_at_index(VmafContext *vmaf, VmafModel *model, double *score,
                                  unsigned index)
{
    const int err = real_score_at_index(vmaf, model, score, index);
    if (!err && score) {
        *score = nextafter(*score, INFINITY);
    }
    return err;
}

const char *conformance_plant(void)
{
    /* The once-only environment snapshot (ADR-0488), not a getenv of this thread. */
    const char *const value = vmaf_gpu_dispatch_env_get("VMAF_COMPAT_PLANT");
    return value ? value : "";
}

/* ---- Running and comparing --------------------------------------------------- */

static const struct {
    const char *name;
    Scenario run;
} scenarios[] = {
    {"lifecycle", scenario_lifecycle},
    {"dictionary", scenario_dictionary},
    {"models", scenario_models},
    {"collections", scenario_collections},
    {"scoring", scenario_scoring},
    {"preallocated", scenario_preallocated},
    {"pictures", scenario_pictures},
    {"conversion", scenario_conversion},
    {"perceptual", scenario_perceptual},
    {"tiny_ai", scenario_tiny_ai},
    {"absent_backends", scenario_absent_backends},
    {"mcp", scenario_mcp},
};

#define N_SCENARIOS (sizeof(scenarios) / sizeof(scenarios[0]))

/* The first line where the traces differ, as each side wrote it. */
static void show_difference(const char *name, const Trace *old_t, const Trace *new_t)
{
    const char *const a = old_t->text ? old_t->text : "";
    const char *const b = new_t->text ? new_t->text : "";
    size_t line = 1;
    size_t start = 0;
    for (size_t i = 0; a[i] && a[i] == b[i]; i++) {
        if (a[i] == '\n') {
            line++;
            start = i + 1u;
        }
    }
    (void)fprintf(stderr, "scenario %s: traces differ at line %zu%s\n", name, line,
                  old_t->failed || new_t->failed ? " (a trace ran out of memory)" : "");
    (void)fprintf(stderr, "  libvmaf : %.*s\n", (int)strcspn(a + start, "\n"), a + start);
    (void)fprintf(stderr, "  compat  : %.*s\n", (int)strcspn(b + start, "\n"), b + start);
}

/* Run one scenario on both sides: NULL when the traces are equal. */
static mu_message_t run_scenario(size_t which, const VmafCompatApi *new_api)
{
    Trace old_t = {0};
    Trace new_t = {0};
    scenarios[which].run(&vmaf_compat_old, &old_t);
    scenarios[which].run(new_api, &new_t);
    const int same = !old_t.failed && !new_t.failed && old_t.len == new_t.len &&
                     (old_t.len == 0 || memcmp(old_t.text, new_t.text, old_t.len) == 0);
    if (!same) {
        show_difference(scenarios[which].name, &old_t, &new_t);
    }
    (void)fprintf(stderr, "scenario %s: %zu trace bytes, %s\n", scenarios[which].name, old_t.len,
                  same ? "equal" : "DIFFERENT");
    trace_free(&old_t);
    trace_free(&new_t);
    return same ? NULL : "the compat library and libvmaf disagree (see the lines above)";
}

/* Every compat function this build has was called through both tables. */
static mu_message_t check_coverage(void)
{
    unsigned uncovered = 0;
    for (size_t i = 0; i < VMAF_COMPAT_COUNT; i++) {
        if (!vmaf_compat_entries[i].built) {
            (void)fprintf(stderr, "not in this build: %s (%s)\n", vmaf_compat_entries[i].name,
                          vmaf_compat_entries[i].condition);
            continue;
        }
        if (!vmaf_compat_calls_old[i] || !vmaf_compat_calls_new[i]) {
            (void)fprintf(stderr, "no conformance case calls %s\n", vmaf_compat_entries[i].name);
            uncovered++;
        }
    }
    for (size_t i = 0; i < VMAF_COMPAT_ENGINE_ONLY_COUNT; i++) {
        (void)fprintf(stderr,
                      "engine exception (one definition, nothing to compare): %s while %s is "
                      "built (core/api/vmafx.toml names what ends it)\n",
                      vmaf_compat_engine_only[i].name, vmaf_compat_engine_only[i].backend);
    }
    return uncovered ? "compat functions without a conformance case (see above)" : NULL;
}

static mu_message_t test_conformance(void)
{
    VmafCompatApi new_api = vmaf_compat_new;
    if (strcmp(conformance_plant(), "score") == 0) {
        real_score_at_index = new_api.score_at_index;
        new_api.score_at_index = planted_score_at_index;
    }
    mu_message_t failure = NULL;
    for (size_t i = 0; i < N_SCENARIOS; i++) {
        if (strcmp(conformance_plant(), "uncovered") == 0 &&
            strcmp(scenarios[i].name, "pictures") == 0) {
            continue;
        }
        mu_message_t message = run_scenario(i, &new_api);
        failure = failure ? failure : message;
    }
    mu_message_t coverage = check_coverage();
    return failure ? failure : coverage;
}

mu_message_t run_tests(void)
{
    mu_run_test(test_conformance);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
