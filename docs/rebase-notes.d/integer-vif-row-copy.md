## Netflix/vmaf 9f4bd165f: integer_vif copies each row's samples (2026-10-09)

- `core/src/feature/integer_vif.c` `extract()`: the same change as upstream
  (`row_bytes = w << (bpc > 8)`), with a comment. **On sync**: identical; take
  either side.
- `core/test/test_integer_vif_row_copy.c` (fork-only): wide-stride luma ending
  at an inaccessible page; scores equal at either stride.
- `4068ee3b5` (CAMBI 10-bit same-size copy with stride) is not ported: the fork
  copies row by row since `T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05`
  (`decimate_same_size_16b()` in `core/src/feature/cambi.c`), and the Rust twin
  (`core/src/rust/feature/cambi/src/preprocess.rs`) does too. On sync, keep the
  fork's function.
