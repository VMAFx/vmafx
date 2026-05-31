### Added

- **Tests — coverage push above ADR-0922 ratchet floors.** Added 31 targeted
  unit tests against the four security-critical paths in `core/src/dnn/` and
  `core/src/read_json_model.c` that the new 90 % per-file critical floor (PR
  #469) put under-water. After the change, unit-test-only measurement shows
  `read_json_model.c` at 92.00 % (was 88.00 %, floor 90 %), `model_loader.c`
  at 90.00 % (was 87.20 %, floor 90 %), and overall coverage delta neutral.
  The `ort_backend.c` floor (78.9 % vs 83 % floor) remains a structural
  ceiling on CPU-only ORT builds and needs either ORT-API mock injection or
  a working CUDA / OpenVINO EP on the CI runner — flagged in the PR for
  follow-up. No production code changed.
