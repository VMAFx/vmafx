- **The assertion-density gate now scans C and C++ files that sit directly under `core/src/`, and the `core/tools` subdirectories.**
  The `git ls-files` pathspecs lacked `:(glob)`, so `core/src/**/*.c` skipped all 31 files directly under `core/src/` (and
  `core/tools/*.c` pulled in subdirectories by accident). The nine functions over 20 lines in four of the skipped files
  that had no `assert()` carry one now, and the harness plants an assert-less function directly under `core/src/`, which
  fails on the old pathspec and is reported on the new one.
