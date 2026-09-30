/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The inputs on which the device-resident SpEED pipelines' fp32-pair log2
 *  (speed_log2() in cuda/speed/speed_score.cu and sycl/speed_sycl_pipeline.cpp)
 *  does not round to nearest, with the correctly rounded result (ADR-1380,
 *  Research-1379).
 *
 *  The pair arithmetic carries about 2^-45 of relative error, and 48 positive
 *  finite floats have an exact log2 closer than that to a rounding boundary:
 *  mantissa 0x1.aa932c at exponents -32..-17 and 16..31, mantissa 0x1.ff800c at
 *  -16..-9 and 8..15. An exhaustive replay of every positive finite float
 *  against quad-precision log2 finds no other. Both twins look the input up
 *  only when its fraction field is one of the two, so the common path costs a
 *  compare.
 *
 *  Plain initializer lists, so the CUDA (__constant__) and SYCL (constexpr)
 *  tables and the replay harness read one set of numbers (HISS-19).
 */

#ifndef VMAF_SRC_FEATURE_SPEED_LOG2_HARD_CASES_H_
#define VMAF_SRC_FEATURE_SPEED_LOG2_HARD_CASES_H_

#define SPEED_LOG2_HARD_CASES 48
#define SPEED_LOG2_HARD_FRACTION_A 0x00554996u /* mantissa 0x1.aa932c */
#define SPEED_LOG2_HARD_FRACTION_B 0x007fc006u /* mantissa 0x1.ff800c */

/* Input bit patterns, ascending. */
#define SPEED_LOG2_HARD_INPUTS                                                                     \
    {0x2fd54996u, 0x30554996u, 0x30d54996u, 0x31554996u, 0x31d54996u, 0x32554996u, 0x32d54996u,    \
     0x33554996u, 0x33d54996u, 0x34554996u, 0x34d54996u, 0x35554996u, 0x35d54996u, 0x36554996u,    \
     0x36d54996u, 0x37554996u, 0x37ffc006u, 0x387fc006u, 0x38ffc006u, 0x397fc006u, 0x39ffc006u,    \
     0x3a7fc006u, 0x3affc006u, 0x3b7fc006u, 0x43ffc006u, 0x447fc006u, 0x44ffc006u, 0x457fc006u,    \
     0x45ffc006u, 0x467fc006u, 0x46ffc006u, 0x477fc006u, 0x47d54996u, 0x48554996u, 0x48d54996u,    \
     0x49554996u, 0x49d54996u, 0x4a554996u, 0x4ad54996u, 0x4b554996u, 0x4bd54996u, 0x4c554996u,    \
     0x4cd54996u, 0x4d554996u, 0x4dd54996u, 0x4e554996u, 0x4ed54996u, 0x4f554996u}

/* Bit pattern of the correctly rounded log2 of the input at the same index. */
#define SPEED_LOG2_HARD_OUTPUTS                                                                    \
    {0xc1fa1b55u, 0xc1f21b55u, 0xc1ea1b55u, 0xc1e21b55u, 0xc1da1b55u, 0xc1d21b55u, 0xc1ca1b55u,    \
     0xc1c21b55u, 0xc1ba1b55u, 0xc1b21b55u, 0xc1aa1b55u, 0xc1a21b55u, 0xc19a1b55u, 0xc1921b55u,    \
     0xc18a1b55u, 0xc1821b55u, 0xc17005c5u, 0xc16005c5u, 0xc15005c5u, 0xc14005c5u, 0xc13005c5u,    \
     0xc12005c5u, 0xc11005c5u, 0xc10005c5u, 0x410ffa3bu, 0x411ffa3bu, 0x412ffa3bu, 0x413ffa3bu,    \
     0x414ffa3bu, 0x415ffa3bu, 0x416ffa3bu, 0x417ffa3bu, 0x4185e4abu, 0x418de4abu, 0x4195e4abu,    \
     0x419de4abu, 0x41a5e4abu, 0x41ade4abu, 0x41b5e4abu, 0x41bde4abu, 0x41c5e4abu, 0x41cde4abu,    \
     0x41d5e4abu, 0x41dde4abu, 0x41e5e4abu, 0x41ede4abu, 0x41f5e4abu, 0x41fde4abu}

#endif /* VMAF_SRC_FEATURE_SPEED_LOG2_HARD_CASES_H_ */
