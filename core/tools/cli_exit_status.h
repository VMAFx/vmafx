/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * Process exit status of the `vmaf` CLI.
 */

#ifndef LIBVMAF_TOOLS_CLI_EXIT_STATUS_H_
#define LIBVMAF_TOOLS_CLI_EXIT_STATUS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Map the result of a CLI run to the exit status every platform reports.
 *
 * libvmaf returns negative errno values (-EINVAL is -22). POSIX keeps the low
 * eight bits of a status returned from main() (-22 becomes 234); Windows keeps
 * all 32 (0xFFFFFFEA), which a POSIX shell or a CI harness reads as something
 * else. The documented status is the code modulo 256 as a non-negative int, so
 * it is formed here and `main()` never returns a negative value. A non-zero
 * code whose low byte is zero (-256) would read as success and is reported as 1.
 *
 * @param err 0 for success, otherwise a libvmaf error code or a CLI exit code.
 * @return 0 when err is 0, otherwise a value in [1, 255].
 */
static inline int vmaf_cli_exit_status(int err)
{
    if (err == 0)
        return 0;
    const int status = (int)((unsigned int)err & 0xFFU);
    return status != 0 ? status : 1;
}

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_TOOLS_CLI_EXIT_STATUS_H_ */
