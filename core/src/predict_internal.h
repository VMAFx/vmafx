/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#ifndef VMAF_SRC_PREDICT_INTERNAL_H_
#define VMAF_SRC_PREDICT_INTERNAL_H_

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "model.h"

static inline int find_linear_function_parameters(VmafPoint p1, VmafPoint p2, double *alpha,
                                                  double *beta)
{

    if (!(p1.x <= p2.x && p1.y <= p2.y))
        return -EINVAL; // first_point coordinates need to be smaller or equal to second_point coordinates

    if (p2.x - p1.x == 0 || p2.y - p1.y == 0) {
        if (!(p1.x == p2.x && p1.y == p2.y))
            return -EINVAL; // first_point and second_point cannot lie on a horizontal or vertical line
        *alpha = 1.0;       // both points are the same
        *beta = 0.0;
    } else if (p1.x == 0) {
        *beta = p1.y;
        *alpha = (p2.y - *beta) / p2.x;
    } else {
        *alpha = (p2.y - p1.y) / (p2.x - p1.x);
        *beta = p1.y - (p1.x * (*alpha));
    }

    return 0;
}

static inline int piecewise_segment_apply(double x, VmafPoint *knots, unsigned idx, unsigned n_seg,
                                          double *y)
{
    /* Errno values are positive; libvmaf's convention is to return their
     * negation so callers can distinguish them from a successful 0.  Returning
     * positive EINVAL here surfaced as a truthy error to local callers but
     * inverted the sign on any caller that propagated `err` upward (e.g.
     * vmaf_predict_score_at_index downstream).  Adversarial audit 2026-05-31,
     * fix/core-lifecycle-memory-audit. */
    if (!(knots[idx].x < knots[idx + 1].x && knots[idx].y <= knots[idx + 1].y))
        return -EINVAL;

    const bool cond0 = knots[idx].x <= x;
    const bool cond1 = x <= knots[idx + 1].x;

    if (knots[idx].y == knots[idx + 1].y) { // the segment is horizontal
        if (cond0 && cond1)
            *y = knots[idx].y;
        if (idx == 0 && x < knots[idx].x)
            *y = knots[idx].y;
        if (idx == n_seg - 1 && x > knots[idx + 1].x)
            *y = knots[idx].y;
        return 0;
    }

    double slope = 0.0;
    double offset = 0.0;
    /* Unreachable failure for a well-ordered, non-horizontal segment (the
     * guard above already enforces x strictly increasing and y
     * non-decreasing), but propagate it rather than silently mapping onto
     * the zero line. CERT ERR33-C / Power-of-10 rule 7. */
    const int err = find_linear_function_parameters(knots[idx], knots[idx + 1], &slope, &offset);
    if (err)
        return err;

    if (cond0 && cond1)
        *y = slope * x + offset;
    if (idx == 0 && x < knots[idx].x)
        *y = slope * x + offset;
    if (idx == n_seg - 1 && x > knots[idx + 1].x)
        *y = slope * x + offset;
    return 0;
}

static inline int piecewise_linear_mapping(double x, VmafPoint *knots, unsigned n_knots, double *y)
{
    /* See piecewise_segment_apply: -EINVAL not +EINVAL. */
    if (n_knots <= 1)
        return -EINVAL;
    /* Every ordered comparison against NaN is false. Without this guard no
     * segment writes `y`, yet the function reports success with the plausible
     * zero assigned below -- laundering a failed model computation into a
     * valid score (Issue #1526). Keep the caller's output untouched on error. */
    if (!isfinite(x))
        return -EINVAL;
    unsigned n_seg = n_knots - 1;

    *y = 0.0;

    // construct the function
    for (unsigned idx = 0; idx < n_seg; idx++) {
        int err = piecewise_segment_apply(x, knots, idx, n_seg, y);
        if (err)
            return err;
    }

    return 0;
}

static inline bool float_values_equal(double a, double b)
{
    if (isnan(a) || isnan(b))
        return false;
    if (a == 0.0 && b == 0.0)
        return true;
    uint64_t a_bits = 0;
    uint64_t b_bits = 0;
    memcpy(&a_bits, &a, sizeof(a_bits));
    memcpy(&b_bits, &b, sizeof(b_bits));
    return a_bits == b_bits;
}

#endif /* VMAF_SRC_PREDICT_INTERNAL_H_ */
