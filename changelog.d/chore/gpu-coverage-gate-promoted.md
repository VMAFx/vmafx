### Chore

- Promote `coverage-gpu` CI job from advisory to required: the two-week
  stability window (2026-05-19 → 2026-06-02) elapsed with no advisory-fail
  runs on the self-hosted `gpu-full` runner. Removed `continue-on-error: true`
  and renamed the job display name from `(Advisory)` to required. Closes
  T-GPU-COVERAGE-STABLE-WEEKS.
