- `docs/state.md` no longer carries five bug ids twice. A keep-both rebase
  resolution had left `T-CUDA-INIT-SUBMIT-LEAKS-2026-06-19` and
  `T-UPSTREAM-1564-ADM-CM-GPU-BORDER-AND-ROUNDING-2026-09-03` in **both** "Open
  bugs" and "Recently closed" (so two fixed bugs read as open), plus three
  duplicated rows inside "Recently closed". `scripts/ci/check-state-md-rows.sh`
  now rejects any id that appears more than once; it runs in pre-commit, in
  `make lint-sh` and in the Rules workflow, with a hermetic self-test.
