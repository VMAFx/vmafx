/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Netflix#1300 / ADR-0157 — loop-reducer that does 10x init/preallocate/
 *  fetch/close cycles and confirms no framework-side memory leak.
 *  Pre-fix this test leaked ~N×(VmafCudaState struct + CudaFunctions table
 *  + pthread mutex state + picture pool internals) per cycle. Post-fix
 *  every cycle's allocations are fully freed before the next iteration.
 */

#include <stdio.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/model.h"

enum {
    LEAK_REDUCER_CYCLES = 10,
    LEAK_REDUCER_FRAMES = 10,
    /* Sentinel returned by run_one_cycle() to mean "no CUDA driver, skip
     * the reducer." Distinct from any errno-code value so the caller can
     * branch without ambiguity. */
    LEAK_REDUCER_SKIP = 1,
};

static int setup_cycle(VmafContext **out_vmaf, VmafCudaState **out_cu_state, VmafModel **out_model)
{
    *out_vmaf = NULL;
    *out_cu_state = NULL;
    *out_model = NULL;

    VmafConfiguration vmaf_cfg = {0};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, vmaf_cfg);
    if (err || !vmaf)
        return err ? err : -1;

    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err || !cu_state) {
        (void)vmaf_close(vmaf);
        return LEAK_REDUCER_SKIP;
    }

    err = vmaf_cuda_import_state(vmaf, cu_state);
    if (err) {
        *out_vmaf = vmaf;
        *out_cu_state = cu_state;
        return err;
    }

    VmafCudaPictureConfiguration cuda_pic_cfg = {
        .pic_params =
            {
                .w = 1920,
                .h = 1080,
                .bpc = 8,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_prealloc_method = VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE,
    };
    err = vmaf_cuda_preallocate_pictures(vmaf, cuda_pic_cfg);
    if (err) {
        *out_vmaf = vmaf;
        *out_cu_state = cu_state;
        return err;
    }

    VmafModelConfig model_cfg = {0};
    VmafModel *model = NULL;
    err = vmaf_model_load(&model, &model_cfg, "vmaf_v0.6.1");
    if (err || !model) {
        *out_vmaf = vmaf;
        *out_cu_state = cu_state;
        return err ? err : -1;
    }

    err = vmaf_use_features_from_model(vmaf, model);
    *out_vmaf = vmaf;
    *out_cu_state = cu_state;
    *out_model = model;
    return err;
}

static int drive_frames(VmafContext *vmaf)
{
    int err = 0;
    for (unsigned i = 0; i < LEAK_REDUCER_FRAMES; i++) {
        VmafPicture ref = {0};
        VmafPicture dist = {0};
        err = vmaf_cuda_fetch_preallocated_picture(vmaf, &ref);
        if (err)
            return err;
        err = vmaf_cuda_fetch_preallocated_picture(vmaf, &dist);
        if (err)
            return err;
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int run_one_cycle(void)
{
    VmafContext *vmaf = NULL;
    VmafCudaState *cu_state = NULL;
    VmafModel *model = NULL;

    int err = setup_cycle(&vmaf, &cu_state, &model);
    if (err == LEAK_REDUCER_SKIP)
        return LEAK_REDUCER_SKIP;

    if (!err && vmaf && model)
        err = drive_frames(vmaf);

    /* Full cleanup on every cycle — this is the point of the reducer:
     * pre-fix, each of these teardown paths leaked something; post-fix,
     * the whole cycle is tidy and ASan reports zero framework-side
     * leaked bytes across N iterations. */
    int close_err = vmaf ? vmaf_close(vmaf) : 0;
    int state_err = vmaf_cuda_state_free(cu_state);
    if (model)
        vmaf_model_destroy(model);
    if (!err)
        err = close_err ? close_err : state_err;
    return err;
}

static char *test_cuda_preallocation_leak_reducer(void)
{
    /* Use cycle #0 as the GPU-runtime probe: if the first cycle reports
     * SKIP, every subsequent cycle would skip for the same reason, so
     * there is no leak coverage to gain. If it reports an error, fail
     * the test. Otherwise, run the remaining cycles and require each
     * one to complete cleanly. */
    int first = run_one_cycle();
    if (first == LEAK_REDUCER_SKIP) {
        (void)fprintf(stderr, "[skip: no CUDA runtime] ");
        return NULL;
    }
    mu_assert("first cycle failed during leak reducer", !first);

    for (unsigned cycle = 1; cycle < LEAK_REDUCER_CYCLES; cycle++) {
        int err = run_one_cycle();
        mu_assert("cycle failed during leak reducer", !err);
    }

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cuda_preallocation_leak_reducer);
    return NULL;
}
