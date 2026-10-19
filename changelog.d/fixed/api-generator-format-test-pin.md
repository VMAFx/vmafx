- **The API generator's format test compares against the pinned
  clang-format.** It ran whichever clang-format was installed, and the hosted
  Linux image's 18.1.3 formats the generated `*_INIT` macros differently from
  the 23.1.2 the repository pins, so the test failed on every hosted leg. It
  now uses only the pinned major (`VMAFX_CLANG_FORMAT` names one explicitly)
  and skips, naming the version it found, otherwise; the Tooling Tests job
  installs the pinned release.
