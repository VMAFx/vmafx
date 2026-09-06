- **The SYCL integer-ADM contrast-masking kernel read the wrong sample at the
  near edge.** The CPU rule is asymmetric — the near edge mirrors to index 1,
  the far edge clamps to the last index — and the SYCL twin clamped both, so
  row/column 0 was read twice and the mirrored sample dropped. It only
  diverges once a scale's ADM border crop collapses to 0 (band dimensions
  <= 14), which is exactly what the shipped 256x144 fixture hits at scale 3.
  `test_sycl_adm_parity` had been failing on real Intel hardware at
  `integer_adm_scale3_csf_2`: cpu=0.58175555 sycl=0.58191226, delta 1.57e-04
  against a 1e-4 gate. CUDA, HIP and Metal all already carried this fix
  (ADR-1167 / PR #1224); SYCL was the only twin that missed it. The shipped
  SYCL parity suite goes 17/1 to 18/0 on an Arc A380.
