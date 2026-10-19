---
paths:
  - scripts/ci/check-dispatch-registry.sh
  - scripts/ci/tests/test-check-dispatch-registry.sh
  - scripts/ci/check-no-non-header-includes.sh
  - scripts/ci/tests/test-check-no-non-header-includes.sh
invariant: Every `vmaf_fex_*_<backend>` symbol is registered; `core/test/` translation units never include `.c` / `.cpp` sources.
area: tests
---
<!-- markdownlint-disable MD013 MD060 -->
# Core source gates: dispatch registry and test includes

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `check-dispatch-registry.sh` | `.pre-commit-config.yaml` (`check-dispatch-registry` hook), `tests-and-quality-gates.yml` (`Pre-Commit` job) | Cross-references backend symbols `vmaf_fex_*_<backend>` in `core/src/feature/<backend>/` against `feature_extractor_list[]` in `core/src/feature/feature_extractor.cpp`. Fails if any backend symbol is omitted from the registration array. Test suite: `scripts/ci/tests/test-check-dispatch-registry.sh`. |
| `check-no-non-header-includes.sh` | `.pre-commit-config.yaml` (`check-no-non-header-includes` hook), `rule-enforcement.yml` (`policy-and-audit` job) | Guards against non-header source inclusions (`.c` / `.cpp`) in unit test translation units under `core/test/` to prevent CodeQL alert `cpp/include-non-header` regressions. Enforces narrow internal headers (e.g. `core/src/libvmaf_priv.h`) and link seams for white-box test access. Maintains grandfathered allowlist for historical alerts. Test suite: `scripts/ci/tests/test-check-no-non-header-includes.sh`. |
