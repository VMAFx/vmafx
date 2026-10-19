/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * The provenance record in the vmaf CLI (#2142, ADR-2073, RC4 WP5); see
 * cli_provenance.h.
 */

#include "cli_provenance.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/path_utf8.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define ANNOTATION_KEY "cli_argv"
#define ANNOTATIONS_MAX 256u
#define PATH_MAX_LEN 4096u
#define RERUN_SUFFIX ".rerun.json"

typedef struct OutputOption {
    const char *name; /* "--output", "-o" */
    bool value;       /* takes a value */
} OutputOption;

static const OutputOption output_options[] = {
    {"--output", true},
    {"-o", true},
    {"--xml", false},
    {"--json", false},
    {"--csv", false},
    {"--sub", false},
    {"--quiet", false},
    {"-q", false},
    {"--provenance-sidecar", false},
    {"--verify-provenance", true},
};

bool cli_arg_is_output(const char *arg, bool *takes_value)
{
    *takes_value = false;
    for (size_t i = 0; arg && i < sizeof(output_options) / sizeof(output_options[0]); i++) {
        const OutputOption *const option = &output_options[i];
        const size_t len = strlen(option->name);
        if (strncmp(arg, option->name, len) != 0) {
            continue;
        }
        const char next = arg[len];
        const bool long_name = option->name[1] == '-';
        /* "--output=x", "-ox" carry the value; "--output", "-o" take the next. */
        if (next == '\0' || (option->value && (long_name ? next == '=' : true))) {
            *takes_value = option->value && next == '\0';
            return true;
        }
    }
    return false;
}

static void print_error(const char *what, VmafxError *error)
{
    (void)fprintf(stderr, "vmaf: %s: %s: %s\n", what, vmafx_error_subject(error),
                  vmafx_error_message(error));
    vmafx_error_free(error);
}

int cli_annotate_run(VmafContext *vmaf, int argc, char *const *argv)
{
    assert(argc <= 0 || argv);
    VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    if (!context) {
        (void)fprintf(stderr, "vmaf: the context has no provenance record\n");
        return -1;
    }
    /* An output option and the value it takes stay out of the record. */
    bool skip_value = false;
    for (int i = 1; i < argc; i++) {
        if (skip_value) {
            skip_value = false;
            continue;
        }
        bool takes_value = false;
        if (cli_arg_is_output(argv[i], &takes_value)) {
            skip_value = takes_value;
            continue;
        }
        VmafxError *error = NULL;
        if (vmafx_context_annotate(context, ANNOTATION_KEY, argv[i], &error) != VMAFX_OK) {
            print_error("cannot record the command line", error);
            return -1;
        }
    }
    return 0;
}

int cli_write_report(VmafContext *vmaf, const char *path, enum VmafOutputFormat fmt,
                     const char *score_format, bool sidecar)
{
    VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    if (!context) {
        (void)fprintf(stderr, "vmaf: the context has no provenance record\n");
        return -1;
    }
    VmafxError *error = NULL;
    const uint32_t flags = sidecar ? (uint32_t)VMAFX_REPORT_PROVENANCE_SIDECAR : 0u;
    if (vmafx_report_write(context, path, (uint32_t)fmt, flags, score_format, &error) != VMAFX_OK) {
        print_error("problem writing the report", error);
        return -1;
    }
    return 0;
}

/* ---- --verify-provenance ---------------------------------------------------------- */

typedef struct Rerun {
    char *argv[ANNOTATIONS_MAX + 8u];
    int argc;
} Rerun;

static void rerun_free(Rerun *rerun)
{
    for (int i = 0; i < rerun->argc; i++) {
        free(rerun->argv[i]);
    }
    rerun->argc = 0;
}

static bool rerun_push(Rerun *rerun, const char *arg)
{
    if ((size_t)rerun->argc + 1u >= sizeof(rerun->argv) / sizeof(rerun->argv[0])) {
        return false;
    }
    const size_t len = strlen(arg);
    char *const copy = malloc(len + 1u);
    if (!copy) {
        return false;
    }
    memcpy(copy, arg, len + 1u);
    rerun->argv[rerun->argc++] = copy;
    rerun->argv[rerun->argc] = NULL;
    return true;
}

/* argv0, the recorded `cli_argv` values, then the re-run's output. */
static int rerun_arguments(const VmafxReportFile *recorded, const char *argv0, const char *output,
                           Rerun *rerun)
{
    bool ok = rerun_push(rerun, argv0);
    int recorded_args = 0;
    for (unsigned i = 0; ok && i < ANNOTATIONS_MAX; i++) {
        char path[64];
        (void)snprintf(path, sizeof(path), "provenance.annotations[%u].key", i);
        const char *const key = vmafx_report_field(recorded, path);
        if (!key) {
            break;
        }
        (void)snprintf(path, sizeof(path), "provenance.annotations[%u].value", i);
        const char *const value = vmafx_report_field(recorded, path);
        if (strcmp(key, ANNOTATION_KEY) == 0 && value) {
            ok = rerun_push(rerun, value);
            recorded_args++;
        }
    }
    ok = ok && rerun_push(rerun, "--output") && rerun_push(rerun, output) &&
         rerun_push(rerun, "--json") && rerun_push(rerun, "--quiet");
    if (!ok) {
        return -1;
    }
    return recorded_args > 0 ? 0 : 1;
}

static int open_report(const char *path, VmafxReportFile **file)
{
    VmafxError *error = NULL;
    if (vmafx_report_open(path, file, &error) != VMAFX_OK) {
        print_error("cannot read the report", error);
        return -1;
    }
    return 0;
}

/* The re-run and its report; 0, or 2 (could not run). */
static int run_again(const VmafxReportFile *recorded, const char *argv0, const char *output,
                     CliRerun rerun, void *user)
{
    Rerun args = {.argc = 0};
    const int built = rerun_arguments(recorded, argv0, output, &args);
    if (built != 0) {
        (void)fprintf(stderr, "vmaf: %s\n",
                      built > 0 ? "the report records no command line (cli_argv annotations)" :
                                  "cannot build the re-run's command line");
        rerun_free(&args);
        return 2;
    }
    const int status = rerun(args.argc, args.argv, user);
    rerun_free(&args);
    if (status != 0) {
        (void)fprintf(stderr, "vmaf: the re-run of the recorded configuration failed (%d)\n",
                      status);
        return 2;
    }
    return 0;
}

static int compare_reports(const VmafxReportFile *recorded, const char *output)
{
    VmafxReportFile *again = NULL;
    if (open_report(output, &again) != 0) {
        return 2;
    }
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_report_verify(recorded, again, &error);
    vmafx_report_close(again);
    if (status == VMAFX_OK) {
        (void)printf("provenance verified: every configuration field and score of the re-run "
                     "is identical\n");
        return 0;
    }
    const bool differs = status == VMAFX_E_MISMATCH;
    (void)fprintf(stderr, "vmaf: provenance %s: %s: %s\n", differs ? "mismatch" : "check failed",
                  vmafx_error_subject(error), vmafx_error_message(error));
    vmafx_error_free(error);
    if (differs) {
        (void)fprintf(stderr, "vmaf: the re-run report is kept: %s\n", output);
    }
    return differs ? 1 : 2;
}

int cli_verify_provenance(const char *report, const char *argv0, CliRerun rerun, void *user)
{
    assert(report && argv0 && rerun);
    char output[PATH_MAX_LEN];
    const int n = snprintf(output, sizeof(output), "%s%s", report, RERUN_SUFFIX);
    if (n < 0 || (size_t)n >= sizeof(output)) {
        (void)fprintf(stderr, "vmaf: the report path is too long\n");
        return 2;
    }
    VmafxReportFile *recorded = NULL;
    if (open_report(report, &recorded) != 0) {
        return 2;
    }
    int result = run_again(recorded, argv0, output, rerun, user);
    if (result == 0) {
        result = compare_reports(recorded, output);
    }
    vmafx_report_close(recorded);
    if (result == 0) {
        (void)vmaf_remove_utf8(output);
    }
    return result;
}

/* NOLINTEND(modernize-use-nullptr) */
