- **test(svm):** Add `test_svm_multiclass` — 17-class and 32-class C_SVC fixtures
  plus a 17-class NU_SVC fixture to exercise the `max_nr_class=16→32` realloc
  doubling in `svm_group_classes()` and `svm_check_parameter()`.  Under
  ASan/UBSan any regression to the double-free fixed in PR #708 aborts
  immediately.  The 2-class fixture in `test_svm_api.c` never crossed the
  16-class threshold so the fixed path had zero coverage.  (ADR-1066)
- **ci(dev):** Add `scripts/ci/check-compose-dri-writable.sh` — a bash lint gate
  that asserts every `/dev/dri` bind-mount in `dev/docker-compose.yml` has
  `read_only: false`.  Wired into `dev-container-build.yml` as a pre-build
  step to prevent re-introduction of the bug fixed by PR #707 (ADR-0529,
  ADR-1066).
