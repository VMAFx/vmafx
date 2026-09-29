/**
 *
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef IQA_DECIMATE_DIM_H_INCLUDED
#define IQA_DECIMATE_DIM_H_INCLUDED

/**
 * @brief Size of one dimension after iqa_decimate() by @p factor.
 *
 * An odd dimension keeps one extra sample whatever the factor (upstream
 * tdistler rule). Kept apart from decimate.h, and free of includes, so the
 * SYCL float_ssim twin can size its device planes with the CPU's own rule
 * without parsing the C-only iqa kernel declarations as C++ (ADR-1370).
 */
static inline int iqa_decimate_dim(int n, int factor)
{
    return n / factor + (n & 1);
}

#endif /* IQA_DECIMATE_DIM_H_INCLUDED */
