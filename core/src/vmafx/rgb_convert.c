/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The statement of an RGB import (RC4 WP13, ADR-2146): which matrix, range and
 * transfer a descriptor must state, and the refusals naming the field when it
 * does not. The conversion itself is rgb_convert.h.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/rgb_convert.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

/* Names by enumerator value (VmafxColorMatrix, VmafxColorTransfer of vmafx/frame.h). */
static const char *const matrix_names[] = {"UNKNOWN", "BT709", "BT2020_NCL",
                                           "ICTCP",   "BT601", "BT2020_CL"};
static const char *const transfer_names[] = {"UNKNOWN", "BT709", "SMPTE2084",
                                             "SRGB",    "HLG",   "LINEAR"};

static const char *const statement =
    "an RGB frame is converted to Y'CbCr with a matrix, range and transfer you state; none "
    "is assumed";

static VmafxStatus refuse_unstated(const VmafxReport *report, const VmafxImportLayout *layout,
                                   const char *field, const char *values)
{
    return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, field,
                      "pixel format %s: %s; set %s (%s)", layout->name, statement, field, values);
}

static VmafxStatus check_matrix(const VmafxReport *report, const VmafxFrameImport *d,
                                const VmafxImportLayout *layout)
{
    if (d->rgb_matrix == VMAFX_COLOR_MATRIX_UNKNOWN) {
        return refuse_unstated(report, layout, "desc.rgb_matrix", "BT601, BT709 or BT2020_NCL");
    }
    if (d->rgb_matrix > VMAFX_COLOR_MATRIX_BT2020_CL) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.rgb_matrix",
                          "%u is not a VmafxColorMatrix", (unsigned)d->rgb_matrix);
    }
    if (d->rgb_matrix == VMAFX_COLOR_MATRIX_BT2020_CL ||
        d->rgb_matrix == VMAFX_COLOR_MATRIX_ICTCP) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.rgb_matrix",
                          "pixel format %s, matrix %s: the integer conversion covers BT601, "
                          "BT709 and BT2020_NCL; this matrix needs the transfer function in "
                          "the conversion",
                          layout->name, matrix_names[d->rgb_matrix]);
    }
    return VMAFX_OK;
}

static VmafxStatus check_range(const VmafxReport *report, uint32_t range, const char *field,
                               const VmafxImportLayout *layout)
{
    if (range == VMAFX_COLOR_RANGE_UNKNOWN) {
        return refuse_unstated(report, layout, field, "LIMITED or FULL");
    }
    if (range > VMAFX_COLOR_RANGE_FULL) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, field,
                          "%u is not a VmafxColorRange", (unsigned)range);
    }
    return VMAFX_OK;
}

static VmafxStatus check_transfer(const VmafxReport *report, const VmafxFrameImport *d,
                                  const VmafxImportLayout *layout)
{
    if (d->rgb_transfer == VMAFX_COLOR_TRC_UNKNOWN) {
        return refuse_unstated(report, layout, "desc.rgb_transfer",
                               "BT709, SRGB, SMPTE2084 or HLG");
    }
    if (d->rgb_transfer > VMAFX_COLOR_TRC_LINEAR) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.rgb_transfer",
                          "%u is not a VmafxColorTransfer", (unsigned)d->rgb_transfer);
    }
    if (d->rgb_transfer == VMAFX_COLOR_TRC_LINEAR) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.rgb_transfer",
                          "pixel format %s, transfer %s: the matrix applies to non-linear "
                          "R'G'B'; encode the samples first",
                          layout->name, transfer_names[d->rgb_transfer]);
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_rgb_check_statement(const VmafxReport *report, const VmafxFrameImport *d,
                                      const VmafxImportLayout *layout)
{
    if (!layout->needs_statement) {
        return VMAFX_OK;
    }
    VmafxStatus status = check_matrix(report, d, layout);
    if (status == VMAFX_OK) {
        status = check_range(report, d->rgb_range, "desc.rgb_range", layout);
    }
    if (status == VMAFX_OK) {
        status = check_transfer(report, d, layout);
    }
    if (status == VMAFX_OK) {
        status = check_range(report, d->rgb_out_range, "desc.rgb_out_range", layout);
    }
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
