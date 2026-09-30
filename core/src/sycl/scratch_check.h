/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Scratch memory in SYCL kernels (ADR-1395).
 *
 *  A kernel uses scratch memory when IGC places a private array in memory
 *  (kernel_device_specific::private_mem_size) or spills registers
 *  (ext::intel spill_memory_size). Under the Linux xe kernel driver an Arc
 *  A-series (DG2) GPU returns wrong values from such kernels, so the SYCL
 *  kernels are kept free of it. This header serves three consumers:
 *
 *    - vmaf_sycl_scratch_selftest(): run by vmaf_sycl_state_init once per
 *      device; logs a warning when the device shows the defect.
 *    - vmaf_sycl_kernel_scratch_audit(): lists every registered kernel's
 *      scratch sizes; test_sycl_kernel_scratch compares them with the
 *      ratchet list core/src/sycl/scratch_ratchet.txt.
 *    - vmaf_sycl_scratch_extractors(): the extractors the ratchet list names,
 *      quoted by the self-test's warning.
 */
#ifndef LIBVMAF_SYCL_SCRATCH_CHECK_H_
#define LIBVMAF_SYCL_SCRATCH_CHECK_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Scratch memory one kernel needs on the audited device. The header
 * declares no typedefs so the C and the C++ view read alike; use
 * `struct VmafSyclKernelScratch`. */
struct VmafSyclKernelScratch {
    const char *name;     /**< SYCL kernel id name, as the ratchet list stores it */
    const char *pretty;   /**< demangled name, or @p name when it does not demangle */
    size_t private_bytes; /**< kernel_device_specific::private_mem_size */
    size_t spill_bytes;   /**< spill_memory_size; 0 where the device does not report it */
};

/**
 * Build every SYCL kernel registered in this program for the default GPU
 * (the device libvmaf picks without a device index) and pass each one's
 * scratch sizes to @p fn, whose strings live until it returns. Kernels the
 * device cannot run are skipped, and so are the two self-test probes, which
 * use scratch on purpose.
 *
 * @return number of kernels reported, -ENODEV when there is no GPU,
 *         -ENOSYS when the toolchain has no kernel-bundle support,
 *         -EIO when the SYCL runtime throws.
 */
int vmaf_sycl_kernel_scratch_audit(void (*fn)(const struct VmafSyclKernelScratch *kernel,
                                              void *user),
                                   void *user);

/** Outcome of the two scratch probes on one device. */
struct VmafSyclScratchProbe {
    unsigned work_items;    /**< work-items each probe checks */
    size_t private_bytes;   /**< private_mem_size of the private-array probe */
    unsigned private_wrong; /**< private-array probe work-items with a wrong result */
    int spill_ran;          /**< 1 when the device ran the SIMD-32 spill probe */
    size_t spill_bytes;     /**< spill_memory_size of the spill probe; 0 if not reported */
    unsigned spill_wrong;   /**< spill probe work-items with a wrong result */
};

/**
 * Run the private-array probe and, on devices with SIMD-32 sub-groups, the
 * register-spill probe on @p queue's device. @p queue is a `sycl::queue *`.
 *
 * @return 0 on success, -EINVAL on a NULL argument, -ENOSYS when the
 *         toolchain has no kernel-bundle support, -EIO when the runtime throws.
 */
int vmaf_sycl_scratch_probe(void *queue, struct VmafSyclScratchProbe *out);

/**
 * Warning-only self-test: run the probes on @p queue's device (once per
 * device per process) and log a warning naming the affected extractors when
 * a probe returns wrong values. Never fails and never refuses the device.
 * VMAF_SYCL_SCRATCH_SELFTEST=0 skips it. @p queue is a `sycl::queue *`.
 */
void vmaf_sycl_scratch_selftest(void *queue);

/**
 * Comma-separated names of the SYCL extractors that run at least one kernel
 * on core/src/sycl/scratch_ratchet.txt. test_sycl_kernel_scratch checks that
 * this list and the ratchet file name the same extractors.
 */
const char *vmaf_sycl_scratch_extractors(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_SYCL_SCRATCH_CHECK_H_ */
