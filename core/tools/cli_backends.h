/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * `vmaf --list-backends`: the scoring backends this binary was built with and
 * which of them initialise on this host (ADR-1874). The tools that pick a
 * backend for a run (vmaf-tune, vmafx-tune) read this report instead of
 * guessing from the help text or from vendor tools.
 */

#ifndef LIBVMAF_TOOLS_CLI_BACKENDS_H_
#define LIBVMAF_TOOLS_CLI_BACKENDS_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write the backend report as one JSON document to @p out.
 *
 * Every backend the CLI knows is listed in the order cpu, cuda, sycl, hip,
 * metal, with `compiled` (built into this binary) and `usable` (its state
 * initialises on the default device here; cpu always). A compiled backend
 * that failed to initialise also carries `init_status`, the negative errno
 * its state initialiser returned.
 *
 * @return 0 on success, -EINVAL when @p out is NULL, -EIO when writing fails.
 */
int cli_list_backends(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_TOOLS_CLI_BACKENDS_H_ */
