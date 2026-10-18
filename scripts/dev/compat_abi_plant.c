/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  LD_PRELOAD plant shim for compat ABI parity verification (ADR-3339).
 *  Intercepts vmaf_feature_score_pooled() and moves the score by one ULP.
 *  RTLD_NEXT needs _GNU_SOURCE, which the build passes as -D_GNU_SOURCE.
 */

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>

#include <libvmaf/libvmaf.h>

int vmaf_feature_score_pooled(VmafContext *vmaf, const char *feature_name,
                              enum VmafPoolingMethod pool_method, double *score, unsigned index_low,
                              unsigned index_high)
{
    static int (*real_fn)(VmafContext *, const char *, enum VmafPoolingMethod, double *, unsigned,
                          unsigned) = NULL;
    if (!real_fn) {
        *(void **)(&real_fn) = dlsym(RTLD_NEXT, "vmaf_feature_score_pooled");
    }
    if (!real_fn) {
        return -1;
    }
    const int err = real_fn(vmaf, feature_name, pool_method, score, index_low, index_high);
    if (!err && score) {
        *score = nextafter(*score, INFINITY);
    }
    return err;
}

int vmaf_score_pooled(VmafContext *vmaf, VmafModel *model, enum VmafPoolingMethod pool_method,
                      double *score, unsigned index_low, unsigned index_high)
{
    static int (*real_fn)(VmafContext *, VmafModel *, enum VmafPoolingMethod, double *, unsigned,
                          unsigned) = NULL;
    if (!real_fn) {
        *(void **)(&real_fn) = dlsym(RTLD_NEXT, "vmaf_score_pooled");
    }
    if (!real_fn) {
        return -1;
    }
    const int err = real_fn(vmaf, model, pool_method, score, index_low, index_high);
    if (!err && score) {
        *score = nextafter(*score, INFINITY);
    }
    return err;
}
