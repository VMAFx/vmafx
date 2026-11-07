/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  vulkan_probe.h — device self-check probe for the opt-in Vulkan
 *  integer compute path (ADR-3402, Phase 0 of the PoC
 *  `poc/vk-integer-adm` @ `0bb98ee21`, ported in-tree).
 *
 *  Four checks, one compute shader (probe.comp), op selected by push
 *  constant:
 *    op0  u32 x u32 -> u64 full product (umulExtended) vs host golden
 *    op1  u64 add-reduce chain with explicit carries vs host golden
 *    op2  fp32 contraction: `precise` (NoContraction) vs the host's
 *         separate mul+add reference — 0 violations is the pass
 *         criterion (ADR-0264; measured on NVIDIA 621.257 and Intel
 *         0.101.7092, #2466 comment 6689054122)
 *    op3  dispatch sentinel: every invocation writes a distinct value,
 *         so a kernel that never ran cannot be mistaken for a passing
 *         one (the ADR-0176 errata's bogus-"ULP=0" lesson)
 *
 *  Device selection is by vendorID/deviceID identity, never by ordinal
 *  (research-0090's inverted-attribution lesson; the ADR-0176 errata).
 *  `vkCreateDevice` requests ONLY features the device reports — the
 *  Intel driver SIGSEGVs when asked for an unreported feature (#2466
 *  comment 6689054122).
 *
 *  Reachability: the probe is test-reachable
 *  (core/test/test_vulkan_probe.c drives it); a user-facing
 *  `--vulkan-probe` CLI flag is an open item (#3319 open questions,
 *  item 9) because it is user-visible surface with codegen
 *  (ADR-1852) + changelog obligations.
 */

#ifndef LIBVMAF_VULKAN_PROBE_H_
#define LIBVMAF_VULKAN_PROBE_H_

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probe request. Zero-initialise for defaults (auto device selection,
 * 4096 elements, report to stderr). */
typedef struct {
    /* Requested device identity. 0/0 = auto (first discrete, else first
     * compute-capable). A non-zero pair must match exactly. */
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t count; /* elements per check (multiple of 64), 0 -> 4096 */
    /* Output stream for the JSON report; NULL -> stderr. */
    FILE *out;
} VmafVulkanProbeParams;

typedef struct {
    int err; /* 0 pass; -errno-style on failure (see below) */

    /* Adapters seen during enumeration (identity + name), for the
     * "requested identity is not selectable" diagnostic. */
    unsigned adapters_seen;
    char adapters[8][128];

    /* The adapter the probe attached to (0/0 when none). */
    uint32_t vendor_id;
    uint32_t device_id;
    char device_name[256];

    /* Per-check verdicts: 1 pass, 0 fail, -1 skipped
     * (capability unavailable on this device). */
    int check_umul32;
    int check_u64_reduce;
    int check_contraction;
    int check_sentinel;

    /* op2 detail counts (NoContraction audit). */
    unsigned contraction_violations; /* `precise` != host mul+add */
    unsigned contraction_unclassified;
} VmafVulkanProbeResult;

/* Return codes:
 *   0          every enabled check passed on the requested identity
 *   -ENODEV    no Vulkan loader / no compute-capable adapter, or the
 *              requested identity is not selectable (result->adapters[]
 *              names what was seen)
 *   -ENOTSUP   a check failed (verdict rows say which)
 *   -EINVAL    bad params
 */
int vmaf_vulkan_probe_run(const VmafVulkanProbeParams *params, VmafVulkanProbeResult *result);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_VULKAN_PROBE_H_ */
