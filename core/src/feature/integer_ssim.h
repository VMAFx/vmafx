/*
Copyright 2001-2012 Xiph.Org and contributors.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * integer_ssim.h — shared type definitions for the integer SSIM feature extractor.
 *
 * The integer_ssim_moments_t struct is the shared accumulation buffer type used
 * by both the scalar path in integer_ssim.c and the SIMD backends
 * (x86/integer_ssim_avx2.h, arm64/integer_ssim_neon.h, etc.).  Defining it here
 * rather than in the x86-specific header ensures non-x86 builds (macOS arm64,
 * Windows arm64) can compile integer_ssim.c without pulling in x86 intrinsics.
 *
 * Layout invariant (ADR-0784): six consecutive int64_t fields, identical to the
 * private ssim_moments struct in integer_ssim.c — casts between the two types
 * are safe for all currently supported platforms.
 */

#ifndef FEATURE_INTEGER_SSIM_H_
#define FEATURE_INTEGER_SSIM_H_

#include <stdint.h>

typedef struct integer_ssim_moments {
    int64_t mux;
    int64_t muy;
    int64_t x2;
    int64_t xy;
    int64_t y2;
    int64_t w;
} integer_ssim_moments_t;

#endif /* FEATURE_INTEGER_SSIM_H_ */
