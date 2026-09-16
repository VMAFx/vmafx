
- **Eight false `doubleFree` reports from the POSIX cppcheck model.**
  cppcheck's `posix.cfg` lists `fdopen` as an unconditional deallocator of the
  descriptor, so 2.13.0 — the version CI installs from the Ubuntu 24.04
  archive — reads the POSIX-mandated `close()` after a *failed* `fdopen()` as
  a second free. 2.21.1 does not report it. Each of the eight sites now
  carries a cited inline suppression explaining the model's limitation, and
  the three sites whose `if` had no braces gained them, because a comment
  between an unbraced `if` and its statement makes clang-tidy's
  `readability-braces-around-statements` fire.
