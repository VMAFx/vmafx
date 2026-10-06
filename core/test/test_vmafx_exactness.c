/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Exactness class of the device twins (core/src/vmafx/exactness.c and the
 * generated table exactness_gen.c, RC4 WP5): the CPU is the reference whatever
 * the name, an exact twin, a libm-bounded twin and a plain tolerance twin each
 * report their class, an unknown name is unclassified, a short buffer is
 * truncated with the full length returned, and the rows are strictly sorted.
 *
 * Failing first: a class flipped in the generated table, an unsorted row or a
 * lookup that ignores the backend fails one of these cases.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/exactness.h"
#include "vmafx/types.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static int class_is(const char *extractor, uint32_t backend, const char *expected)
{
    char buf[64];
    size_t need = vmafx_exactness_class(extractor, backend, buf, sizeof(buf));
    return strcmp(buf, expected) == 0 && need == strlen(expected);
}

static char *test_cpu_is_reference(void)
{
    mu_assert("cpu adm", class_is("adm", VMAFX_BACKEND_CPU, "cpu-reference"));
    mu_assert("cpu with a gpu-looking name",
              class_is("adm_cuda", VMAFX_BACKEND_CPU, "cpu-reference"));
    mu_assert("cpu with no name", class_is(NULL, VMAFX_BACKEND_CPU, "cpu-reference"));
    mu_assert("cpu never has a row", vmafx_exactness_find("adm_cuda", VMAFX_BACKEND_CPU) == NULL);
    return NULL;
}

static char *test_device_classes(void)
{
    mu_assert("adm_cuda exact", class_is("adm_cuda", VMAFX_BACKEND_CUDA, "exact"));
    mu_assert("ciede_cuda libm", class_is("ciede_cuda", VMAFX_BACKEND_CUDA, "libm-bounded 1e-09"));
    mu_assert("metal psnr tolerance",
              class_is("integer_psnr_metal", VMAFX_BACKEND_METAL, "tolerance 5e-05"));
    return NULL;
}

static char *test_unclassified(void)
{
    mu_assert("unknown name", class_is("nosuch_cuda", VMAFX_BACKEND_CUDA, "unclassified"));
    mu_assert("known name on another backend",
              class_is("adm_cuda", VMAFX_BACKEND_SYCL, "unclassified"));
    mu_assert("NULL extractor", class_is(NULL, VMAFX_BACKEND_CUDA, "unclassified"));
    mu_assert("NULL extractor has no row", vmafx_exactness_find(NULL, VMAFX_BACKEND_CUDA) == NULL);
    return NULL;
}

static char *test_truncation(void)
{
    char buf[4];
    memset(buf, 'x', sizeof(buf));
    size_t need = vmafx_exactness_class("ciede_cuda", VMAFX_BACKEND_CUDA, buf, sizeof(buf));
    mu_assert("full length returned", need == strlen("libm-bounded 1e-09"));
    mu_assert("three characters and a NUL", strcmp(buf, "lib") == 0);
    need = vmafx_exactness_class("adm_cuda", VMAFX_BACKEND_CUDA, NULL, 0);
    mu_assert("size 0 measures", need == strlen("exact"));
    return NULL;
}

static char *test_rows_sorted_and_named(void)
{
    mu_assert("table not empty", vmafx_exactness_row_count > 0u);
    for (uint32_t i = 0; i < vmafx_exactness_row_count; i++) {
        const VmafxExactnessRow *row = &vmafx_exactness_rows[i];
        mu_assert("row has a backend",
                  row->backend >= VMAFX_BACKEND_CUDA && row->backend <= VMAFX_BACKEND_HIP);
        mu_assert("row has a kind",
                  row->kind >= VMAFX_EXACTNESS_EXACT && row->kind <= VMAFX_EXACTNESS_TOLERANCE);
        mu_assert("row finds itself", vmafx_exactness_find(row->extractor, row->backend) == row);
        if (i > 0u) {
            mu_assert("rows strictly sorted by extractor",
                      strcmp(vmafx_exactness_rows[i - 1u].extractor, row->extractor) < 0);
        }
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_cpu_is_reference),      MU_TEST(test_device_classes),
        MU_TEST(test_unclassified),          MU_TEST(test_truncation),
        MU_TEST(test_rows_sorted_and_named),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
