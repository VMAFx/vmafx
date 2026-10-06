/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * The provenance record in the vmaf CLI (#2142, ADR-2073, RC4 WP5): the run's
 * command line as annotations of its record, the report through
 * vmafx_report_write() (the library embeds the record, the backend receipt
 * and the score format; the CLI no longer edits the file), and
 * `--verify-provenance`. A C translation unit, so the C++ CLI sources never
 * include the generated C headers of the VMAFx API (requests/WP1-5).
 */

#ifndef LIBVMAF_TOOLS_CLI_PROVENANCE_H_
#define LIBVMAF_TOOLS_CLI_PROVENANCE_H_

#include <stdbool.h>

#include "libvmaf/libvmaf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * True when @p arg (one argv element) is an option of where or how the report
 * is written, not of what is scored: -o/--output, --xml, --json, --csv,
 * --sub, --provenance-sidecar, -q/--quiet, --verify-provenance.
 * @p takes_value is set when the next argv element is its value.
 */
bool cli_arg_is_output(const char *arg, bool *takes_value);

/**
 * Record argv[1..argc) without the output options (cli_arg_is_output()) as
 * `cli_argv` annotations, in order, of the VMAFx context @p vmaf is bound to.
 * 0, or -1 with the reason on stderr.
 */
int cli_annotate_run(VmafContext *vmaf, int argc, char *const *argv);

/**
 * Write the report of @p vmaf to @p path in @p fmt with vmafx_report_write();
 * @p sidecar also writes `<path>.provenance.json`. 0, or -1 with the reason
 * on stderr.
 */
int cli_write_report(VmafContext *vmaf, const char *path, enum VmafOutputFormat fmt,
                     const char *score_format, bool sidecar);

/** Runs the vmaf CLI on @p argv (argv[0] the program); returns its exit status. */
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no `using`. ADR-1138. */
typedef int (*CliRerun)(int argc, char **argv, void *user);
/* NOLINTEND(modernize-use-using) */

/**
 * `--verify-provenance`: re-run the `cli_argv` annotations of the JSON report
 * at @p report through @p rerun, writing `<report>.rerun.json`, and compare
 * the two with vmafx_report_verify(). Prints the verdict; returns 0 when the
 * re-run matches (the re-run report is then removed), 1 on a difference
 * (named on stderr; the re-run report is kept), 2 when the check could not
 * run.
 */
int cli_verify_provenance(const char *report, const char *argv0, CliRerun rerun, void *user);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_TOOLS_CLI_PROVENANCE_H_ */
