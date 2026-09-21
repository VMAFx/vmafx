// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// Negative fixture for HISS-04/c: a function exactly at the 60-line cap must
// not be reported. The Praetor native check is brace-tracked, so its count
// runs from the signature line through the closing brace inclusive. Measured
// boundary (praetorctl 7c0f803d40ee): a span of 60 is clean, 61 is reported.
// `exactly_sixty` below spans lines 11-70 = 60. Do not add a line here; the
// over-cap side is pinned by ../positive/loc-func-over-cap.c.

int exactly_sixty(int count)
{
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    count++;
    return count;
}
