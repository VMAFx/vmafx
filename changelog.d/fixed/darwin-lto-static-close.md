- **macOS release builds no longer branch from `close()` into a feature
  extractor.** macOS declares the C library's `close()` with an assembler
  label, and in a full-LTO link (the release default) that symbol and the
  CPU extractors' `static close()` callbacks became the same symbol: the
  `close()` on the fdopen() failure paths of the output file, the CAMBI
  heatmap file and the SVM model save called an extractor's close with a
  file descriptor and crashed, and `test_adm_coverage` crashed on every macOS
  leg. The callbacks are named `close_fex`, and
  `test_libc_named_internal_functions` refuses a static C function named
  after a C library function.
