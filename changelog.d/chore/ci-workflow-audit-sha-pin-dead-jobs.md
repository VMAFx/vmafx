### Chore

- SHA-pin `EnricoMi/publish-unit-test-result-action` in `e2e-k8s.yml` (was
  `@v2`; pinned to `c950f6fb` / v2.23.0), making it consistent with every
  other action in the repository.
- Remove permanently-disabled `clang-tidy-sycl` job from `lint-and-format.yml`
  (~150 lines of dead YAML that could never run due to `if: (false)`); add a
  short tombstone comment explaining the toolchain blocker and referencing
  ADR-0217.
- Correct stale `fuzz.yml` comment that cited ADR-0882 and a
  `fuzz_json_model` harness that do not yet exist; replace with a forward
  note so the gap is visible rather than silently wrong.
