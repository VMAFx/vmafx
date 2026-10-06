/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Exactness class of every device twin (RC4 WP5 provenance). The rows are
 * generated into exactness_gen.c from the parity gate's data
 * (scripts/codegen/vmafx_exactness.py); this header is internal and not
 * exported.
 */

#ifndef VMAFX_EXACTNESS_H
#define VMAFX_EXACTNESS_H

#include <stddef.h>
#include <stdint.h>

enum { VMAFX_EXACTNESS_EXACT = 1, VMAFX_EXACTNESS_LIBM_BOUNDED = 2, VMAFX_EXACTNESS_TOLERANCE = 3 };

typedef struct VmafxExactnessRow {
    const char *extractor;     /* registered twin name: "adm_cuda", "integer_ssim_cuda" */
    const char *cpu_extractor; /* "adm" */
    uint32_t backend;          /* VmafxBackend: CUDA 1, SYCL 2, METAL 3, HIP 4 (vmafx/types.h) */
    uint32_t kind;             /* VMAFX_EXACTNESS_* */
    const char *bound;         /* "0", "1e-09", "5e-05" */
    const char *adr;           /* "ADR-1416", "" */
    const char *text;          /* "exact", "libm-bounded 1e-09", "tolerance 5e-05" */
} VmafxExactnessRow;

extern const VmafxExactnessRow vmafx_exactness_rows[];
extern const uint32_t vmafx_exactness_row_count;

/*
 * Class text of `extractor` running on `backend` (a VmafxBackend value), with
 * snprintf semantics into buf/size (buf may be NULL when size is 0). Returns
 * the length the full text needs. "cpu-reference" for VMAFX_BACKEND_CPU (0),
 * whatever the name; for a device backend the row whose extractor and backend
 * match: "exact", "libm-bounded <bound>" or "tolerance <bound>"; otherwise
 * "unclassified".
 */
size_t vmafx_exactness_class(const char *extractor, uint32_t backend, char *buf, size_t size);

/* The class text with static storage: "cpu-reference" for the CPU, the row's
 * text, or "unclassified" when no row matches (RC4 WP5 provenance record). */
const char *vmafx_exactness_text(const char *extractor, uint32_t backend);

/* The matching row, or NULL (always NULL for the CPU). */
const VmafxExactnessRow *vmafx_exactness_find(const char *extractor, uint32_t backend);

#endif /* VMAFX_EXACTNESS_H */
