// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

// Pointer arithmetic with a computed offset and no stated proof that the offset is in
// bounds. HISS-09's axiom is "unsafe pointer arithmetic and memory dereferencing
// require explicit rationale"; C has the arithmetic but no `unsafe` block to anchor a
// matcher, and no scanner implements a C HISS-09 check.
unsigned char at(const unsigned char *base, unsigned long offset)
{
    return *(base + offset);
}
