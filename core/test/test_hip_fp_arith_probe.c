/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Host side of the HIP device probe of test_hip_fp_arith_contract
 *  (ADR-1407): loads the kernel of test_hip_fp_arith_probe.hip, which
 *  core/test/meson.build compiles with the list every HIP feature kernel
 *  gets, runs it over the operands and copies the results back.
 */

#include <errno.h>
#include <stddef.h>

#include <hip/hip_runtime_api.h>

#include "test_hip_fp_arith_probe.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

/* HSACO embedded by xxd from test_hip_fp_arith_probe.hip. */
extern const unsigned char test_hip_fp_arith_probe_hsaco[];

enum {
    PROBE_ARRAYS = 6,
    PROBE_INPUTS = 3,
    PROBE_BLOCK = 256,
};

/* Copies the three operand arrays up, runs the kernel and copies the three
 * result arrays back. `dev` holds the six arrays back to back. */
static hipError_t probe_run(hipFunction_t kernel, float *dev, const float *const in[PROBE_INPUTS],
                            float *const out[PROBE_INPUTS], size_t n)
{
    const size_t bytes = n * sizeof(float);
    float *slot[PROBE_ARRAYS];
    for (size_t i = 0; i < PROBE_ARRAYS; i++)
        slot[i] = dev + (i * n);

    hipError_t rc = hipSuccess;
    for (size_t i = 0; i < PROBE_INPUTS && rc == hipSuccess; i++)
        rc = hipMemcpy(slot[i], in[i], bytes, hipMemcpyHostToDevice);
    if (rc != hipSuccess)
        return rc;

    unsigned count = (unsigned)n;
    void *args[] = {
        (void *)&slot[0], (void *)&slot[1], (void *)&slot[2], (void *)&slot[3],
        (void *)&slot[4], (void *)&slot[5], (void *)&count,
    };
    const unsigned grid = (count + PROBE_BLOCK - 1u) / PROBE_BLOCK;
    rc = hipModuleLaunchKernel(kernel, grid, 1u, 1u, PROBE_BLOCK, 1u, 1u, 0u, NULL, args, NULL);
    if (rc == hipSuccess)
        rc = hipDeviceSynchronize();
    for (size_t i = 0; i < PROBE_INPUTS && rc == hipSuccess; i++)
        rc = hipMemcpy(out[i], slot[PROBE_INPUTS + i], bytes, hipMemcpyDeviceToHost);
    return rc;
}

int vmaf_test_hip_fp_arith(const float *a, const float *b, const float *c, size_t n, float *mad,
                           float *quot, float *root)
{
    if (a == NULL || b == NULL || c == NULL || mad == NULL || quot == NULL || root == NULL)
        return -EINVAL;
    if (n == 0)
        return 0;

    int devices = 0;
    if (hipInit(0) != hipSuccess || hipGetDeviceCount(&devices) != hipSuccess || devices < 1 ||
        hipSetDevice(0) != hipSuccess)
        return -ENODEV;

    hipModule_t module = NULL;
    hipFunction_t kernel = NULL;
    float *dev = NULL;
    hipError_t rc = hipModuleLoadData(&module, test_hip_fp_arith_probe_hsaco);
    if (rc != hipSuccess)
        return -EIO;
    rc = hipModuleGetFunction(&kernel, module, "vmaf_test_hip_fp_arith_kernel");
    if (rc == hipSuccess)
        rc = hipMalloc((void **)&dev, PROBE_ARRAYS * n * sizeof(float));
    if (rc == hipSuccess) {
        const float *const in[PROBE_INPUTS] = {a, b, c};
        float *const out[PROBE_INPUTS] = {mad, quot, root};
        rc = probe_run(kernel, dev, in, out, n);
    }
    if (dev != NULL)
        (void)hipFree(dev);
    (void)hipModuleUnload(module);
    return (rc == hipSuccess) ? 0 : -EIO;
}

/* NOLINTEND(modernize-use-nullptr) */
