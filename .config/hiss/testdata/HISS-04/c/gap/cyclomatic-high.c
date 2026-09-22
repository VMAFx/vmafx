// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

/* Cyclomatic complexity 16 in 49 lines: over the HISS-04 cap of 10 and under the length
 * cap, which is the only thing the scanner measures for C. The .clang-tidy template
 * enables clang-diagnostic-*, clang-analyzer-*, bugprone-*, performance-* and
 * portability-* -- not readability-function-size or
 * readability-function-cognitive-complexity -- and no clang-tidy step exists in the
 * Makefile, lefthook or CI. */
int branchy(int count)
{
    if (count > 0) {
        count++;
    }
    if (count > 1) {
        count++;
    }
    if (count > 2) {
        count++;
    }
    if (count > 3) {
        count++;
    }
    if (count > 4) {
        count++;
    }
    if (count > 5) {
        count++;
    }
    if (count > 6) {
        count++;
    }
    if (count > 7) {
        count++;
    }
    if (count > 8) {
        count++;
    }
    if (count > 9) {
        count++;
    }
    if (count > 10) {
        count++;
    }
    if (count > 11) {
        count++;
    }
    if (count > 12) {
        count++;
    }
    if (count > 13) {
        count++;
    }
    if (count > 14) {
        count++;
    }
    return count;
}
