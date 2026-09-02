Fix the AVX2 ADM direct-LUT-range `__builtin_clz()` UBSan path exposed by
`test_score_pooled_eagain`, then retire that test's sanitizer deselect so
ASan+LSan, UBSan, and TSan run it again.
