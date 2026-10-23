## llvm.sh install steps capped at 10 minutes (2026-10-23)

- Every apt step that runs `llvm.sh` (`lint-and-format.yml` Tidy Changed / Tidy Ratchet / Tidy SYCL, `fuzz.yml`, `nightly.yml` clang-tidy-full, `sanitizers.yml` asan-ubsan / tsan / fuzz-nightly) carries `timeout-minutes: 10`: llvm.sh downloads from apt.llvm.org outside the archive cache; measured max 5.7m (libvmaf-build-matrix, 142 runs), a Tidy Ratchet run timed out at the earlier 5-minute cap after 5.2m (job 120415869295).
- The job limits of those jobs rose by 5 minutes, except `clang-tidy-full` (60 already covers 20 + 1.25x 21.1m).
- **On sync**: keep the 10-minute cap on any step that calls `llvm.sh`; `python3 scripts/ci/check_job_timeout_budget.py` must stay green.
