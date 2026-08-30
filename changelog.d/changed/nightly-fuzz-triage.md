- **`nightly.yml` + `fuzz.yml` triage — gates stay on, bugs documented for
  follow-up.** Research-0089 (PR #525) §5 flagged that both workflows had
  0 successful runs in the last 50. Triage on 2026-05-09 confirmed both
  gates fire correctly on `schedule:` and are catching real bugs:
  `nightly.yml` ThreadSanitizer surfaces a data race in
  `div_lookup_generator` (`core/src/feature/integer_adm.h:32-38`) where
  every worker thread spawned from `vmaf_thread_pool_create` re-populates
  the static `div_lookup[65537]` table without a `pthread_once` guard;
  `fuzz.yml` `fuzz_y4m_input` surfaces a NULL-deref SEGV in
  `y4m_input_fetch_frame` (`core/tools/y4m_input.c:877`) on negative-
  width Y4M headers (reproducer `YUV4MPEG2 W-8 H4 F30:1 Ip A1:1 C422`).
  Per memory `feedback_no_test_weakening`, neither workflow is muted /
  `continue-on-error`'d / matrix-trimmed; both stay red until the
  underlying fixes land in dedicated follow-up PRs. Two new Open rows in
  [`docs/state.md`](../docs/state.md) (`T-NIGHTLY-TSAN-ADM-INIT`,
  `T-FUZZ-Y4M-NEG-WIDTH-SEGV`) pin the failing tests + reopen triggers
  so a *new* TSan / fuzz finding is immediately distinguishable from the
  two known-open bugs. Triage decision recorded in
  [ADR-0332](../docs/adr/0332-nightly-fuzz-triage-keep-gates.md). No
  workflow files modified.
