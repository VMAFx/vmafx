/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Provenance device fields for a libvmaf context that imports a VmafCudaState
 * via vmaf_cuda_import_state() and is wrapped with vmafx_context_from_libvmaf()
 * (#3259, CLI --backend cuda --json path).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

#ifndef VMAFX_TEST_ARCH
#define VMAFX_TEST_ARCH ""
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static char *check_cpu_provenance(VmafxContext *context)
{
    VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
    mu_assert("provenance", vmafx_context_provenance(context, &rec, NULL) == VMAFX_OK);
    mu_assert("cpu active_backend", rec.active_backend == VMAFX_BACKEND_CPU);
    mu_assert("cpu device_index", rec.device_index == 0);
    mu_assert("cpu device_name", rec.device_name && strcmp(rec.device_name, "cpu") == 0);
    mu_assert("cpu device_runtime",
              rec.device_runtime && strcmp(rec.device_runtime, VMAFX_TEST_ARCH) == 0);
    return NULL;
}

static char *check_cpu_provenance_json(VmafxContext *context)
{
    const char *json = NULL;
    mu_assert("provenance json",
              vmafx_context_provenance_json(context, 0, &json, NULL) == VMAFX_OK);
    mu_assert("json cpu backend", json && strstr(json, "\"active_backend\":\"cpu\"") != NULL);
    mu_assert("json cpu index", strstr(json, "\"device_index\":0") != NULL);
    mu_assert("json cpu name", strstr(json, "\"device_name\":\"cpu\"") != NULL);
    return NULL;
}

static char *test_libvmaf_without_cuda_state_unchanged(void)
{
    VmafContext *vmaf = NULL;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init", vmaf_init(&vmaf, cfg) == 0 && vmaf != NULL);
    VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    mu_assert("vmafx_context_from_libvmaf", context != NULL);
    char *failed = check_cpu_provenance(context);
    if (!failed) {
        failed = check_cpu_provenance_json(context);
    }
    (void)vmaf_close(vmaf);
    return failed;
}

static char *test_no_device_backend_unknown_unchanged(void)
{
    VmafxContext *context = NULL;
    mu_assert("vmafx_context_create", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    char *failed = check_cpu_provenance(context);
    if (!failed) {
        failed = check_cpu_provenance_json(context);
    }
    (void)vmafx_context_destroy(context, NULL);
    return failed;
}

static char *check_cuda_provenance_matches_device0(const VmafxProvenance *rec)
{
    VmafxDeviceInfo dev_info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("vmafx_device_info",
              vmafx_device_info(VMAFX_BACKEND_CUDA, 0, &dev_info, NULL) == VMAFX_OK);
    mu_assert("device_index matches ordinal 0", rec->device_index == dev_info.index);
    mu_assert("device_name matches device 0", strcmp(rec->device_name, dev_info.name) == 0);
    mu_assert("device_runtime matches device 0",
              strcmp(rec->device_runtime, dev_info.runtime) == 0);
    return NULL;
}

static char *check_imported_cuda_provenance(VmafxContext *context)
{
    VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
    mu_assert("provenance", vmafx_context_provenance(context, &rec, NULL) == VMAFX_OK);
    mu_assert("cuda active_backend", rec.active_backend == VMAFX_BACKEND_CUDA);
    mu_assert("cuda device_index >= 0", rec.device_index >= 0);
    mu_assert("cuda device_name not fallback", rec.device_name && rec.device_name[0] != '\0' &&
                                                   strcmp(rec.device_name, "cuda") != 0);
    mu_assert("cuda device_runtime starts with cuda driver",
              rec.device_runtime && strncmp(rec.device_runtime, "cuda driver ", 12) == 0);
    return check_cuda_provenance_matches_device0(&rec);
}

static char *check_imported_cuda_provenance_json(VmafxContext *context)
{
    const char *json = NULL;
    mu_assert("provenance json",
              vmafx_context_provenance_json(context, 0, &json, NULL) == VMAFX_OK);
    mu_assert("json active_backend cuda",
              json && strstr(json, "\"active_backend\":\"cuda\"") != NULL);
    mu_assert("json device_index not -1", strstr(json, "\"device_index\":-1") == NULL);
    mu_assert("json device_name not cuda", strstr(json, "\"device_name\":\"cuda\"") == NULL);
    mu_assert("json device_runtime cuda driver",
              strstr(json, "\"device_runtime\":\"cuda driver ") != NULL);
    return NULL;
}

static char *test_imported_cuda_state_provenance(void)
{
    const VmafCudaConfiguration cu_cfg = {0};
    VmafCudaState *cu_state = NULL;
    if (vmaf_cuda_state_init(&cu_state, cu_cfg) != 0 || !cu_state) {
        (void)fprintf(stderr, "SKIP: test_vmafx_provenance_cuda — no CUDA device available\n");
        mu_skipped = 1;
        return NULL;
    }
    VmafContext *vmaf = NULL;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    if (vmaf_init(&vmaf, cfg) != 0 || !vmaf) {
        (void)vmaf_cuda_state_free(cu_state);
        mu_assert("vmaf_init", false);
    }
    const int import_err = vmaf_cuda_import_state(vmaf, cu_state);
    VmafxContext *const context = import_err == 0 ? vmafx_context_from_libvmaf(vmaf) : NULL;
    char *failed = context ? check_imported_cuda_provenance(context) : NULL;
    if (context && !failed) {
        failed = check_imported_cuda_provenance_json(context);
    }
    (void)vmaf_close(vmaf);
    (void)vmaf_cuda_state_free(cu_state);
    mu_assert("vmaf_cuda_import_state", import_err == 0);
    mu_assert("vmafx_context_from_libvmaf", context != NULL);
    return failed;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_libvmaf_without_cuda_state_unchanged),
        MU_TEST(test_no_device_backend_unknown_unchanged),
        MU_TEST(test_imported_cuda_state_provenance),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
