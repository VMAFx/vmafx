- chore(ci): aggressively ratchet the Coverage Gate floors and add a
  per-PR coverage-delta gate. `scripts/ci/coverage-check.sh` now enforces
  70 % overall (was 37 %; recovery raised beyond the proposed 60 % to
  match measured coverage after #420/#412) and 90 % critical (was 85 %); every
  `PER_FILE_MIN` override tightens by +5pp (`ort_backend.c` /
  `dnn_api.c` 78 → 83, `tiny_extractor_template.h` 10 → 15). The new
  `scripts/ci/coverage-delta-check.sh` runs on pull-request events in the
  Coverage Gate job and fails any PR that drops overall coverage by more
  than 0.5pp or drops any touched file by more than 0.5pp vs the
  merge-base. PRs opened before 2026-05-31 get a 30-day grace window
  (through 2026-06-30); thereafter the new floors and delta gate apply
  uniformly. Floor changes are one-way: loosening requires a follow-up
  ADR superseding ADR-0922. See
  [ADR-0922](../docs/adr/0922-coverage-ratchet-aggressive.md).
