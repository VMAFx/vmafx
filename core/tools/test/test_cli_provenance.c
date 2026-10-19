/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * The provenance record in the vmaf CLI (#2142, ADR-2073, RC4 WP5): which
 * arguments are output options (left out of the record, so a re-run that
 * writes elsewhere records the same command line), the command line as
 * `cli_argv` annotations of a real context, and `--verify-provenance` refusing
 * a report without a command line or with a failed re-run. The end-to-end
 * verification of real reports is core/tools/test/test_vmaf_verify_provenance.py.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cli_provenance.h"
#include "libvmaf/libvmaf.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

typedef struct ArgCase {
    const char *arg;
    bool output;
    bool takes_value;
} ArgCase;

static const ArgCase arg_cases[] = {
    {"-o", true, true},
    {"-oreport.json", true, false},
    {"--output", true, true},
    {"--output=report.json", true, false},
    {"--xml", true, false},
    {"--json", true, false},
    {"--csv", true, false},
    {"--sub", true, false},
    {"--quiet", true, false},
    {"-q", true, false},
    {"--provenance-sidecar", true, false},
    {"--verify-provenance", true, true},
    {"--verify-provenance=r.json", true, false},
    {"--reference", false, false},
    {"-r", false, false},
    {"--outputs", false, false},
    {"--json5", false, false},
    {"-q2", false, false},
    {"--precision", false, false},
    {"report.json", false, false},
};

static char *test_output_arguments(void)
{
    for (size_t i = 0; i < sizeof(arg_cases) / sizeof(arg_cases[0]); i++) {
        bool takes_value = !arg_cases[i].takes_value;
        const bool output = cli_arg_is_output(arg_cases[i].arg, &takes_value);
        if (output != arg_cases[i].output || takes_value != arg_cases[i].takes_value) {
            (void)fprintf(stderr, "argument %s: output %d value %d\n", arg_cases[i].arg,
                          (int)output, (int)takes_value);
        }
        mu_assert("output option classified wrong", output == arg_cases[i].output);
        mu_assert("value of the option classified wrong", takes_value == arg_cases[i].takes_value);
    }
    bool takes_value = true;
    mu_assert("NULL is an output option", !cli_arg_is_output(NULL, &takes_value) && !takes_value);
    return NULL;
}

static const char *const expected_argv[] = {"-r",       "ref.yuv",     "-d",
                                            "dist.yuv", "--precision", "max"};

/* Annotation `i` of `context` is cli_argv = expected_argv[i]. */
static bool annotation_is_argv(const VmafxContext *context, uint32_t i)
{
    VmafxAnnotation annotation = VMAFX_ANNOTATION_INIT;
    return vmafx_context_annotation(context, i, &annotation, NULL) == VMAFX_OK &&
           strcmp(annotation.key, "cli_argv") == 0 &&
           strcmp(annotation.value, expected_argv[i]) == 0;
}

static bool annotations_are_argv(VmafContext *vmaf)
{
    const VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    VmafxProvenance record = VMAFX_PROVENANCE_INIT;
    const size_t n = sizeof(expected_argv) / sizeof(expected_argv[0]);
    bool ok =
        vmafx_context_provenance(context, &record, NULL) == VMAFX_OK && record.n_annotations == n;
    for (uint32_t i = 0; ok && i < record.n_annotations; i++) {
        ok = annotation_is_argv(context, i);
    }
    return ok;
}

static char *test_annotations_leave_out_output(void)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);
    char *const argv[] = {"vmaf",
                          "-r",
                          "ref.yuv",
                          "--output",
                          "out.json",
                          "--json",
                          "-d",
                          "dist.yuv",
                          "-q",
                          "--precision",
                          "max",
                          "-oalt.json",
                          "--provenance-sidecar"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const bool annotated = cli_annotate_run(vmaf, argc, argv) == 0;
    const bool recorded = annotated && annotations_are_argv(vmaf);
    const bool closed = vmaf_close(vmaf) == 0;
    mu_assert("annotation failed", annotated);
    mu_assert("the record holds the scoring arguments only", recorded);
    mu_assert("vmaf_close failed", closed);
    mu_assert("a handle without a context annotated", cli_annotate_run(NULL, argc, argv) != 0);
    return NULL;
}

static int rerun_calls;

static int failing_rerun(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    (void)user;
    rerun_calls++;
    return 3;
}

/* A JSON report of a context with no command line recorded. */
static char *write_report(const char *path, bool annotate)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);
    char *const argv[] = {"vmaf", "-r", "ref.yuv"};
    if (annotate) {
        mu_assert("annotation failed", cli_annotate_run(vmaf, 3, argv) == 0);
    }
    mu_assert("report", cli_write_report(vmaf, path, VMAF_OUTPUT_FORMAT_JSON, NULL, false) == 0);
    mu_assert("vmaf_close failed", vmaf_close(vmaf) == 0);
    return NULL;
}

static char *test_verify_refuses_without_command_line(void)
{
    const char *const path = "test_cli_provenance_no_argv.json";
    char *const failed = write_report(path, false);
    if (failed) {
        return failed;
    }
    rerun_calls = 0;
    mu_assert("a report without cli_argv verified",
              cli_verify_provenance(path, "vmaf", failing_rerun, NULL) == 2);
    mu_assert("re-ran without a command line", rerun_calls == 0);
    (void)remove(path);
    return NULL;
}

static char *test_verify_reports_failed_rerun(void)
{
    const char *const path = "test_cli_provenance_rerun_fails.json";
    char *const failed = write_report(path, true);
    if (failed) {
        return failed;
    }
    rerun_calls = 0;
    mu_assert("a failed re-run verified",
              cli_verify_provenance(path, "vmaf", failing_rerun, NULL) == 2);
    mu_assert("no re-run", rerun_calls == 1);
    mu_assert("a missing report verified",
              cli_verify_provenance("no/such/report.json", "vmaf", failing_rerun, NULL) == 2);
    (void)remove(path);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_output_arguments),
        MU_TEST(test_annotations_leave_out_output),
        MU_TEST(test_verify_refuses_without_command_line),
        MU_TEST(test_verify_reports_failed_rerun),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
