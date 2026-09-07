- CAMBI on the HIP backend returned **exactly 0.0** on banding content
  that the CPU scores at 5.85. Three divergences, all now fixed: the
  twin hand-rolled the TVI-threshold bisection over an inverted
  predicate seeded from luma 0 instead of `luma_range.foot`, producing
  `tvi_for_diff = [1026, 1025, 1024, 4]` against the CPU's
  `[182, 309, 436, 563]` and collapsing the scored luma band from 564
  entries to a handful; its `filter_mode` filtered output rows 0 and
  height-1, which `cambi.c` leaves unfiltered; and its 7x7 mask box sum
  clamped out-of-frame taps to the border pixel where the CPU's
  summed-area table zero-pads them. HIP CAMBI is now bit-exact with the
  CPU (`5.8461540042` on both). The Metal twin carried the first two and
  gets the same fixes. **Any recorded HIP CAMBI score is invalid and
  must be re-measured.**
- The HIP and Metal CAMBI parity tests now use a 10-bit banding
  gradient and refuse to run against a degenerate CPU score. Their
  previous fixtures were 8-bit gradients stepping 32 code levels every
  32 columns; CAMBI only counts neighbour differences of 1..4 at default
  settings, so those fixtures scored 0.0 on the CPU too and the parity
  assertion was `0 == 0`. See ADR-1219.
