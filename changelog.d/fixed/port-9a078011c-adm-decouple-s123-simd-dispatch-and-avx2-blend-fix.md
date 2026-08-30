## ADM: enable adm_decouple_s123 SIMD + fix AVX2 kh_msb blend (port 9a078011c)

Uncomments `s->adm_decouple_s123 = adm_decouple_s123_avx2/avx512` in
`integer_adm.c::init()`. Fixes the AVX2 kh/kv/kd MSB blend selecting
`const_32768` instead of `abs_oh_epi32` for the small-value branch,
causing small float-feature drift on 10-bit content. AVX-512 gains a
matching `kh_shift_epi32` zero-clear.
