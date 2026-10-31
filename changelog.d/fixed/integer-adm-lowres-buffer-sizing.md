- **Integer ADM sizes detail sub-band working buffers for the low-resolution scale.**
  `init_buffers()` in `integer_adm.c` previously allocated full scale-0 buffers (`buf_sz_one`)
  for all 18 scale 1..3 integer detail bands. Sizing them with `buf_sz_s1` based on
  `rows_s1 = ((h + 1) / 2 + 1) / 2 + 2` reduces working memory allocation by approximately
  half for these bands while maintaining sufficient rows for multi-scale DWT decomposition (port of
  Netflix/vmaf 4611e47a7).
