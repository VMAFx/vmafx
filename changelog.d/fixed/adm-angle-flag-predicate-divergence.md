Collapse the four divergent integer-ADM `angle_flag` predicates onto one
shared definition in `core/src/feature/adm_angle_flag.h`. The CPU, AVX2 and
AVX-512 paths narrow the int64 operands to `float` and compare in `double` —
the form the Netflix golden gate freezes — while CUDA/HIP scale 0 compared
the exact int64 products in `double`, SYCL did the whole comparison in
`float`, and Metal narrowed the exact products to `float`. The four disagreed
on roughly 4e-5 of near-parallel scale-0 band quadruples, flipping the
enhancement-gain-limited branch of `decouple()` and moving GPU `adm` scores.
CUDA and HIP now call the frozen expression directly; SYCL and Metal — neither
of which can execute a binary64 instruction — call a bit-identical 64-bit
integer reformulation of it. CPU scores are unchanged (the compiled
`integer_adm.c` is byte-identical to before).
