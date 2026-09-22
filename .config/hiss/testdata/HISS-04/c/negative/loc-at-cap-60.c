// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// Negative fixture for HISS-04/c: a function exactly at the 60-line cap must
// not be reported. The Praetor native check is brace-tracked — its span runs
// from the opening brace line through the closing brace line, inclusive, and
// the signature lines above the brace are not counted. Measured boundary
// (praetorctl 7ec6f6ca5e28): a span of 60 is clean, 61 is reported.
// `exactly_sixty` below spans lines 22-81 (opening brace to closing brace) =
// 60. Do not add or remove a line inside it: one more turns this negative
// fixture into a false positive, one fewer stops it pinning the boundary. The
// over-cap side is pinned by ../positive/loc-func-over-cap.c.
//
// The two enforcers differ by exactly one line. clang-tidy's
// readability-function-size (LineThreshold: 60, the enforcer docs/principles.md
// names) counts closing-brace line minus opening-brace line, so it is clean at
// a span of 61 and reports at 62 — measured with clang-tidy 22.1.8 against this
// repository's .clang-tidy. A function Praetor rejects can therefore still be
// clean under clang-tidy; the stricter of the two is the one that gates.

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
    count++;
    return count;
}
