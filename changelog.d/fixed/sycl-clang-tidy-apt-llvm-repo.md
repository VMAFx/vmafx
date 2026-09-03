- **`Clang-Tidy SYCL (Changed Files, Advisory)` could never run, so SYCL
  clang-tidy coverage had silently been zero since the LLVM 22 bump.** The
  job installed `clang-tidy-22` with a plain `apt-get install` from the stock
  Ubuntu 24.04 archive, which tops out at `clang-tidy-18`; the step aborted
  with `E: Unable to locate package clang-tidy-22`. Its two sibling jobs
  (`Clang-Tidy (Changed C/C++ Files)` and `Clang-Tidy Ratchet (Whole Tree)`)
  already add the `apt.llvm.org` archive via `llvm.sh 22` first; the SYCL job
  was missed when the version was bumped in #1161 / #1200. The outage hid
  itself because the job is gated on `steps.detect.outputs.files != ''`: a PR
  touching no SYCL file skips every step and the job reports success, so the
  run history read as mostly green. Across the last 11 runs the install step
  completed **zero** times — 7 green no-ops, 2 red runs with real work, and
  no SYCL file ever linted. The job now adds the LLVM archive exactly as its
  siblings do.
