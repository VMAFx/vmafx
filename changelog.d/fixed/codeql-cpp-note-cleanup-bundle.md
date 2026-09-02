Resolved 28 CodeQL C++ note-level alerts across 18 files:
- **cpp/declaration-hides-parameter** (8 alerts): renamed shadowing loop/buffer
  variables in `adm_avx2.c`, `adm_avx512.c`, `integer_adm.c`, `feature_collector.c`.
- **cpp/unused-static-function** (2 alerts): removed dead `threaded_enqueue_one` +
  associated `ThreadData` struct from `libvmaf.c`; removed disabled test function from
  `test_score_pooled_eagain.c`.
- **cpp/lossy-function-result-cast** (2 alerts): corrected `extract_channel` return
  type in `speed.c`; added explicit `(int)` casts on `mirror()` call sites in
  `vif_tools.c`.
- **cpp/constant-comparison** (1 alert): removed always-true lower-bound guard in
  `pdjson.c` `utf8_seq_length`.
- **cpp/equality-on-floats** (14 alerts): all intentional sentinel/idempotency/parity
  checks; added inline explanatory comments; no epsilon introduced.
- **cpp/loop-variable-changed** (1 actionable alert): documented intentional
  double-increment pattern in `mkdirp.c`.
